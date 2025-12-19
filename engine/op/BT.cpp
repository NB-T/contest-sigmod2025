#include "op/BT.hpp"
#include "infra/QueryMemory.hpp"
#include "infra/Scheduler.hpp"
#include "query/RuntimeValue.hpp"
#include "storage/StringPtr.hpp"
#include <algorithm>
#include <cstring>
#include <shared_mutex>
namespace engine {
static constexpr size_t chunkSize = 8048;
// number of attrs in a chunk
static constexpr size_t chunkCount = chunkSize / sizeof(uint64_t) - 2;
/// number of chunks in a block
static constexpr size_t blockChunks = BTBuild::maxPartitions - 1;

struct BTBuild::Chunk {
    // next chunk within partition
    Chunk* next;
    size_t end;
    uint64_t data[chunkCount];
};
static_assert(sizeof(BTBuild::Chunk) == chunkSize);

// block
struct alignas(4096) BTBuild::Block {
    // next block within allocated blocks
    Block* next;
    // upcoming chunk to allocate
    size_t currentChunk;
    // chunks
    Chunk chunks[blockChunks];
};

std::string BTBuild::getPretty() const {
    return "bt";
}

std::string BTProbe::getPretty() const {
    return tree->pretty;
}

BTBuild::LocalState::LocalState(BTBuild& build) : partitionShift(build.partitionShift) {
    auto numPartitions = 1ull << (BTBuild::maxPartitionsShift - partitionShift);
    memset(partitions.data(), 0, sizeof(ChunkRef) * numPartitions);
    memset(chunks.data(), 0, sizeof(Chunk*) * numPartitions);
    next = build.localStateRefs.exchange(this);
}

void BTBuild::LocalState::allocateChunk(uint32_t partition, size_t attrCountInp) {
    // allocate a block if full
    if (!blocks || (blocks->currentChunk == blockChunks)) {
        attrCount = attrCountInp;
        static_assert(sizeof(Block) % 4096 == 0);
        auto* newBlock = static_cast<Block*>(querymemory::allocate(sizeof(Block)));
        newBlock->next = blocks;
        newBlock->currentChunk = 0;
        blocks = newBlock;
        if (!tail)
            tail = newBlock;
    }
    assert(attrCountInp == attrCount);

    auto& part = partitions[partition];
    // update end position of the previous chunk
    if (chunks[partition]) {
        assert(part.cur);
        assert(part.cur >= chunks[partition]->data);
        assert(part.cur <= chunks[partition]->data + chunkCount);
        chunks[partition]->end = part.cur - chunks[partition]->data;
    }

    // allocate the new chunk
    auto* newChunk = &blocks->chunks[blocks->currentChunk++];
    newChunk->next = chunks[partition];
    newChunk->end = 0;
    chunks[partition] = newChunk;

    // set up partition
    part.cur = newChunk->data;
    part.end = newChunk->data + chunkCount - (chunkCount % attrCount);
}

void BT::allocateBT(size_t numElements) {
    // empty BT
    root = nullptr;
    numTuples = 0;
    numKeys = 0;
    isCertainlyDuplicateFree = false;
    build_complete.store(false);
}

void BT::filterEq(const EqRestriction& restriction) {
    // empty BT
    if (!root) return;

    // leftmost leaf
    BT::Node* current = root;
    while (!current->isLeaf) {
        current = current->children[0];
    }

    // iterate through all leaf nodes and filter
    while (current) {
        for (size_t i = 0; i < current->keyCount; ++i) {
            if constexpr (config::handleMultiplicity) {
                if (current->entries[i]->tuple[0] != restriction.value) {
                    current->entries[i] = nullptr;
                    numTuples--;
                }
            } else {
                if (current->entries[i]->tuple[restriction.offset + 1] != restriction.value) {
                    // remove this entry
                    current->entries[i] = nullptr;
                    numTuples--;
                }
            }
        }
        current = current->next;
    }
}

BT::Entry* BT::find(uint64_t key) const {
    Node* leaf = findLeaf(key);
    if (!leaf) return nullptr;
    // binary search over [0, keyCount)
    size_t lo = 0, hi = leaf->keyCount;
    while (lo < hi) {
        size_t mid = (lo + hi) >> 1;
        if (leaf->keys[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < leaf->keyCount && leaf->keys[lo] == key) {
        return leaf->entries[lo];
    }
    return nullptr;
}

void BT::insert(uint64_t key, Entry* entry) {
    std::lock_guard<std::mutex> guard(tree_mutex);  // Protect concurrent insertions
    
    if (!root) {
        root = new Node(true);
    }

    auto res = insertRecursive(root, key, entry);
    if (res.second) {
        // root split
        Node* newRoot = new Node(false);
        newRoot->children[0] = root;
        newRoot->keys[0] = res.first;
        newRoot->children[1] = res.second;
        newRoot->keyCount = 1;
        root = newRoot;
    }
}

std::pair<uint64_t, BT::Node*> BT::insertRecursive(Node* node, uint64_t key, Entry* entry) {
    if (node->isLeaf) {
        if (node->keyCount < MAX_KEYS) {
            insertIntoLeaf(node, key, entry);
            return {0, nullptr};
        } else {
            Node* right = splitNode(node, key, entry);
            return {right->keys[0], right};
        }
    } else {
        size_t i = 0;
        while (i < node->keyCount && key >= node->keys[i]) ++i;

        auto res = insertRecursive(node->children[i], key, entry);
        if (res.second) {
            // child split
            if (node->keyCount < MAX_KEYS) {
                insertIntoInternal(node, res.first, res.second);
                return {0, nullptr};
            } else {
                return splitInternalNode(node, res.first, res.second);
            }
        }
        return {0, nullptr};
    }
}

BT::Node* BT::findLeaf(uint64_t key) const {
    Node* x = root;
    if (!x) return nullptr;
    while (!x->isLeaf) {
        // choose first i with key < keys[i], else the rightmost child
        size_t i = 0;
        while (i < x->keyCount && key >= x->keys[i]) ++i;
        x = x->children[i];
        if (!x) return nullptr;
    }
    return x;
}

void BT::insertIntoLeaf(Node* leaf, uint64_t key, Entry* entry) {
    size_t pos = 0;
    size_t lo = 0, hi = leaf->keyCount;
    while (lo < hi) {
        size_t mid = (lo + hi) >> 1;
        uint64_t k = leaf->keys[mid];
        if (k < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    pos = lo;

    // shift
    for (size_t i = leaf->keyCount; i > pos; --i) {
        leaf->keys[i] = leaf->keys[i - 1];
        leaf->entries[i] = leaf->entries[i - 1];
    }

    leaf->keys[pos] = key;
    leaf->entries[pos] = entry;
    ++leaf->keyCount;
}

void BT::insertIntoInternal(Node* node, uint64_t key, Node* newChild) {
    size_t pos = 0;
    while (pos < node->keyCount && node->keys[pos] < key)
        ++pos;

    // correct child shift (note +1 range)
    for (size_t i = node->keyCount + 1; i > pos + 1; --i)
        node->children[i] = node->children[i - 1];
    for (size_t i = node->keyCount; i > pos; --i)
        node->keys[i] = node->keys[i - 1];

    node->keys[pos] = key;
    node->children[pos + 1] = newChild;
    ++node->keyCount;
}

BT::Node* BT::splitNode(Node* node, uint64_t key, Entry* entry) {
    std::array<uint64_t, MAX_KEYS + 1> tmp_keys;
    std::array<Entry*, MAX_KEYS + 1> tmp_entries{};
    size_t total = node->keyCount;
    size_t insertPos = 0;
    while (insertPos < total && node->keys[insertPos] < key)
        ++insertPos;

    size_t idx = 0;
    for (; idx < insertPos; ++idx) {
        tmp_keys[idx] = node->keys[idx];
        tmp_entries[idx] = node->entries[idx];
    }
    if (entry) {
        tmp_keys[idx] = key;
        tmp_entries[idx] = entry;
        ++idx;
    }
    for (size_t j = insertPos; j < total; ++j, ++idx) {
        tmp_keys[idx] = node->keys[j];
        tmp_entries[idx] = node->entries[j];
    }

    size_t mid = (idx + 1) / 2;
    Node* right = new Node(node->isLeaf);

    node->keyCount = mid;
    right->keyCount = idx - mid;

    for (size_t i = 0; i < mid; ++i) {
        node->keys[i] = tmp_keys[i];
        node->entries[i] = tmp_entries[i];
    }

    for (size_t i = 0; i < right->keyCount; ++i) {
        right->keys[i] = tmp_keys[mid + i];
        right->entries[i] = tmp_entries[mid + i];
    }

    if (node->isLeaf) {
        right->next = node->next;
        if (right->next) right->next->prev = right;
        node->next = right;
        right->prev = node;
    }

    return right;
}

std::pair<uint64_t, BT::Node*> BT::splitInternalNode(Node* node, uint64_t key, Node* newChild) {
    std::array<uint64_t, MAX_KEYS + 1> tmp_keys;
    std::array<Node*, MAX_KEYS + 2> tmp_children;

    size_t total = node->keyCount;
    size_t insertPos = 0;
    while (insertPos < total && node->keys[insertPos] < key)
        ++insertPos;

    for (size_t i = 0; i < insertPos; ++i) {
        tmp_keys[i] = node->keys[i];
        tmp_children[i] = node->children[i];
    }
    tmp_children[insertPos] = node->children[insertPos];

    tmp_keys[insertPos] = key;
    tmp_children[insertPos + 1] = newChild;

    for (size_t i = insertPos; i < total; ++i) {
        tmp_keys[i + 1] = node->keys[i];
        tmp_children[i + 2] = node->children[i + 1];
    }

    size_t numKeys = total + 1;
    size_t mid = numKeys / 2;

    Node* right = new Node(false);

    node->keyCount = mid;
    uint64_t pivotKey = tmp_keys[mid];

    right->keyCount = numKeys - mid - 1;

    for (size_t i = 0; i < node->keyCount; ++i) {
        node->keys[i] = tmp_keys[i];
        node->children[i] = tmp_children[i];
    }
    node->children[node->keyCount] = tmp_children[node->keyCount];

    for (size_t i = 0; i < right->keyCount; ++i) {
        right->keys[i] = tmp_keys[mid + 1 + i];
        right->children[i] = tmp_children[mid + 1 + i];
    }
    right->children[right->keyCount] = tmp_children[numKeys];

    return {pivotKey, right};
}

void BTBuild::finishConsume() {
    size_t total = 0;
    for (auto* cur = localStateRefs.load(); cur; cur = cur->next)
        total += cur->numTuples;

    tree.numTuples = total;
    tree.numKeys = 0;
    tree.isCertainlyDuplicateFree = false;
    
    // Ensure all tree modifications are visible before marking complete
    std::atomic_thread_fence(std::memory_order_release);
    tree.build_complete.store(true, std::memory_order_release);

    if constexpr (!std::is_trivially_destructible_v<LocalState>) {
        for (auto* cur = localStateRefs.load(); cur; cur = cur->next)
            cur->~LocalState();
    }
}

BTBuild::BTBuild(BT& tree, size_t cardEstimate) : tree(tree) {
    // simple partitioning
    partitionShift = 0;
}

}
