#include "infra/ProbeTiming.hpp"
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
std::atomic<uint64_t> ProbeTiming::total_consumer_invoke_ns{0};
std::atomic<uint64_t> ProbeTiming::total_consumer_invoke_count{0};
std::atomic<uint64_t> ProbeTiming::total_probe_ns{0};

// Thread-local accumulators - these are fine as inline since they're per-thread
// No need to define them here as thread_local inline works correctly
//---------------------------------------------------------------------------
} // namespace engine
//---------------------------------------------------------------------------
