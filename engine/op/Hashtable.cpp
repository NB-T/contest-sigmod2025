#include "op/Hashtable.hpp"
#include "infra/QueryMemory.hpp"
#include "infra/Scheduler.hpp"
#include "infra/helper/BitOps.hpp"
#include "query/DataSource.hpp"
#include "query/RuntimeValue.hpp"

#include <parlay/primitives.h>
#include <parlay/slice.h>

#include <algorithm>
#include <numeric>
#include <unordered_set>
//---------------------------------------------------------------------------
namespace engine {
//---------------------------------------------------------------------------
/// The size of a chunk for entry allocation
static constexpr size_t chunk_size = 8048;
/// The number of uint64_t values in a chunk (minus header)
static constexpr size_t chunk_count = chunk_size / sizeof(uint64_t) - 2;
/// The number of chunks in a block
static constexpr size_t block_chunks = HashtableBuild::maxPartitions - 1;
static_assert(block_chunks < (1ull << 16));
//---------------------------------------------------------------------------
/// A chunk for entry allocation
struct HashtableBuild::Chunk {
    /// The next chunk
    Chunk* next;
    /// Current position in data
    size_t pos;
    /// The data
    uint64_t data[chunk_count];
};
static_assert(sizeof(HashtableBuild::Chunk) == chunk_size);

/// A block containing multiple chunks
struct alignas(4096) HashtableBuild::Block {
    /// The next block
    Block* next;
    /// The next chunk to allocate
    size_t current_chunk;
    /// The chunks
    Chunk chunks[block_chunks];
};
//---------------------------------------------------------------------------
std::string HashtableBuild::getPretty() const {
    return "ht";
}
//---------------------------------------------------------------------------
std::string HashtableProbe::getPretty() const {
    return ht->pretty;
}
//---------------------------------------------------------------------------
HashtableBuild::LocalState::LocalState(HashtableBuild& build) {
    next = build.local_state_refs.exchange(this);
}
//---------------------------------------------------------------------------
Hashtable::Entry* HashtableBuild::LocalState::allocateEntry(size_t attr_count_inp) {
    // entry has data and next pointer
    size_t entry_size = 1 + attr_count_inp; // next pointer counts as 1 uint64_t

    // if chunk has no space
    if (!chunk_pos || chunk_pos + entry_size > chunk_end) {
        // allocate new chunk
        if (!blocks || blocks->current_chunk == block_chunks) {
            // block
            static_assert(sizeof(Block) % DataSource::PAGE_SIZE == 0);
            auto* new_block = static_cast<Block*>(querymemory::allocate(sizeof(Block)));
            new_block->next = blocks;
            new_block->current_chunk = 0;
            blocks = new_block;
        }

        attr_count = attr_count_inp;

        // chunk
        auto* new_chunk = &blocks->chunks[blocks->current_chunk++];
        new_chunk->next = current_chunk;
        new_chunk->pos = 0;
        current_chunk = new_chunk;

        chunk_pos = new_chunk->data;
        chunk_end = new_chunk->data + chunk_count - (chunk_count % entry_size);
    }

    // allocate entry from current chunk
    auto* entry = reinterpret_cast<Hashtable::Entry*>(chunk_pos);
    chunk_pos += entry_size;
    return entry;
}
//---------------------------------------------------------------------------
void Hashtable::allocateHashtable(size_t num_elements) {
    // obsolete
}
//---------------------------------------------------------------------------
void Hashtable::filterEq(const EqRestriction& restriction) {
    // filter entries in leaf pages
    for (auto* leaf : leaf_pages_) {
        size_t write_pos = 0;
        for (size_t i = 0; i < leaf->header.num_keys; ++i) {
            auto* entry = leaf->entries[i];
            if (entry && entry->tuple[restriction.offset + 1] == restriction.value) {
                // Keep this entry
                if (write_pos != i) {
                    leaf->keys[write_pos] = leaf->keys[i];
                    leaf->entries[write_pos] = leaf->entries[i];
                }
                write_pos++;
            } else {
                // remove entry
                if (num_tuples > 0) num_tuples--;
            }
        }
        leaf->header.num_keys = write_pos;
    }

    // recount unique keys
    std::unordered_set<uint64_t> unique_keys;
    for (auto* leaf : leaf_pages_) {
        for (size_t i = 0; i < leaf->header.num_keys; ++i) {
            unique_keys.insert(leaf->keys[i]);
        }
    }
    num_keys = unique_keys.size();
}
//---------------------------------------------------------------------------
Vector<HashtableBuild::BufferEntry> HashtableBuild::collectAndSort() {
    // count
    size_t total_tuples = 0;
    for (auto* ls = local_state_refs.load(); ls; ls = ls->next) {
        total_tuples += ls->num_tuples;
    }

    if (total_tuples == 0) {
        return Vector<BufferEntry>();
    }

    // merge local buffers
    Vector<BufferEntry> all_entries;
    all_entries.reserve(total_tuples);

    for (auto* ls = local_state_refs.load(); ls; ls = ls->next) {
        for (const auto& entry : ls->buffer) {
            all_entries.push_back(entry);
        }
    }

    // sort with parlay::integer_sort_inplace
    auto slice = parlay::make_slice(all_entries.data(), all_entries.data() + all_entries.size());
    parlay::integer_sort_inplace(slice, [](const BufferEntry& e) {
        return e.key;
    });

    return all_entries;
}
//---------------------------------------------------------------------------
void HashtableBuild::buildBloomFilter(const Vector<BufferEntry>& sorted_data) {
    if (sorted_data.empty()) return;

    // determine bloom filter size
    size_t bits = bloom_filter_bits_;
    if (bits == 0) {
        // Default: 10 bits per key for ~1% false positive rate
        bits = std::max(sorted_data.size() * 10, size_t(1024));
    }

    ht.bloom_filter_.allocate(bits);

    // add keys in parallel
    size_t n = sorted_data.size();
    if (n > 256) {
        Scheduler::parallelFor(0, n, [&](size_t, size_t i) {
            ht.bloom_filter_.add(sorted_data[i].key);
        });
    } else {
        for (size_t i = 0; i < n; ++i) {
            ht.bloom_filter_.add(sorted_data[i].key);
        }
    }
}
//---------------------------------------------------------------------------
void HashtableBuild::buildTreeBottomUp(Vector<BufferEntry>& sorted_data) {
    if (sorted_data.empty()) {
        ht.root_ = nullptr;
        ht.height_ = 0;
        return;
    }

    size_t n = sorted_data.size();
    size_t entries_per_page = Hashtable::LeafPage::MAX_ENTRIES;
    size_t num_leaf_pages = (n + entries_per_page - 1) / entries_per_page;

    // create leaf pages in parallel
    ht.leaf_pages_.resize(num_leaf_pages);

    if (num_leaf_pages > 16) {
        Scheduler::parallelFor(0, num_leaf_pages, [&](size_t, size_t page_idx) {
            auto* page = static_cast<Hashtable::LeafPage*>(
                querymemory::allocate(BTREE_PAGE_SIZE));

            page->header.is_leaf = 1;
            page->header.num_keys = 0;
            page->header.reserved = 0;
            page->header.next_page_idx = (page_idx + 1 < num_leaf_pages) ? static_cast<uint32_t>(page_idx + 1) : 0xFFFFFFFF;

            size_t start = page_idx * entries_per_page;
            size_t end = std::min(start + entries_per_page, n);

            for (size_t i = start; i < end; ++i) {
                size_t slot = i - start;
                page->keys[slot] = sorted_data[i].key;
                page->entries[slot] = sorted_data[i].entry;
                page->header.num_keys++;
            }

            ht.leaf_pages_[page_idx] = page;
        });
    } else {
        for (size_t page_idx = 0; page_idx < num_leaf_pages; ++page_idx) {
            auto* page = static_cast<Hashtable::LeafPage*>(
                querymemory::allocate(BTREE_PAGE_SIZE));

            page->header.is_leaf = 1;
            page->header.num_keys = 0;
            page->header.reserved = 0;
            page->header.next_page_idx = (page_idx + 1 < num_leaf_pages) ? static_cast<uint32_t>(page_idx + 1) : 0xFFFFFFFF;

            size_t start = page_idx * entries_per_page;
            size_t end = std::min(start + entries_per_page, n);

            for (size_t i = start; i < end; ++i) {
                size_t slot = i - start;
                page->keys[slot] = sorted_data[i].key;
                page->entries[slot] = sorted_data[i].entry;
                page->header.num_keys++;
            }

            ht.leaf_pages_[page_idx] = page;
        }
    }

    // singleton tree just has root
    if (num_leaf_pages == 1) {
        ht.root_ = ht.leaf_pages_[0];
        ht.height_ = 1;
        return;
    }

    // build internal levels bottom up
    Vector<void*> current_level;
    current_level.reserve(num_leaf_pages);
    for (auto* leaf : ht.leaf_pages_) {
        current_level.push_back(leaf);
    }

    bool children_are_leaves = true;
    size_t height = 1;

    while (current_level.size() > 1) {
        size_t num_children = current_level.size();
        size_t children_per_page = Hashtable::InternalPage::MAX_KEYS + 1;
        size_t num_parent_pages = (num_children + children_per_page - 1) / children_per_page;

        Vector<Hashtable::InternalPage*> parent_pages;
        parent_pages.resize(num_parent_pages);

        if (num_parent_pages > 16) {
            Scheduler::parallelFor(0, num_parent_pages, [&](size_t, size_t page_idx) {
                auto* page = static_cast<Hashtable::InternalPage*>(
                    querymemory::allocate(BTREE_PAGE_SIZE));

                page->header.is_leaf = 0;
                page->header.num_keys = 0;
                page->header.reserved = 0;
                page->header.next_page_idx = 0xFFFFFFFF;

                size_t start = page_idx * children_per_page;
                size_t end = std::min(start + children_per_page, num_children);
                size_t num_children_in_page = end - start;

                // children
                // get high keys
                for (size_t i = 0; i < num_children_in_page; ++i) {
                    page->children[i] = current_level[start + i];

                    if (i < num_children_in_page - 1) {
                        uint64_t high_key;
                        if (children_are_leaves) {
                            auto* leaf = static_cast<Hashtable::LeafPage*>(current_level[start + i]);
                            high_key = leaf->keys[leaf->header.num_keys - 1];
                        } else {
                            // DEBUG 19d:
                            // For internal pages, we need the high key of the rightmost leaf
                            // in the subtree, not the last separator key
                            void* rightmost = current_level[start + i];
                            while (true) {
                                auto* internal_node = static_cast<Hashtable::InternalPage*>(rightmost);
                                rightmost = internal_node->children[internal_node->header.num_keys];
                                if (static_cast<Hashtable::PageHeader*>(rightmost)->is_leaf) break;
                            }
                            auto* leaf = static_cast<Hashtable::LeafPage*>(rightmost);
                            high_key = leaf->keys[leaf->header.num_keys - 1];
                        }
                        page->keys[page->header.num_keys++] = high_key;
                    }
                }

                parent_pages[page_idx] = page;
            });
        } else {
            for (size_t page_idx = 0; page_idx < num_parent_pages; ++page_idx) {
                auto* page = static_cast<Hashtable::InternalPage*>(
                    querymemory::allocate(BTREE_PAGE_SIZE));

                page->header.is_leaf = 0;
                page->header.num_keys = 0;
                page->header.reserved = 0;
                page->header.next_page_idx = 0xFFFFFFFF;

                size_t start = page_idx * children_per_page;
                size_t end = std::min(start + children_per_page, num_children);
                size_t num_children_in_page = end - start;

                for (size_t i = 0; i < num_children_in_page; ++i) {
                    page->children[i] = current_level[start + i];

                    if (i < num_children_in_page - 1) {
                        uint64_t high_key;
                        if (children_are_leaves) {
                            auto* leaf = static_cast<Hashtable::LeafPage*>(current_level[start + i]);
                            high_key = leaf->keys[leaf->header.num_keys - 1];
                        } else {
                            void* rightmost = current_level[start + i];
                            while (true) {
                                auto* internal_node = static_cast<Hashtable::InternalPage*>(rightmost);
                                rightmost = internal_node->children[internal_node->header.num_keys];
                                if (static_cast<Hashtable::PageHeader*>(rightmost)->is_leaf) break;
                            }
                            auto* leaf = static_cast<Hashtable::LeafPage*>(rightmost);
                            high_key = leaf->keys[leaf->header.num_keys - 1];
                        }
                        page->keys[page->header.num_keys++] = high_key;
                    }
                }

                parent_pages[page_idx] = page;
            }
        }

        // next level
        current_level.clear();
        for (auto* p : parent_pages) {
            current_level.push_back(p);
        }
        children_are_leaves = false;
        height++;
    }

    ht.root_ = current_level[0];
    ht.height_ = height;
}
//---------------------------------------------------------------------------
void HashtableBuild::finishConsume() {
    // collect and sort
    auto sorted_data = collectAndSort();

    ht.num_tuples = sorted_data.size();

    if (sorted_data.empty()) {
        ht.root_ = nullptr;
        ht.num_tuples = 0;
        ht.num_keys = 0;
        ht.height_ = 0;
        return;
    }

    // count unique keys
    size_t unique_keys = 1;
    bool has_duplicates = false;
    for (size_t i = 1; i < sorted_data.size(); ++i) {
        if (sorted_data[i].key != sorted_data[i - 1].key) {
            unique_keys++;
        } else {
            has_duplicates = true;
        }
    }

    ht.num_keys = unique_keys;
    ht.is_certainly_duplicate_free = !has_duplicates;

    // duplicates
    if constexpr (config::handleMultiplicity) {
        for (const auto& entry : sorted_data) {
            if (entry.entry->tuple[0] > 1) {
                ht.is_certainly_duplicate_free = false;
                break;
            }
        }
    }

    // bloom filter
    buildBloomFilter(sorted_data);

    // build tree bottom up
    buildTreeBottomUp(sorted_data);

    // cleanup local states
    if constexpr (!std::is_trivially_destructible_v<LocalState>) {
        for (auto* current = local_state_refs.load(); current; current = current->next) {
            current->~LocalState();
        }
    }
}
//---------------------------------------------------------------------------
HashtableBuild::HashtableBuild(Hashtable& ht, size_t card_estimate, size_t bloom_filter_bits)
    : ht(ht), bloom_filter_bits_(bloom_filter_bits) {
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
