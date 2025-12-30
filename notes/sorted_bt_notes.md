# Sorted B+ Tree Join Implementation

This document describes the new three-phase join implementation that replaces the previous hash table and B+ tree join implementations. Mostly written by Claude.

## Overview

The new join implementation uses a sorted B+ tree with a Bloom filter for early rejection. It consists of three phases:

1. **Collection Phase**: Collect tuples into thread-local buffers, merge, and sort using `parlay::integer_sort`
2. **Build Phase**: Construct B+ tree bottom-up from sorted data with parallel page creation
3. **Probe Phase**: Bloom filter early rejection followed by B+ tree binary search

## Architecture

### Data Structures

#### Bloom Filter (`engine/infra/BloomFilter.hpp`)

```cpp
class BloomFilter {
    uint64_t* bits_;
    size_t totalBits_;
    static constexpr size_t NUM_HASHES = 3;

    void allocate(size_t totalBits);
    void add(uint64_t key);           // Thread-safe with atomic OR
    bool mayContain(uint64_t key);    // May have false positives
};
```

- Configurable total bits (default: 10 bits per key for ~1% false positive rate)
- Thread-safe concurrent adds using `__atomic_fetch_or`
- Uses double hashing with 3 hash functions

#### B+ Tree Pages (`engine/op/Hashtable.hpp`)

```cpp
// Page size matches DataSource::PAGE_SIZE = 8192 bytes
static constexpr size_t BTREE_PAGE_SIZE = 8192;

struct PageHeader {
    uint16_t isLeaf : 1;
    uint16_t numKeys : 15;
    uint16_t reserved;
    uint32_t nextPageIdx;  // Sibling chain for leaves
};

struct LeafPage {
    PageHeader header;
    static constexpr size_t MAX_ENTRIES = 511;  // (8192-8)/16
    uint64_t keys[MAX_ENTRIES];
    Entry* entries[MAX_ENTRIES];
};

struct InternalPage {
    PageHeader header;
    static constexpr size_t MAX_KEYS = 510;  // (8192-8-8)/16
    uint64_t keys[MAX_KEYS];
    void* children[MAX_KEYS + 1];
};
```

### Build Operator (`HashtableBuild`)

The build operator collects tuples from the scan phase and constructs the B+ tree:

```cpp
struct HashtableBuild {
    struct BufferEntry {
        uint64_t key;
        Entry* entry;
    };

    struct LocalState {
        Vector<BufferEntry> buffer;  // local buffer
        // ... chunk allocation for entry storage
    };

    void operator()(LocalState&, uint64_t mult, uint64_t key, Attrs...);
    void finishConsume();  // sort, Bloom filter, tree
};
```

#### Phase 1: Collection and Sort

```cpp
Vector<BufferEntry> collectAndSort() {
    // merge local buffers
    Vector<BufferEntry> allEntries;
    for (auto* ls = localStateRefs.load(); ls; ls = ls->next) {
        for (const auto& entry : ls->buffer) {
            allEntries.push_back(entry);
        }
    }

    // sort with parlay::integer_sort_inplace
    auto slice = parlay::make_slice(allEntries.data(),
                                     allEntries.data() + allEntries.size());
    parlay::integer_sort_inplace(slice, [](const BufferEntry& e) {
        return e.key;
    });

    return allEntries;
}
```

#### Phase 2: Bottom-Up Tree Build

```cpp
void buildTreeBottomUp(Vector<BufferEntry>& sortedData) {
    // create leafs in parallel
    size_t numLeafPages = (n + entriesPerPage - 1) / entriesPerPage;
    Scheduler::parallelFor(0, numLeafPages, [&](size_t, size_t pageIdx) {
        auto* page = allocate<LeafPage>(BTREE_PAGE_SIZE);
    });

    // internal levels
    while (currentLevel.size() > 1) {
        Scheduler::parallelFor(0, numParentPages, [&](size_t, size_t pageIdx) {
            auto* page = allocate<InternalPage>(BTREE_PAGE_SIZE);
        });
        currentLevel = parentPages;
    }

    root_ = currentLevel[0];
}
```

### Probe Operator (`HashtableProbe`)

```cpp
void operator()(LocalState& ls, KeyT key, ConsumerType&& consumer) {
    // Bloom filter
    if (!ht->bloomFilter_.mayContain(key)) {
        return;
    }

    // binary search to get to leaf
    void* current = ht->root_;
    for (size_t level = ht->height_; level > 1; --level) {
        auto* internal = static_cast<InternalPage*>(current);
        current = internal->children[index];
    }

    //search leaf
    auto* leaf = static_cast<LeafPage*>(current);
    // duplicates
    while (leaf && leaf->keys[i] == key) {
        consumer([entry](unsigned col) { return entry->tuple[col]; });
    }
}
```

## Files Changed

| File | Description |
|------|-------------|
| `engine/infra/BloomFilter.hpp` | New Bloom filter implementation |
| `engine/op/Hashtable.hpp` | B+ tree data structures, build/probe operators |
| `engine/op/Hashtable.cpp` | Three-phase build implementation |
| `engine/op/BT.hpp` | Type alias (`using BT = Hashtable`) |
| `engine/op/BT.cpp` | Empty (compatibility only) |
| `engine/query/Restriction.hpp` | Forward declarations for BT/BTBuild |
| `engine/pipeline/PipelineFunction.hpp` | Forward declarations |
| `engine/query/QueryPlan.hpp` | Removed conflicting forward declarations |
| `CMakeLists.txt` | Added `engine/op/Hashtable.cpp` to build |

## Performance Characteristics

### Build Phase
- **Sorting**: O(n) using radix sort via `parlay::integer_sort`
- **Bloom filter**: O(n) with parallel adds
- **Tree construction**: O(n) with parallel page creation per level

### Probe Phase
- **Bloom filter check**: O(1) - constant number of hash lookups
- **Tree traversal**: O(log n) - binary search at each level
- **Duplicate iteration**: O(k) where k is number of matches

### Memory Usage
- **Bloom filter**: Configurable (default 10 bits/key)
- **Leaf pages**: 8192 bytes each, 511 entries max
- **Internal pages**: 8192 bytes each, 510 keys max
- **Tree height**: log₅₁₁(n) for n entries

## Configuration

### Bloom Filter Size

The Bloom filter size can be configured in `HashtableBuild`:

```cpp
HashtableBuild(Hashtable& ht, size_t cardEstimate, size_t bloomFilterBits = 0);
```

- `bloomFilterBits = 0`: size (10 bits per key)
- Customizable number of bits for filter

### Parallelism Thresholds

- Bloom filter parallel adds: n > 256
- Leaf page parallel creation: numLeafPages > 16
- Internal page parallel creation: numParentPages > 16

## Backward Compatibility

Backward compatibility by aliasing types.

```cpp
using BT = Hashtable;
using BTBuild = HashtableBuild;
using BTProbe = HashtableProbe;
```

Existing code using `BT`, `BTBuild`, and `BTProbe` continues to work without modification.

## Testing

All 20 unit tests pass (1,694,248 assertions), including:
- Join filter tests (Bloom filter + precise lookup)
- Hash join correctness tests
- Various relation size combinations
