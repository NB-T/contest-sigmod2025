#pragma once
//---------------------------------------------------------------------------
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
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
    static inline thread_local uint64_t tl_hash_lookup_ns = 0;
    static inline thread_local uint64_t tl_chain_traverse_ns = 0;
    static inline thread_local uint64_t tl_chain_traverse_count = 0;
    static inline thread_local uint64_t tl_consumer_invoke_ns = 0;
    static inline thread_local uint64_t tl_consumer_invoke_count = 0;
    static inline thread_local uint64_t tl_total_probe_ns = 0;

    // Global aggregated atomics (merged from thread-local at end of workers, defined in ProbeTiming.cpp)
    static std::atomic<uint64_t> total_probe_count;
    static std::atomic<uint64_t> total_bloom_check_ns;
    static std::atomic<uint64_t> total_bloom_reject_count;
    static std::atomic<uint64_t> total_hash_lookup_ns;
    static std::atomic<uint64_t> total_chain_traverse_ns;
    static std::atomic<uint64_t> total_chain_traverse_count;
    static std::atomic<uint64_t> total_consumer_invoke_ns;
    static std::atomic<uint64_t> total_consumer_invoke_count;
    static std::atomic<uint64_t> total_probe_ns;

    /// Flush thread-local accumulators to global atomics (call at end of each worker)
    static void flushThreadLocal() {
        total_probe_count.fetch_add(tl_probe_count, std::memory_order_relaxed);
        total_bloom_check_ns.fetch_add(tl_bloom_check_ns, std::memory_order_relaxed);
        total_bloom_reject_count.fetch_add(tl_bloom_reject_count, std::memory_order_relaxed);
        total_hash_lookup_ns.fetch_add(tl_hash_lookup_ns, std::memory_order_relaxed);
        total_chain_traverse_ns.fetch_add(tl_chain_traverse_ns, std::memory_order_relaxed);
        total_chain_traverse_count.fetch_add(tl_chain_traverse_count, std::memory_order_relaxed);
        total_consumer_invoke_ns.fetch_add(tl_consumer_invoke_ns, std::memory_order_relaxed);
        total_consumer_invoke_count.fetch_add(tl_consumer_invoke_count, std::memory_order_relaxed);
        total_probe_ns.fetch_add(tl_total_probe_ns, std::memory_order_relaxed);

        // Reset thread-local
        tl_probe_count = 0;
        tl_bloom_check_ns = 0;
        tl_bloom_reject_count = 0;
        tl_hash_lookup_ns = 0;
        tl_chain_traverse_ns = 0;
        tl_chain_traverse_count = 0;
        tl_consumer_invoke_ns = 0;
        tl_consumer_invoke_count = 0;
        tl_total_probe_ns = 0;
    }

    /// Reset all timing statistics
    static void reset() {
        total_probe_count.store(0, std::memory_order_relaxed);
        total_bloom_check_ns.store(0, std::memory_order_relaxed);
        total_bloom_reject_count.store(0, std::memory_order_relaxed);
        total_hash_lookup_ns.store(0, std::memory_order_relaxed);
        total_chain_traverse_ns.store(0, std::memory_order_relaxed);
        total_chain_traverse_count.store(0, std::memory_order_relaxed);
        total_consumer_invoke_ns.store(0, std::memory_order_relaxed);
        total_consumer_invoke_count.store(0, std::memory_order_relaxed);
        total_probe_ns.store(0, std::memory_order_relaxed);
        // Also reset thread-local (for current thread)
        tl_probe_count = 0;
        tl_bloom_check_ns = 0;
        tl_bloom_reject_count = 0;
        tl_hash_lookup_ns = 0;
        tl_chain_traverse_ns = 0;
        tl_chain_traverse_count = 0;
        tl_consumer_invoke_ns = 0;
        tl_consumer_invoke_count = 0;
        tl_total_probe_ns = 0;
    }

    /// Print probe timing statistics
    static void print() {
        auto probes = total_probe_count.load();
        auto bloom_ns = total_bloom_check_ns.load();
        auto bloom_rejects = total_bloom_reject_count.load();
        auto hash_ns = total_hash_lookup_ns.load();
        auto chain_ns = total_chain_traverse_ns.load();
        auto chain_count = total_chain_traverse_count.load();
        auto consumer_ns = total_consumer_invoke_ns.load();
        auto consumer_count = total_consumer_invoke_count.load();
        auto total_ns = total_probe_ns.load();

        if (probes == 0) return;

        std::cerr << "\n========== PROBE TIMING BREAKDOWN ==========\n";
        std::cerr << "Total probes:           " << probes << "\n";
        std::cerr << "Bloom filter:\n";
        std::cerr << "  Total time:           " << (bloom_ns / 1000000.0) << " ms\n";
        std::cerr << "  Avg per probe:        " << (bloom_ns / (double)probes) << " ns\n";
        std::cerr << "  Rejections:           " << bloom_rejects << " ("
                  << (100.0 * bloom_rejects / probes) << "%)\n";
        std::cerr << "Hash lookup:\n";
        std::cerr << "  Total time:           " << (hash_ns / 1000000.0) << " ms\n";
        if (probes - bloom_rejects > 0) {
            std::cerr << "  Avg per lookup:       " << (hash_ns / (double)(probes - bloom_rejects)) << " ns\n";
        }
        std::cerr << "Chain traversal:\n";
        std::cerr << "  Total time:           " << (chain_ns / 1000000.0) << " ms\n";
        std::cerr << "  Traversals:           " << chain_count << "\n";
        if (chain_count > 0) {
            std::cerr << "  Avg per traversal:    " << (chain_ns / (double)chain_count) << " ns\n";
        }
        std::cerr << "Consumer invocation:\n";
        std::cerr << "  Total time:           " << (consumer_ns / 1000000.0) << " ms\n";
        std::cerr << "  Invocations:          " << consumer_count << "\n";
        if (consumer_count > 0) {
            std::cerr << "  Avg per invocation:   " << (consumer_ns / (double)consumer_count) << " ns\n";
        }
        std::cerr << "Total probe time:       " << (total_ns / 1000000.0) << " ms\n";
        std::cerr << "  Avg per probe:        " << (total_ns / (double)probes) << " ns\n";

        // Breakdown percentages
        if (total_ns > 0) {
            std::cerr << "Time breakdown:\n";
            std::cerr << "  Bloom filter:         " << (100.0 * bloom_ns / total_ns) << "%\n";
            std::cerr << "  Hash lookup:          " << (100.0 * hash_ns / total_ns) << "%\n";
            std::cerr << "  Chain traversal:      " << (100.0 * chain_ns / total_ns) << "%\n";
            std::cerr << "  Consumer:             " << (100.0 * consumer_ns / total_ns) << "%\n";
            auto overhead = total_ns - bloom_ns - hash_ns - chain_ns - consumer_ns;
            std::cerr << "  Overhead/other:       " << (100.0 * overhead / total_ns) << "%\n";
        }
        std::cerr << "=============================================\n";
    }

    /// Print to file
    static void printToFile(FILE* f) {
        auto probes = total_probe_count.load();
        auto bloom_ns = total_bloom_check_ns.load();
        auto bloom_rejects = total_bloom_reject_count.load();
        auto hash_ns = total_hash_lookup_ns.load();
        auto chain_ns = total_chain_traverse_ns.load();
        auto chain_count = total_chain_traverse_count.load();
        auto consumer_ns = total_consumer_invoke_ns.load();
        auto consumer_count = total_consumer_invoke_count.load();
        auto total_ns = total_probe_ns.load();

        if (probes == 0) return;

        fprintf(f, "\n========== PROBE TIMING BREAKDOWN ==========\n");
        fprintf(f, "Total probes:           %zu\n", (size_t)probes);
        fprintf(f, "Bloom filter:\n");
        fprintf(f, "  Total time:           %.3f ms\n", bloom_ns / 1000000.0);
        fprintf(f, "  Avg per probe:        %.1f ns\n", bloom_ns / (double)probes);
        fprintf(f, "  Rejections:           %zu (%.1f%%)\n", (size_t)bloom_rejects, 100.0 * bloom_rejects / probes);
        fprintf(f, "Hash lookup:\n");
        fprintf(f, "  Total time:           %.3f ms\n", hash_ns / 1000000.0);
        if (probes - bloom_rejects > 0) {
            fprintf(f, "  Avg per lookup:       %.1f ns\n", hash_ns / (double)(probes - bloom_rejects));
        }
        fprintf(f, "Chain traversal:\n");
        fprintf(f, "  Total time:           %.3f ms\n", chain_ns / 1000000.0);
        fprintf(f, "  Traversals:           %zu\n", (size_t)chain_count);
        if (chain_count > 0) {
            fprintf(f, "  Avg per traversal:    %.1f ns\n", chain_ns / (double)chain_count);
        }
        fprintf(f, "Consumer invocation:\n");
        fprintf(f, "  Total time:           %.3f ms\n", consumer_ns / 1000000.0);
        fprintf(f, "  Invocations:          %zu\n", (size_t)consumer_count);
        if (consumer_count > 0) {
            fprintf(f, "  Avg per invocation:   %.1f ns\n", consumer_ns / (double)consumer_count);
        }
        fprintf(f, "Total probe time:       %.3f ms\n", total_ns / 1000000.0);
        fprintf(f, "  Avg per probe:        %.1f ns\n", total_ns / (double)probes);

        if (total_ns > 0) {
            fprintf(f, "Time breakdown:\n");
            fprintf(f, "  Bloom filter:         %.1f%%\n", 100.0 * bloom_ns / total_ns);
            fprintf(f, "  Hash lookup:          %.1f%%\n", 100.0 * hash_ns / total_ns);
            fprintf(f, "  Chain traversal:      %.1f%%\n", 100.0 * chain_ns / total_ns);
            fprintf(f, "  Consumer:             %.1f%%\n", 100.0 * consumer_ns / total_ns);
            auto overhead = total_ns - bloom_ns - hash_ns - chain_ns - consumer_ns;
            fprintf(f, "  Overhead/other:       %.1f%%\n", 100.0 * overhead / total_ns);
        }
        fprintf(f, "=============================================\n");
        fflush(f);
    }
};
//---------------------------------------------------------------------------
} // namespace engine
//---------------------------------------------------------------------------
