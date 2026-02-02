#pragma once
//---------------------------------------------------------------------------
#include "Config.hpp"
#include "infra/BloomFilter.hpp"
#include "infra/ProbeTiming.hpp"
#include "infra/QueryMemory.hpp"
#include "op/OpBase.hpp"
#include "op/TargetBase.hpp"
#include "query/DataSource.hpp"

#include <atomic>
#include <string>
#include <vector>
//---------------------------------------------------------------------------
namespace engine {
//---------------------------------------------------------------------------
/// Page size for B+ tree nodes (matches DataSource::PAGE_SIZE)
static constexpr size_t BTREE_PAGE_SIZE = DataSource::PAGE_SIZE;

/// B+ tree built bottom-up from sorted data with bloom filter
class Hashtable {
    public:
    /// A hashtable entry consists of a next pointer and the tuple itself
    struct Entry {
        Entry* next;
        uint64_t tuple[];
    };

    /// page header for both leaf and internal pages
    struct PageHeader {
        uint16_t is_leaf : 1;
        uint16_t num_keys : 15;
        uint16_t reserved;
        // 0xFFFFFFFF means no next page
        // otherwise, it is the index of the next page in leaf_pages_
        uint32_t next_page_idx;
    };
    static_assert(sizeof(PageHeader) == 8);

    /// leaf page: [header][keys][entries]
    struct LeafPage {
        PageHeader header;
        // remaining space is BTREE_PAGE_SIZE - sizeof(PageHeader) = 8184 bytes
        static constexpr size_t MAX_ENTRIES =
            (BTREE_PAGE_SIZE - sizeof(PageHeader)) / (sizeof(uint64_t) + sizeof(Entry*));

        uint64_t keys[MAX_ENTRIES];
        Entry* entries[MAX_ENTRIES];

        [[gnu::always_inline]] inline uint64_t getKey(size_t idx) const {
            return keys[idx];
        }
        [[gnu::always_inline]] inline Entry* getEntry(size_t idx) const {
            return entries[idx];
        }
    };
    static_assert(sizeof(LeafPage) <= BTREE_PAGE_SIZE);

    /// inner page: [header][keys][children]
    struct InternalPage {
        PageHeader header;
        static constexpr size_t MAX_KEYS =
            (BTREE_PAGE_SIZE - sizeof(PageHeader) - sizeof(void*)) / (sizeof(uint64_t) + sizeof(void*));

        uint64_t keys[MAX_KEYS];
        void* children[MAX_KEYS + 1];
        [[gnu::always_inline]] inline uint64_t getKey(size_t idx) const {
            return keys[idx];
        }
    };
    static_assert(sizeof(InternalPage) <= BTREE_PAGE_SIZE);

#if defined(__AVX512F__)
#define _AVX_JOINFILTER
#endif

#ifdef AVX_JOINFILTER
    using hash_type = uint32_t;
    static constexpr std::pair<hash_type, uint32_t> computeHashes(uint32_t key) {
        return {key * 0x85ebca6b, key * 0xc2b2ae35};
    }
#else
    using hash_type = uint64_t;
    static constexpr std::pair<hash_type, uint32_t> computeHashes(uint32_t key) {
        auto val = key * 11400714819323198485llu;
        return {val, static_cast<uint32_t>(val)};
    }
#endif
    static constexpr size_t hashSize = sizeof(hash_type);
    static constexpr size_t hashBits = hashSize * 8;

    public:
    /// Root page (leaf or internal)
    void* root_ = nullptr;
    /// Number of tuples
    size_t num_tuples = 0;
    /// Number of unique keys
    size_t num_keys = 0;
    /// Tree height (1 = just leaves)
    size_t height_ = 0;
    /// Are we certainly duplicate free?
    bool is_certainly_duplicate_free = true;
    /// Bloom filter
    BloomFilter bloom_filter_;
    /// leaf pages vector for sibling traversal
    Vector<LeafPage*> leaf_pages_;

    friend struct HashtableBuild;
    friend struct HashtableProbe;

    /// The pretty name for debugging
    std::string pretty;

    /// The first index is next pointer, the second index is multiplicity
    static constexpr size_t key_offset = config::handleMultiplicity ? 1 : 0;
    /// Get the hash table size
    [[nodiscard]] size_t htSize() const { return leaf_pages_.size(); }
    /// Get the number of tuples
    [[nodiscard]] size_t getNumTuples() const { return num_tuples; }
    /// Get the number of keys
    [[nodiscard]] size_t getNumKeysEstimate() const { return num_keys; }
    /// Is the hash table empty?
    [[nodiscard]] bool isEmpty() const { return num_tuples == 0; }
    /// Is the hash table duplicate free?
    [[nodiscard]] bool isDuplicateFree() const {
        return is_certainly_duplicate_free;
    }

    /// Join filter using bloom filter (may have false positives)
    [[gnu::always_inline]] inline bool joinFilter(uint64_t key) const {
        return bloom_filter_.mayContain(key);
    }
    /// Precise join filter, actually searches tree (no false positives)
    [[gnu::always_inline]] inline bool joinFilterPrecise(uint64_t key) const {
        if (!bloom_filter_.mayContain(key)) return false;
        if (!root_) return false;

        // traverse to leaf
        void* current = root_;
        for (size_t level = height_; level > 1; --level) {
            auto* internal = static_cast<InternalPage*>(current);
            size_t lo = 0, hi = internal->header.num_keys;
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (internal->keys[mid] < key)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            current = internal->children[lo];
        }

        // binary search
        auto* leaf = static_cast<LeafPage*>(current);
        size_t lo = 0, hi = leaf->header.num_keys;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (leaf->keys[mid] < key)
                lo = mid + 1;
            else
                hi = mid;
        }

        return lo < leaf->header.num_keys && leaf->keys[lo] == key;
    }

    /// Allocate the hashtable (placeholder for compatibility)
    void allocateHashtable(size_t num_elements);

    /// Eq restrictions
    struct EqRestriction {
        unsigned offset;
        uint32_t value;
    };
    /// Filter the table with eq restrictions
    void filterEq(const EqRestriction& restrictions);

    /// Iterate over all keys found in the tree
    template <typename CallbackT>
    void iterateAll(CallbackT&& callback) {
        for (auto* leaf : leaf_pages_) {
            for (size_t i = 0; i < leaf->header.num_keys; ++i) {
                callback(leaf->entries[i]->tuple[key_offset]);
            }
        }
    }
};
//---------------------------------------------------------------------------
/// Build sub operator - collects tuples, sorts, builds tree bottom-up
struct HashtableBuild : public TargetImpl<HashtableBuild> {
    /// Buffer entry for sorting
    struct BufferEntry {
        uint64_t key;
        Hashtable::Entry* entry;
    };

    /// The maximum number of partitions shift
    static constexpr size_t maxPartitionsShift = 7;
    /// The maximum number of partitions
    static constexpr size_t maxPartitions = 1ull << maxPartitionsShift;

    /// Chunk for tuple allocation
    struct Chunk;
    /// Block for chunk allocation
    struct Block;

    /// Local state - per-thread collection
    struct LocalState {
        /// Number of collected tuples
        size_t num_tuples = 0;
        /// Buffer for entries
        Vector<BufferEntry> buffer;
        /// Current chunk for entry allocation
        Chunk* current_chunk = nullptr;
        /// Current position in chunk
        uint64_t* chunk_pos = nullptr;
        uint64_t* chunk_end = nullptr;
        /// Linked list of blocks
        Block* blocks = nullptr;
        /// Attribute count
        size_t attr_count = 0;
        /// Next local state
        LocalState* next = nullptr;

        explicit LocalState(HashtableBuild& build);

        /// allocate new entry
        Hashtable::Entry* allocateEntry(size_t attr_count);
    };

    /// The hashtable
    Hashtable& ht;
    /// Bloom filter bits (0 = auto)
    size_t bloom_filter_bits_;
    /// References to the local states
    std::atomic<LocalState*> local_state_refs = nullptr;
    /// Should we build a cross product table or a normal table?
    bool is_cross_product = false;

    /// Add tuple to tuple materialization
    template <typename... AttrT>
    [[gnu::always_inline]] void operator()(LocalState& ls, uint64_t multiplicity, uint64_t key, AttrT... attrs) {
        constexpr size_t attr_count = sizeof...(AttrT) + 1 + (config::handleMultiplicity ? 1 : 0);

        // Allocate entry
        auto* entry = ls.allocateEntry(attr_count);
        entry->next = nullptr;

        // Store tuple data
        size_t idx = 0;
        if constexpr (config::handleMultiplicity) {
            entry->tuple[idx++] = multiplicity;
        }
        entry->tuple[idx++] = key;
        ((entry->tuple[idx++] = attrs), ...);

        // Add to buffer
        ls.buffer.push_back({key, entry});
        ls.num_tuples++;
    }

    /// Finish tuples - sort, build bloom filter, build tree
    void finishConsume();

    /// Constructor
    explicit HashtableBuild(Hashtable& ht, size_t card_estimate, size_t bloom_filter_bits = 0);

    std::string getPretty() const override;

    private:
    /// Collect and sort all buffer entries
    Vector<BufferEntry> collectAndSort();
    /// Build bloom filter from sorted entries
    void buildBloomFilter(const Vector<BufferEntry>& sorted_data);
    /// Build B+ tree bottom-up from sorted entries
    void buildTreeBottomUp(Vector<BufferEntry>& sorted_data);
};
//---------------------------------------------------------------------------
/// Probe sub operator - bloom filter + B+ tree search
struct HashtableProbe : OpBase {
    const Hashtable* ht;

    // whether to collect bloom filter stats (controlled by BLOOM_STATS setting)
    static inline bool collect_bloom_stats = false;

    // bloom filter false positive tracking
   // total probes attempted
    static std::atomic<size_t> bloom_probes;
    // bloom filter returned true
    static std::atomic<size_t> bloom_passes;
    // bloom passed but no match found
    static std::atomic<size_t> bloom_false_positives;

    static void resetBloomStats();
    static void printBloomStats();

    struct LocalState {
        explicit constexpr LocalState(HashtableProbe&) noexcept {}
    };

    explicit HashtableProbe(const Hashtable* ht) : ht(ht) {}

    void prepare(LocalState& ls, uint64_t key) {
        // Prefetch root page
        if (ht->root_) {
            __builtin_prefetch(ht->root_, 0, 0);
        }
    }

    template <typename KeyT, typename ConsumerType, typename = std::enable_if_t<Consumer<ConsumerType>>>
    [[gnu::always_inline]] void operator()(LocalState& ls, KeyT key, ConsumerType&& consumer) {
        if constexpr (config::collectBloomStats) {
            bloom_probes.fetch_add(1, std::memory_order_relaxed);
        }

        const bool timing_enabled = ProbeTiming::isEnabled();
        FastTimer phase_timer;

        if (timing_enabled) {
            phase_timer.start();
            ProbeTiming::tl_probe_count++;
        }

        // BF early rejection
        if (!ht->bloom_filter_.mayContain(key)) {
            if (timing_enabled) {
                ProbeTiming::tl_bloom_check_ns += phase_timer.elapsedNs();
                ProbeTiming::tl_bloom_reject_count++;
            }
            return;
        }

        if (timing_enabled) {
            ProbeTiming::tl_bloom_check_ns += phase_timer.elapsedNs();
        }

        if constexpr (config::collectBloomStats) {
            bloom_passes.fetch_add(1, std::memory_order_relaxed);
        }

        // search the tree
        if (!ht->root_) {
            if constexpr (config::collectBloomStats) {
                bloom_false_positives.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }

        void* current = ht->root_;

        if (timing_enabled) {
            phase_timer.start();
        }

        // traverse to leaf
        for (size_t level = ht->height_; level > 1; --level) {
            auto* internal = static_cast<Hashtable::InternalPage*>(current);

            // Binary search for child
            size_t lo = 0, hi = internal->header.num_keys;
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (internal->keys[mid] < key) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            current = internal->children[lo];
        }

        if (timing_enabled) {
            ProbeTiming::tl_tree_traverse_ns += phase_timer.elapsedNs();
            if (ht->height_ > 1) {
                ProbeTiming::tl_tree_traverse_count++;
            }
            phase_timer.start();
        }

        // search in leaf and get duplicates
        auto* leaf = static_cast<Hashtable::LeafPage*>(current);

        size_t lo = 0, hi = leaf->header.num_keys;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (leaf->keys[mid] < key) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }

        bool found_match = false;
        size_t leaf_pages_this_probe = 1;
        while (leaf) {
            for (size_t i = lo; i < leaf->header.num_keys; ++i) {
                if (leaf->keys[i] > key) {
                    if constexpr (config::collectBloomStats) {
                        if (!found_match) bloom_false_positives.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (timing_enabled) {
                        ProbeTiming::tl_leaf_search_ns += phase_timer.elapsedNs();
                        ProbeTiming::tl_leaf_pages_visited += leaf_pages_this_probe;
                    }
                    return;
                }

                if (leaf->keys[i] == key) {
                    found_match = true;
                    auto* entry = leaf->entries[i];
                    if (entry) {
                        if (timing_enabled) {
                            ProbeTiming::tl_leaf_search_ns += phase_timer.elapsedNs();
                            phase_timer.start();
                        }
                        consumer([entry](unsigned col) {
                            return entry->tuple[col];
                        });
                        if (timing_enabled) {
                            phase_timer.start();
                        }
                    }
                }
            }
            if (leaf->header.next_page_idx == 0xFFFFFFFF) break;
            leaf = ht->leaf_pages_[leaf->header.next_page_idx];
            leaf_pages_this_probe++;
            lo = 0;
        }
        if constexpr (config::collectBloomStats) {
            if (!found_match) bloom_false_positives.fetch_add(1, std::memory_order_relaxed);
        }

        if (timing_enabled) {
            ProbeTiming::tl_leaf_search_ns += phase_timer.elapsedNs();
            ProbeTiming::tl_leaf_pages_visited += leaf_pages_this_probe;
        }
    }

    std::string getPretty() const override;
};
//---------------------------------------------------------------------------
static_assert(TargetOperator<HashtableBuild, 1>);
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
