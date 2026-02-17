#pragma once
//---------------------------------------------------------------------------
#include "Config.hpp"
#include "infra/QueryMemory.hpp"
#include "infra/Scheduler.hpp"
#include "infra/Util.hpp"
#include "op/Hashtable.hpp"
#include "pipeline/JoinPipeline.hpp"

#include <parlay/parallel.h>
#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <parlay/slice.h>
#include <parlay/internal/semisort.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <tuple>
#include <type_traits>
#include <vector>
//---------------------------------------------------------------------------
namespace engine {
//---------------------------------------------------------------------------
/// Entry in the semisort buffer: a probe key paired with the row index
struct ProbeEntry {
    uint64_t key;
    uint32_t row_id;
};
//---------------------------------------------------------------------------
/// Batched join pipeline that materializes rows, semisorts by probe key,
/// deduplicates bloom checks per unique key group, and shares tree traversals.
template <typename Target, typename Scan, typename Probes, typename Keys, typename Attrs>
struct BatchedJoinPipeline;
//---------------------------------------------------------------------------
template <typename Target, typename Scan, typename... Probes, size_t... Keys, size_t... Attrs>
struct BatchedJoinPipeline<Target, Scan, std::tuple<Probes...>, std::index_sequence<Keys...>, std::index_sequence<Attrs...>> {
    static_assert(sizeof...(Probes) > 0, "BatchedJoinPipeline requires at least one probe");

    /// The probe operators (HashtableProbe instances)
    std::tuple<Probes...> probes;
    /// Key offsets within each relation
    std::array<unsigned, sizeof...(Keys)> keyOffsets;
    /// Output attribute offsets
    std::array<unsigned, sizeof...(Attrs)> attrOffsets;
    /// Target operator
    Target& target;
    /// Scan operator
    Scan& scan;

    static constexpr size_t NumProbes = sizeof...(Probes);
    static constexpr size_t NumAttrs = sizeof...(Attrs);
    static constexpr size_t CHUNK_SIZE = 1 << 16; // 64K entries per chunk

    /// Which relation provides each key
    static constexpr std::array<size_t, NumProbes> keySources = {Keys...};
    /// Which relation provides each output attribute
    static constexpr std::array<size_t, NumAttrs> attrSources = {Attrs...};

    /// Constructor
    BatchedJoinPipeline(Target& target, Scan& scan, decltype(probes) probes,
                        decltype(keyOffsets) keyOffsets, decltype(attrOffsets) attrOffsets)
        : probes(probes), keyOffsets(keyOffsets), attrOffsets(attrOffsets),
          target(target), scan(scan) {
        if constexpr (config::handleMultiplicity) {
            for (size_t i = 0; i < NumProbes; i++)
                this->keyOffsets[i] += keySources[i] != 0;
            for (size_t i = 0; i < NumAttrs; i++)
                this->attrOffsets[i] += attrSources[i] != 0;
        }
    }

    /// Get the Hashtable pointer for probe at index Ind
    template <size_t Ind>
    const Hashtable* getHT() const {
        return std::get<Ind>(probes).ht;
    }

    // -----------------------------------------------------------------------
    // Helper: count how many scan columns the provider produces.
    // We learn this at runtime from the scan, but for materialization we need
    // to know the maximum column index accessed from relation 0.
    // We compute it from keyOffsets and attrOffsets for relation-0 references.
    // -----------------------------------------------------------------------
    size_t computeScanCols() const {
        size_t maxCol = 0;
        // Keys referencing relation 0
        for (size_t i = 0; i < NumProbes; i++) {
            if (keySources[i] == 0) {
                maxCol = std::max(maxCol, (size_t)keyOffsets[i] + 1);
            }
        }
        // Attrs referencing relation 0
        for (size_t i = 0; i < NumAttrs; i++) {
            if (attrSources[i] == 0) {
                maxCol = std::max(maxCol, (size_t)attrOffsets[i] + 1);
            }
        }
        return maxCol;
    }

    /// Compute how many columns a given probe relation contributes.
    /// Probe relation r (1-based) needs columns referenced by later probes' keys
    /// and by output attributes.
    size_t computeProbeCols(size_t probeRelation) const {
        size_t maxCol = 0;
        for (size_t i = 0; i < NumProbes; i++) {
            if (keySources[i] == probeRelation)
                maxCol = std::max(maxCol, (size_t)keyOffsets[i] + 1);
        }
        for (size_t i = 0; i < NumAttrs; i++) {
            if (attrSources[i] == probeRelation)
                maxCol = std::max(maxCol, (size_t)attrOffsets[i] + 1);
        }
        return maxCol;
    }

    // -----------------------------------------------------------------------
    // Main entry point
    // -----------------------------------------------------------------------
    void operator()() {
        size_t scanCols = computeScanCols();

        // Step 1: Scan + Materialize all scan rows into a flat buffer
        // Use thread-local buffers, then merge.

        size_t numWorkers = scan.concurrency() == 1 ? 1 : Scheduler::concurrency();

        struct alignas(hardwareCachelineSize) WorkerBuf {
            Vector<uint64_t> rows;
            size_t count = 0;
        };
        auto workerBufs = std::make_unique<WorkerBuf[]>(numWorkers);

        // We need a JoinPipeline-compatible LocalState for the scan
        // But we only use the scan portion. We'll create a minimal struct.
        struct alignas(hardwareCachelineSize) ScanLocalState {
            bool initialized = false;
            size_t workerId;
            typename Scan::LocalState scan;
            ScanLocalState(size_t wid, Scan& s) : workerId(wid), scan(s) {}
        };

        LocalStateContainer<ScanLocalState> scanStates(numWorkers);

        scan(
            // getLocalState
            [&](size_t workerId) {
                return scanStates.get(workerId);
            },
            // consume: materialize each scanned row
            [&](void* localStateRaw, auto provider) __attribute__((always_inline)) {
                auto* ls = static_cast<ScanLocalState*>(localStateRaw);
                auto& buf = workerBufs[ls->workerId];
                for (size_t c = 0; c < scanCols; c++)
                    buf.rows.push_back(provider(c));
                buf.count++;
            },
            // prepare: no-op for batched mode
            [&](void* /*localStateRaw*/, uint64_t /*key*/) __attribute__((always_inline)) {
            },
            // init
            [&](size_t workerId, void* localStateRaw, bool init) {
                auto* ls = static_cast<ScanLocalState*>(localStateRaw);
                if (init) {
                    new (ls) ScanLocalState(workerId, scan);
                    ls->initialized = true;
                } else {
                    ls->~ScanLocalState();
                }
            });

        // Merge thread-local buffers into single materialized array
        size_t totalRows = 0;
        for (size_t w = 0; w < numWorkers; w++)
            totalRows += workerBufs[w].count;

        if (totalRows == 0) {
            callFinishConsume(target);
            return;
        }

        // Current materialized buffer: totalRows * scanCols
        size_t curCols = scanCols;
        auto materialized = parlay::sequence<uint64_t>(totalRows * curCols);

        // Compute offsets and copy
        {
            size_t offset = 0;
            for (size_t w = 0; w < numWorkers; w++) {
                auto& buf = workerBufs[w];
                if (buf.count > 0) {
                    std::memcpy(&materialized[offset * curCols], buf.rows.data(),
                                buf.count * curCols * sizeof(uint64_t));
                    offset += buf.count;
                }
                // Free worker buffer
                buf.rows.clear();
            }
        }

        // Now process each probe level sequentially
        size_t curRows = totalRows;
        // Track column offsets for each relation in the materialized buffer
        // relation 0 (scan) starts at offset 0
        // relation 1 (probe 0 results) starts at scanCols
        // relation 2 (probe 1 results) starts at scanCols + probe0Cols
        // etc.
        std::array<size_t, NumProbes + 1> relationOffsets;
        relationOffsets[0] = 0; // scan columns start at 0

        for (size_t probeIdx = 0; probeIdx < NumProbes; probeIdx++) {
            processProbeLevel(probeIdx, materialized, curRows, curCols, relationOffsets);
            if (curRows == 0) break;
        }

        // Step 6: Emit final results to target
        if (curRows > 0) {
            emitToTarget(materialized, curRows, curCols, relationOffsets);
        }

        callFinishConsume(target);
    }

private:
    /// Process a single probe level: build probe buffer, semisort, bloom sweep, tree traverse
    void processProbeLevel(size_t probeIdx, parlay::sequence<uint64_t>& materialized,
                           size_t& curRows, size_t& curCols,
                           std::array<size_t, NumProbes + 1>& relationOffsets) {
        // Dispatch to the correct compile-time probe index
        processProbeDispatch(probeIdx, materialized, curRows, curCols, relationOffsets,
                            std::make_index_sequence<NumProbes>{});
    }

    template <size_t... Is>
    void processProbeDispatch(size_t probeIdx, parlay::sequence<uint64_t>& materialized,
                              size_t& curRows, size_t& curCols,
                              std::array<size_t, NumProbes + 1>& relationOffsets,
                              std::index_sequence<Is...>) {
        // Use a fold to dispatch runtime index to compile-time
        ((probeIdx == Is ? (processProbeImpl<Is>(materialized, curRows, curCols, relationOffsets), 0) : 0), ...);
    }

    template <size_t Ind>
    void processProbeImpl(parlay::sequence<uint64_t>& materialized,
                          size_t& curRows, size_t& curCols,
                          std::array<size_t, NumProbes + 1>& relationOffsets) {
        const Hashtable* ht = getHT<Ind>();
        size_t N = curRows;

        // Determine which column in materialized[] holds this probe's key
        constexpr size_t keyRelation = keySources[Ind];
        size_t keyColInMat = relationOffsets[keyRelation] + keyOffsets[Ind];

        // Step 2: Build probe buffer
        auto probe_buf = parlay::sequence<ProbeEntry>(N);
        parlay::parallel_for(0, N, [&](size_t i) {
            probe_buf[i] = {materialized[i * curCols + keyColInMat], static_cast<uint32_t>(i)};
        });

        // Step 3: Semisort to group equal keys
        parlay::internal::semisort_equal_inplace(
            parlay::make_slice(probe_buf),
            [](const ProbeEntry& e) { return e.key; });

        // Step 4: Parallel bloom filter sweep with in-chunk compaction
        size_t num_chunks = (N + CHUNK_SIZE - 1) / CHUNK_SIZE;
        auto chunk_survivor_counts = parlay::sequence<size_t>(num_chunks);

        parlay::parallel_for(0, num_chunks, [&](size_t c) {
            size_t begin = c * CHUNK_SIZE;
            size_t end = std::min(begin + CHUNK_SIZE, N);
            size_t write = begin;

            size_t i = begin;
            while (i < end) {
                uint64_t key = probe_buf[i].key;
                size_t group_end = i + 1;
                while (group_end < end && probe_buf[group_end].key == key)
                    group_end++;

                // ONE bloom check for the entire group
                if (ht->bloom_filter_.mayContain(key)) {
                    for (size_t j = i; j < group_end; j++)
                        probe_buf[write++] = probe_buf[j];
                }

                i = group_end;
            }
            chunk_survivor_counts[c] = write - begin;
        });

        // Compact survivors across chunks
        auto offsets = parlay::scan(chunk_survivor_counts).first;
        size_t total_survivors = (num_chunks > 0)
            ? offsets[num_chunks - 1] + chunk_survivor_counts[num_chunks - 1]
            : 0;

        if (total_survivors == 0) {
            curRows = 0;
            return;
        }

        auto survivors = parlay::sequence<ProbeEntry>(total_survivors);
        parlay::parallel_for(0, num_chunks, [&](size_t c) {
            size_t src = c * CHUNK_SIZE;
            size_t dst = offsets[c];
            for (size_t j = 0; j < chunk_survivor_counts[c]; j++)
                survivors[dst + j] = probe_buf[src + j];
        });

        // Free probe_buf
        probe_buf.clear();

        // Step 5: Tree traversal + join expansion
        // Determine how many columns this probe contributes
        size_t probeCols = computeProbeCols(Ind + 1);
        size_t newCols = curCols + probeCols;

        // We need to expand: each surviving row may match multiple entries in the tree.
        // Process in chunks, collect results into thread-local buffers, then merge.
        size_t numSurvivorChunks = (total_survivors + CHUNK_SIZE - 1) / CHUNK_SIZE;

        struct alignas(hardwareCachelineSize) ChunkResult {
            std::vector<uint64_t> rows;
            size_t count = 0;
        };
        auto chunkResults = std::make_unique<ChunkResult[]>(numSurvivorChunks);

        parlay::parallel_for(0, numSurvivorChunks, [&](size_t c) {
            size_t begin = c * CHUNK_SIZE;
            size_t end = std::min(begin + CHUNK_SIZE, total_survivors);
            auto& result = chunkResults[c];

            size_t i = begin;
            while (i < end) {
                uint64_t key = survivors[i].key;
                size_t group_end = i + 1;
                while (group_end < end && survivors[group_end].key == key)
                    group_end++;

                // ONE tree traversal for this key group
                if (!ht->root_) {
                    i = group_end;
                    continue;
                }

                void* current = ht->root_;
                for (size_t level = ht->height_; level > 1; --level) {
                    auto* internal = static_cast<Hashtable::InternalPage*>(current);
#ifdef __AVX512F__
                    size_t lo = simdLowerBound(internal->keys, internal->header.num_keys, key);
#else
                    size_t lo = 0, nn = internal->header.num_keys;
                    while (nn > 1) {
                        size_t half = nn / 2;
                        lo += (internal->keys[lo + half - 1] < key) * half;
                        nn -= half;
                    }
                    lo += (nn == 1 && internal->keys[lo] < key);
#endif
                    current = internal->children[lo];
                }

                auto* leaf = static_cast<Hashtable::LeafPage*>(current);
#ifdef __AVX512F__
                size_t lo = simdLowerBound(leaf->keys, leaf->header.num_keys, key);
#else
                size_t lo = 0, nn = leaf->header.num_keys;
                while (nn > 1) {
                    size_t half = nn / 2;
                    lo += (leaf->keys[lo + half - 1] < key) * half;
                    nn -= half;
                }
                lo += (nn == 1 && leaf->keys[lo] < key);
#endif

                // Iterate through leaf chain for all matching entries
                while (leaf) {
                    for (size_t k = lo; k < leaf->header.num_keys; ++k) {
                        if (leaf->keys[k] > key) goto done_group;
                        if (leaf->keys[k] == key) {
                            auto* entry = leaf->entries[k];
                            if (entry) {
                                // Cross with ALL rows in this group
                                for (size_t r = i; r < group_end; r++) {
                                    uint32_t row_id = survivors[r].row_id;
                                    // Copy existing columns from source row
                                    for (size_t col = 0; col < curCols; col++)
                                        result.rows.push_back(materialized[row_id * curCols + col]);
                                    // Append probe result columns
                                    for (size_t col = 0; col < probeCols; col++)
                                        result.rows.push_back(entry->tuple[col]);
                                    result.count++;
                                }
                            }
                        }
                    }
                    if (leaf->header.next_page_idx == 0xFFFFFFFF) break;
                    leaf = ht->leaf_pages_[leaf->header.next_page_idx];
                    lo = 0;
                }
                done_group:

                i = group_end;
            }
        });

        // Merge chunk results into new materialized buffer
        size_t newTotalRows = 0;
        for (size_t c = 0; c < numSurvivorChunks; c++)
            newTotalRows += chunkResults[c].count;

        if (newTotalRows == 0) {
            curRows = 0;
            return;
        }

        auto newMaterialized = parlay::sequence<uint64_t>(newTotalRows * newCols);
        {
            size_t offset = 0;
            for (size_t c = 0; c < numSurvivorChunks; c++) {
                auto& cr = chunkResults[c];
                if (cr.count > 0) {
                    std::memcpy(&newMaterialized[offset * newCols], cr.rows.data(),
                                cr.count * newCols * sizeof(uint64_t));
                    offset += cr.count;
                }
            }
        }

        // Update state for next level
        materialized = std::move(newMaterialized);
        relationOffsets[Ind + 1] = curCols; // new probe's columns start where old ended
        curCols = newCols;
        curRows = newTotalRows;
    }

    /// Emit final materialized rows to the target
    void emitToTarget(const parlay::sequence<uint64_t>& materialized,
                      size_t curRows, size_t curCols,
                      const std::array<size_t, NumProbes + 1>& relationOffsets) {
        // Create a single target local state and emit all rows
        std::aligned_storage_t<sizeof(typename Target::LocalState),
                               alignof(typename Target::LocalState)> storage;
        auto* ls = reinterpret_cast<typename Target::LocalState*>(&storage);
        new (ls) typename Target::LocalState(target);

        for (size_t row = 0; row < curRows; row++) {
            emitRow(materialized, row, curCols, relationOffsets, *ls);
        }

        callFinalize(target, *ls);
    }

    /// Emit a single row to the target
    void emitRow(const parlay::sequence<uint64_t>& materialized,
                 size_t row, size_t curCols,
                 const std::array<size_t, NumProbes + 1>& relationOffsets,
                 typename Target::LocalState& ls) {
        emitRowImpl(materialized, row, curCols, relationOffsets, ls,
                    std::make_index_sequence<NumAttrs>{});
    }

    template <size_t... Is>
    void emitRowImpl(const parlay::sequence<uint64_t>& materialized,
                     size_t row, size_t curCols,
                     const std::array<size_t, NumProbes + 1>& relationOffsets,
                     typename Target::LocalState& ls,
                     std::index_sequence<Is...>) {
        uint64_t multiplicity = 1;
        target(ls, multiplicity,
               materialized[row * curCols + relationOffsets[attrSources[Is]] + attrOffsets[Is]]...);
    }

    // Helper for finishConsume / finalize (same pattern as JoinPipeline)
    template <typename T>
    static void callFinishConsume(T&& obj) {
        callFinishConsumeImpl(std::forward<T>(obj), 0);
    }
    template <typename T>
    static auto callFinishConsumeImpl(T&& obj, int) -> decltype(obj.finishConsume(), void()) {
        obj.finishConsume();
    }
    template <typename T>
    static void callFinishConsumeImpl(T&&, ...) {}

    template <typename T>
    static void callFinalize(T&& obj, typename Target::LocalState& ls) {
        callFinalizeImpl(std::forward<T>(obj), ls, 0);
    }
    template <typename T>
    static auto callFinalizeImpl(T&& obj, typename Target::LocalState& ls, int) -> decltype(obj.finalize(ls), void()) {
        obj.finalize(ls);
    }
    template <typename T>
    static void callFinalizeImpl(T&&, typename Target::LocalState&, ...) {}
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
