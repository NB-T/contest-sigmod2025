#include "infra/ProbeTiming.hpp"
#include "infra/JoinTiming.hpp"
//---------------------------------------------------------------------------
namespace engine {
//---------------------------------------------------------------------------
// Static member definitions for ProbeTiming
bool ProbeTiming::enabled = false;

// Global aggregated atomics
std::atomic<uint64_t> ProbeTiming::total_probe_count{0};
std::atomic<uint64_t> ProbeTiming::total_bloom_check_ns{0};
std::atomic<uint64_t> ProbeTiming::total_bloom_reject_count{0};
std::atomic<uint64_t> ProbeTiming::total_tree_traverse_ns{0};
std::atomic<uint64_t> ProbeTiming::total_tree_traverse_count{0};
std::atomic<uint64_t> ProbeTiming::total_leaf_search_ns{0};
std::atomic<uint64_t> ProbeTiming::total_leaf_pages_visited{0};
//---------------------------------------------------------------------------
void ProbeTiming::recordToJoinTiming() {
    auto probes = total_probe_count.load(std::memory_order_relaxed);
    if (probes == 0) return;

    auto bloom_ns = total_bloom_check_ns.load(std::memory_order_relaxed);
    auto bloom_rejects = total_bloom_reject_count.load(std::memory_order_relaxed);
    auto tree_ns = total_tree_traverse_ns.load(std::memory_order_relaxed);
    auto tree_count = total_tree_traverse_count.load(std::memory_order_relaxed);
    auto leaf_ns = total_leaf_search_ns.load(std::memory_order_relaxed);
    auto leaf_pages = total_leaf_pages_visited.load(std::memory_order_relaxed);
    auto total_ns = bloom_ns + tree_ns + leaf_ns;

    JoinTiming::record("probeBloomFilter",
        "total_ns=" + std::to_string(bloom_ns) +
        " probes=" + std::to_string(probes) +
        " rejects=" + std::to_string(bloom_rejects),
        bloom_ns / 1000000);

    JoinTiming::record("probeTreeTraversal",
        "total_ns=" + std::to_string(tree_ns) +
        " count=" + std::to_string(tree_count),
        tree_ns / 1000000);

    JoinTiming::record("probeLeafSearch",
        "total_ns=" + std::to_string(leaf_ns) +
        " pages=" + std::to_string(leaf_pages),
        leaf_ns / 1000000);

    JoinTiming::record("probeTotal",
        "total_ns=" + std::to_string(total_ns) +
        " probes=" + std::to_string(probes),
        total_ns / 1000000);
}
//---------------------------------------------------------------------------
} // namespace engine
//---------------------------------------------------------------------------
