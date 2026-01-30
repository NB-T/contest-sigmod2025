#pragma once

#include "infra/QueryMemory.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
namespace engine {
/// Bloom filter with total bits parameter
class BloomFilter {
    public:
    static constexpr size_t NUM_HASHES = 3;

    private:
    uint64_t* bits_ = nullptr;
    size_t total_bits_ = 0;
    size_t num_words_ = 0;

    // murmurhash3 mixer
    [[gnu::always_inline]] static inline uint64_t fmix64(uint64_t k) {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return k;
    }

    public:
    BloomFilter() = default;

    void allocate(size_t total_bits) {
        total_bits_ = total_bits;
        num_words_ = (total_bits + 63) / 64;
        bits_ = static_cast<uint64_t*>(querymemory::allocate(num_words_ * sizeof(uint64_t)));
        // Zero-initialize
        for (size_t i = 0; i < num_words_; ++i) {
            bits_[i] = 0;
        }
    }

    // add key (not thread-safe)
    [[gnu::always_inline]] inline void add(uint64_t key) {
        uint64_t h1 = fmix64(key);
        uint64_t h2 = fmix64(key + 0x9e3779b97f4a7c15ULL); // golden ratio constant
        for (size_t i = 0; i < NUM_HASHES; ++i) {
            size_t bit_pos = (h1 + i * h2) % total_bits_;
            size_t word = bit_pos / 64;
            uint64_t mask = 1ULL << (bit_pos % 64);
            bits_[word] |= mask;
        }
    }

    // false positives possible
    [[gnu::always_inline]] [[nodiscard]] inline bool mayContain(uint64_t key) const {
        if (!bits_) return false;
        uint64_t h1 = fmix64(key);
        uint64_t h2 = fmix64(key + 0x9e3779b97f4a7c15ULL);
        for (size_t i = 0; i < NUM_HASHES; ++i) {
            size_t bit_pos = (h1 + i * h2) % total_bits_;
            size_t word = bit_pos / 64;
            uint64_t mask = 1ULL << (bit_pos % 64);
            if (!(bits_[word] & mask)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] size_t getTotalBits() const { return total_bits_; }

    [[nodiscard]] bool isAllocated() const { return bits_ != nullptr; }

    [[nodiscard]] size_t countBitsSet() const {
        size_t count = 0;
        for (size_t i = 0; i < num_words_; ++i) {
            count += __builtin_popcountll(bits_[i]);
        }
        return count;
    }
};
}