#pragma once
#include "Config.hpp"
#include "infra/JoinFilter.hpp"
#include "infra/QueryMemory.hpp"
#include "op/OpBase.hpp"
#include "op/TargetBase.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <fmt/core.h>

#include <iostream>
#include <mutex>
#include <thread>
namespace engine {
// BT implementation
class BT {
    public:
    mutable std::mutex tree_mutex;  // Protect concurrent tree access
    std::atomic<bool> build_complete;  // Track if build phase is done

    // Constructor to ensure proper initialization
    BT() : build_complete(false), root(nullptr), numTuples(0), numKeys(0), 
           isCertainlyDuplicateFree(false) {
    }

    static constexpr size_t MAX_KEYS = 64;
    // entry consists of a key and the tuple data
    struct Entry {
        uint64_t key;
        uint64_t tuple[];
    };

    struct Node {
        bool isLeaf;
        size_t keyCount;
        uint64_t keys[MAX_KEYS]; // max keys per node
        union {
            Node* children[MAX_KEYS + 1];
            Entry* entries[MAX_KEYS];
        };
        Node* next;
        Node* prev;

        Node(bool leaf = false) : isLeaf(leaf), keyCount(0), next(nullptr), prev(nullptr) {
            if (isLeaf) {
                for (size_t i = 0; i < MAX_KEYS; ++i) {
                    entries[i] = nullptr;
                }
            } else {
                for (size_t i = 0; i < MAX_KEYS + 1; ++i) {
                    children[i] = nullptr;
                }
            }
        }
    };

    static constexpr size_t keyOffset = config::handleMultiplicity ? 1 : 0;

    public:
    Node* root;
    size_t numTuples;
    size_t numKeys;
    bool isCertainlyDuplicateFree;
    std::string pretty;

    friend struct BTBuild;
    friend struct BTProbe;

    friend struct TestBT;

    [[nodiscard]] size_t getNumTuples() const { return numTuples; }
    [[nodiscard]] size_t getNumKeysEstimate() const { return numKeys; }
    [[nodiscard]] bool isEmpty() const { return numTuples == 0; }
    [[nodiscard]] size_t htSize() const { return numTuples; }
    [[nodiscard]] bool isDuplicateFree() const {
        return isCertainlyDuplicateFree;
    }

    // check if key exists in BT
    [[gnu::always_inline]] inline bool joinFilter(uint64_t key) const {
        return find(key) != nullptr;
    }

    // check if key exists in BT
    [[gnu::always_inline]] inline bool joinFilterPrecise(uint64_t key) const {
        return find(key) != nullptr;
    }

    // allocate BT
    void allocateBT(size_t numElements);

    // eq restrictions
    struct EqRestriction {
        unsigned offset;
        uint32_t value;
    };

    // filter table with eq restrictions
    void filterEq(const EqRestriction& restriction);

    // find entry by key
    Entry* find(uint64_t key) const;

    // insert entry
    void insert(uint64_t key, Entry* entry);

    // iterate over all keys found in BT
    template <typename CallbackT>
    void iterateAll(CallbackT&& callback) {
        if (!root) return;

        // leftmost leaf
        Node* current = root;
        while (!current->isLeaf) {
            current = current->children[0];
        }

        // iterate through all leaf nodes
        while (current) {
            for (size_t i = 0; i < current->keyCount; ++i) {
                if (current->entries[i]) {
                    callback(current->entries[i]->key);
                }
            }
            current = current->next;
        }
    }

    void prettyPrint(std::ostream& os = std::cout) const {
        os << "B+Tree @" << this
           << "  (keys=" << numKeys
           << ", tuples=" << numTuples << ")\n";
        if (!root) {
            os << "  <empty>\n";
            return;
        }
        prettyPrintNode(os, root, "", true);
    }

    static void printKeys(std::ostream& os, const Node* node) {
        os << "[";
        for (size_t i = 0; i < node->keyCount; ++i) {
            if (i) os << ", ";
            os << node->keys[i];
        }
        os << "]";
    }

    static void prettyPrintNode(std::ostream& os,
                                const Node* node,
                                std::string prefix,
                                bool isLast) {
        // Current node line prefix (tree branches)
        os << prefix;
        if (!prefix.empty()) {
            os << (isLast ? "└─ " : "├─ ");
        }

        // Node header
        os << (node->isLeaf ? "[L] " : "[I] ");
        printKeys(os, node);

        os << "  (keys=" << node->keyCount << ")";
        if (node->isLeaf) {
            os << "  leaf@" << node
               << "  next=" << node->next
               << "  prev=" << node->prev;
        } else {
            os << "  internal@" << node;
        }
        os << "\n";

        // Children
        if (!node->isLeaf) {
            std::string childPrefix = prefix;
            if (!prefix.empty())
                childPrefix += (isLast ? "   " : "│  ");

            // children[0..keyCount]
            for (size_t i = 0; i <= node->keyCount; ++i) {
                Node* child = node->children[i];
                if (!child) continue;
                bool childIsLast = (i == node->keyCount);
                prettyPrintNode(os, child, childPrefix, childIsLast);
            }
        }
    }

    void prettyPrintLeaves(std::ostream& os = std::cout) const {
        if (!root) {
            os << "<empty leaf level>\n";
            return;
        }
        const Node* leaf = leftmostLeaf(root);
        os << "Leaf chain:\n";
        while (leaf) {
            os << "  leaf@" << leaf << "  ";
            printKeys(os, leaf);
            os << "  next=" << leaf->next << "\n";
            leaf = leaf->next;
        }
    }

    static const Node* leftmostLeaf(const Node* node) {
        const Node* cur = node;
        while (cur && !cur->isLeaf) {
            // go down the leftmost non-null child
            Node* next = nullptr;
            for (size_t i = 0; i <= cur->keyCount; ++i) {
                if (cur->children[i]) {
                    next = cur->children[i];
                    break;
                }
            }
            if (!next) break;
            cur = next;
        }
        return cur;
    }

    private:
    // split node and return new node
    Node* splitNode(Node* node, uint64_t key, Entry* entry);

    void insertIntoLeaf(Node* leaf, uint64_t key, Entry* entry);

    void insertIntoInternal(Node* node, uint64_t key, Node* newChild);

    // find and return
    Node* findLeaf(uint64_t key) const;

    // recursive insert
    // returns {pivotKey, newRightNode} if split occurred, else {0, nullptr}
    std::pair<uint64_t, Node*> insertRecursive(Node* node, uint64_t key, Entry* entry);

    // split internal node
    std::pair<uint64_t, Node*> splitInternalNode(Node* node, uint64_t key, Node* newChild);
};

// build sub operator for BT
struct BTBuild : public TargetImpl<BTBuild> {
    static constexpr size_t maxPartitionsShift = 7;
    static constexpr size_t maxPartitions = 1ull << maxPartitionsShift;

    struct ChunkRef {
        uint64_t* cur = nullptr;
        const uint64_t* end = nullptr;
    };

    struct Chunk;
    struct Block;

    struct LocalState {
        size_t numTuples = 0;
        size_t partitionShift;
        std::array<ChunkRef, maxPartitions> partitions;
        std::array<Chunk*, maxPartitions> chunks;
        // linked list of blocks
        Block* blocks = nullptr;
        // first allocated block
        Block* tail = nullptr;
        // number of attributes
        size_t attrCount = 0;
        // nxt local state
        LocalState* next = nullptr;

        // Allocate a chunk
        void allocateChunk(uint32_t partition, size_t attrCount);

        explicit LocalState(BTBuild& build);
    };

    BT& tree;
    // mask for partitions
    size_t partitionShift;
    // references to local states
    std::atomic<LocalState*> localStateRefs = nullptr;

    // build cross product table or normal table?
    bool isCrossProduct = false;

    template <typename... AttrT>
    void operator()(LocalState& ls, uint64_t multiplicity, uint64_t key, AttrT... attrs) {
        ls.numTuples++;

        constexpr bool HM = config::handleMultiplicity;
        constexpr size_t populated =
            (HM ? 1 : 0) + 1 + sizeof...(attrs);

        auto thing = querymemory::allocate(sizeof(BT::Entry) + populated * sizeof(uint64_t));
        auto* entry = static_cast<BT::Entry*>(thing);

        entry->key = key;

        size_t idx = 0;
        if constexpr (HM) entry->tuple[idx++] = multiplicity;
        entry->tuple[idx++] = key;
        ((entry->tuple[idx++] = static_cast<uint64_t>(attrs)), ...);

        assert(idx == populated);

        /*  std::cout << "Inserting key: " << key << "\n";
        std::cout << "Multiplicity: " << multiplicity << "\n";
        std::cout << "Entry: " << entry << "\n";
        std::cout << "Entry->key: " << entry->key << "\n";
        std::cout << "Entry->tuple[0]: " << entry->tuple[0] << "\n";
        std::cout << "Entry->tuple[1]: " << entry->tuple[1] << "\n";
        if constexpr (config::handleMultiplicity) {
            std::cout << "Entry->tuple[2]: " << entry->tuple[2] << "\n";
        } */

        // std::cout << "root keycount before insert: " << tree.root->keyCount << "\n";
        tree.insert(key, entry);
        // std::cout << "root keycount after insert: " << tree.root->keyCount << "\n";
    }

    // finish tuples
    void finishConsume();
    explicit BTBuild(BT& tree, size_t cardEstimate);
    std::string getPretty() const override;
};

// probe sub operator for BT
struct BTProbe : OpBase {
    const BT* tree;

    struct LocalState {
        explicit constexpr LocalState(BTProbe&) noexcept {}
    };

    explicit BTProbe(const BT* tree) : tree(tree) {
    }

    void prepare(LocalState& ls, uint64_t key) {
        // prefetch
        auto* entry = tree->find(key);
        if (entry) {
            __builtin_prefetch(entry, 0, 0); // read+nta
        }
    }

    template <typename KeyT, typename ConsumerType, typename = std::enable_if_t<Consumer<ConsumerType>>>
    void operator()(LocalState& ls, KeyT key, ConsumerType&& consumer) {
        // Ensure key is uint64_t as expected
        uint64_t search_key = static_cast<uint64_t>(key);
        
        // Wait for build to complete
        while (!tree->build_complete.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        
        // Acquire lock to ensure memory visibility of all tree state
        std::lock_guard<std::mutex> guard(tree->tree_mutex);
        auto* node = tree->root;
        if (!node) return;

        while (!node->isLeaf) {
            size_t i = 0;
            // Must match insertRecursive logic: when key equals a pivot,
            // we go to the right child (same as insertion does)
            while (i < node->keyCount && search_key >= node->keys[i]) ++i;
            
            node = node->children[i];
        }

        size_t idx = 0;
        size_t lo = 0, hi = node->keyCount;
        while (lo < hi) {
            size_t mid = (lo + hi) >> 1;
            if (node->keys[mid] < search_key) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        idx = lo;

        while (node) {
            for (; idx < node->keyCount; ++idx) {
                uint64_t k = node->keys[idx];
                if (k > search_key) return;

                if (k == search_key) {
                    auto* entry = node->entries[idx];
                    if (entry) {
                        consumer([entry](unsigned col) { return entry->tuple[col]; });
                    }
                }
            }
            node = node->next;
            idx = 0;
        }
    }

    std::string getPretty() const override;
};

static_assert(TargetOperator<BTBuild, 1>);

}

struct TestBT {
    void simpleInsertFind() {
        engine::BT tree;
        tree.allocateBT(10);
        for (size_t i = 0; i < 10; ++i) {
            engine::BT::Entry entry;
            entry.key = i;
            entry.tuple[0] = i;
            tree.insert(i, &entry);
        }
        for (size_t i = 0; i < 10; ++i) {
            engine::BT::Entry* entry = tree.find(i);
            if (entry) {
                fmt::println("found entry: {}", entry->key);
            } else {
                fmt::println("entry not found: {}", i);
            }
        }
    }
};
