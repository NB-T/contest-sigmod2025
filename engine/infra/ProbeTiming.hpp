#pragma once
//---------------------------------------------------------------------------
#include <atomic>
#include <chrono>
#include <cstdint>
//---------------------------------------------------------------------------
namespace engine {
//---------------------------------------------------------------------------
/// Low-overhead timer using steady_clock for hot-path timing
struct FastTimer {
    std::chrono::steady_clock::time_point start_time;

    [[gnu::always_inline]] void start() {
        start_time = std::chrono::steady_clock::now();
    }

    [[gnu::always_inline]] uint64_t elapsedNs() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_time).count();
    }
};
//---------------------------------------------------------------------------
/// Probe timing statistics - uses thread-local accumulators for low overhead
struct ProbeTiming {
    /// Whether probe timing is enabled (defined in ProbeTiming.cpp)
    static bool enabled;

    /// Enable probe timing
    static void enable() { enabled = true; }
    /// Disable probe timing
    static void disable() { enabled = false; }
    /// Is probe timing enabled?
    static bool isEnabled() { return enabled; }

    // Thread-local accumulators (no synchronization during hot path)
    static inline thread_local uint64_t tl_probe_count = 0;
    static inline thread_local uint64_t tl_bloom_check_ns = 0;
    static inline thread_local uint64_t tl_bloom_reject_count = 0;
    static inline thread_local uint64_t tl_tree_traverse_ns = 0;
    static inline thread_local uint64_t tl_tree_traverse_count = 0;
    static inline thread_local uint64_t tl_leaf_search_ns = 0;
    static inline thread_local uint64_t tl_leaf_pages_visited = 0;

    // Global aggregated atomics (merged from thread-local at end of workers, defined in ProbeTiming.cpp)
    static std::atomic<uint64_t> total_probe_count;
    static std::atomic<uint64_t> total_bloom_check_ns;
    static std::atomic<uint64_t> total_bloom_reject_count;
    static std::atomic<uint64_t> total_tree_traverse_ns;
    static std::atomic<uint64_t> total_tree_traverse_count;
    static std::atomic<uint64_t> total_leaf_search_ns;
    static std::atomic<uint64_t> total_leaf_pages_visited;

    /// Flush thread-local accumulators to global atomics (call at end of each worker)
    static void flushThreadLocal() {
        total_probe_count.fetch_add(tl_probe_count, std::memory_order_relaxed);
        total_bloom_check_ns.fetch_add(tl_bloom_check_ns, std::memory_order_relaxed);
        total_bloom_reject_count.fetch_add(tl_bloom_reject_count, std::memory_order_relaxed);
        total_tree_traverse_ns.fetch_add(tl_tree_traverse_ns, std::memory_order_relaxed);
        total_tree_traverse_count.fetch_add(tl_tree_traverse_count, std::memory_order_relaxed);
        total_leaf_search_ns.fetch_add(tl_leaf_search_ns, std::memory_order_relaxed);
        total_leaf_pages_visited.fetch_add(tl_leaf_pages_visited, std::memory_order_relaxed);

        // Reset thread-local
        tl_probe_count = 0;
        tl_bloom_check_ns = 0;
        tl_bloom_reject_count = 0;
        tl_tree_traverse_ns = 0;
        tl_tree_traverse_count = 0;
        tl_leaf_search_ns = 0;
        tl_leaf_pages_visited = 0;
    }

    /// Reset all timing statistics
    static void reset() {
        total_probe_count.store(0, std::memory_order_relaxed);
        total_bloom_check_ns.store(0, std::memory_order_relaxed);
        total_bloom_reject_count.store(0, std::memory_order_relaxed);
        total_tree_traverse_ns.store(0, std::memory_order_relaxed);
        total_tree_traverse_count.store(0, std::memory_order_relaxed);
        total_leaf_search_ns.store(0, std::memory_order_relaxed);
        total_leaf_pages_visited.store(0, std::memory_order_relaxed);
        // Also reset thread-local (for current thread)
        tl_probe_count = 0;
        tl_bloom_check_ns = 0;
        tl_bloom_reject_count = 0;
        tl_tree_traverse_ns = 0;
        tl_tree_traverse_count = 0;
        tl_leaf_search_ns = 0;
        tl_leaf_pages_visited = 0;
    }

    /// Record accumulated probe stats as JoinTiming entries for the current pipeline
    static void recordToJoinTiming();
};
//---------------------------------------------------------------------------
} // namespace engine
//---------------------------------------------------------------------------
