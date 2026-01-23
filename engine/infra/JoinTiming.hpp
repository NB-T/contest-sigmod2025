#pragma once
//---------------------------------------------------------------------------
#include "infra/ProbeTiming.hpp"
#include <nbtlog/NBTlog.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
//---------------------------------------------------------------------------
namespace engine {
//---------------------------------------------------------------------------
/// Global timing statistics for join phases
struct JoinTiming {
    /// Timing entry for a single phase
    struct Entry {
        std::string phase;
        std::string detail;
        size_t duration_ms;
        size_t pipeline_id;
    };

    /// All timing entries
    static inline std::vector<Entry> entries;
    /// Mutex for thread-safe access
    static inline std::mutex mutex;
    /// Current pipeline ID
    static inline std::atomic<size_t> current_pipeline{0};
    /// Whether timing is enabled
    static inline bool enabled = false;

    /// Enable timing
    static void enable() { enabled = true; }
    /// Disable timing
    static void disable() { enabled = false; }
    /// Is timing enabled?
    static bool isEnabled() { return enabled; }

    /// Clear all entries
    static void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        entries.clear();
        current_pipeline.store(0);
    }

    /// Start a new pipeline
    static size_t startPipeline() {
        return current_pipeline.fetch_add(1);
    }

    /// Record a timing entry
    static void record(const std::string& phase, const std::string& detail, size_t duration_ms, size_t pipeline_id = ~0u) {
        if (!enabled) return;
        std::lock_guard<std::mutex> lock(mutex);
        if (pipeline_id == ~0u) pipeline_id = current_pipeline.load();
        entries.push_back({phase, detail, duration_ms, pipeline_id});
    }

    /// Print all timing entries to stderr
    static void print() {
        std::lock_guard<std::mutex> lock(mutex);
        std::cerr << "\n========== JOIN TIMING REPORT ==========\n";
        for (const auto& e : entries) {
            std::cerr << "[Pipeline " << e.pipeline_id << "] "
                      << e.phase;
            if (!e.detail.empty()) {
                std::cerr << " (" << e.detail << ")";
            }
            std::cerr << ": " << e.duration_ms << " ms\n";
        }
        std::cerr << "=========================================\n";
    }

    /// Print all timing entries to a file
    static void printToFile(FILE* f) {
        std::lock_guard<std::mutex> lock(mutex);
        fprintf(f, "\n========== JOIN TIMING REPORT ==========\n");
        for (const auto& e : entries) {
            fprintf(f, "[Pipeline %zu] %s", e.pipeline_id, e.phase.c_str());
            if (!e.detail.empty()) {
                fprintf(f, " (%s)", e.detail.c_str());
            }
            fprintf(f, ": %zu ms\n", e.duration_ms);
        }
        fprintf(f, "=========================================\n");
        fflush(f);
    }

    /// Get CSV output
    static std::string toCSV() {
        std::lock_guard<std::mutex> lock(mutex);
        std::string result = "pipeline_id,phase,detail,duration_ms\n";
        for (const auto& e : entries) {
            result += std::to_string(e.pipeline_id) + "," + e.phase + ",\"" + e.detail + "\"," + std::to_string(e.duration_ms) + "\n";
        }
        return result;
    }
};
//---------------------------------------------------------------------------
/// RAII timer that records duration on destruction
class ScopedTimer {
    NBTlog timer;
    std::string phase;
    std::string detail;
    size_t pipeline_id;

public:
    ScopedTimer(const std::string& phase, const std::string& detail = "", size_t pipeline_id = ~0u)
        : phase(phase), detail(detail), pipeline_id(pipeline_id) {
        timer.start();
    }

    ~ScopedTimer() {
        if (JoinTiming::isEnabled()) {
            auto duration = timer._duration_();
            JoinTiming::record(phase, detail, duration, pipeline_id);
        }
    }

    /// Get elapsed time without stopping
    size_t elapsed() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - timer.START_TIME).count();
    }
};
//---------------------------------------------------------------------------
/// Manual timer for more control
class ManualTimer {
    NBTlog timer;

public:
    ManualTimer() { timer.start(); }

    void start() { timer.start(); }

    size_t stop() {
        return timer._duration_();
    }

    void record(const std::string& phase, const std::string& detail = "", size_t pipeline_id = ~0u) {
        if (JoinTiming::isEnabled()) {
            JoinTiming::record(phase, detail, stop(), pipeline_id);
        }
    }
};
//---------------------------------------------------------------------------
} // namespace engine
//---------------------------------------------------------------------------
