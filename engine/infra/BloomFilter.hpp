#pragma once

#include "infra/QueryMemory.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
namespace engine {
/// Cache-line-blocked Bloom filter: all 3 hash bits per key land in one
/// 512-bit (64-byte) block, so lookup is a single cache miss.
class BloomFilter {
    public:
    static constexpr size_t NUM_HASHES = 3;

    private:
    uint64_t* bits_ = nullptr;
    size_t total_bits_ = 0;
    size_t num_words_ = 0;
    size_t num_blocks_ = 0;
    size_t num_blocks_mask_ = 0;

    // murmurhash3 mixer
    [[gnu::always_inline]] static inline uint64_t fmix64(uint64_t k) {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return k;
    }

    // Round up to next power of 2 (returns v if already power of 2)
    static inline size_t nextPow2(size_t v) {
        if (v == 0) return 1;
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        return v + 1;
    }

    public:
    BloomFilter() = default;

    void allocate(size_t total_bits) {
        size_t min_blocks = (total_bits + 511) / 512;
        num_blocks_ = nextPow2(min_blocks);
        num_blocks_mask_ = num_blocks_ - 1;
        total_bits_ = num_blocks_ * 512;
        num_words_ = num_blocks_ * 8;
        bits_ = static_cast<uint64_t*>(querymemory::allocate(num_words_ * sizeof(uint64_t)));
        // Zero-initialize
        for (size_t i = 0; i < num_words_; ++i) {
            bits_[i] = 0;
        }
    }

    // add key — all 3 bits land in one 512-bit block
    [[gnu::always_inline]] inline void add(uint64_t key) {
        uint64_t h1 = fmix64(key);
        uint64_t h2 = fmix64(key + 0x9e3779b97f4a7c15ULL);
        uint64_t* block_ptr = bits_ + (h1 & num_blocks_mask_) * 8;
        for (size_t i = 0; i < NUM_HASHES; ++i) {
            size_t bit_pos = (h2 >> (i * 9)) & 0x1FF; // 9-bit field → [0, 511]
            // block_ptr[bit_pos >> 6] |= (1ULL << (bit_pos & 63));
            __atomic_or_fetch(block_ptr + (bit_pos >> 6), 1ULL << (bit_pos & 63), __ATOMIC_RELAXED);
        }
    }

    // false positives possible — single cache line access
    [[gnu::always_inline]] [[nodiscard]] inline bool mayContain(uint64_t key) const {
        if (!bits_) return false;
        uint64_t h1 = fmix64(key);
        uint64_t h2 = fmix64(key + 0x9e3779b97f4a7c15ULL);
        const uint64_t* block_ptr = bits_ + (h1 & num_blocks_mask_) * 8;
        for (size_t i = 0; i < NUM_HASHES; ++i) {
            size_t bit_pos = (h2 >> (i * 9)) & 0x1FF;
            if (!(block_ptr[bit_pos >> 6] & (1ULL << (bit_pos & 63)))) {
                return false;
            }
        }
        return true;
    }

    /// Prefetch the single cache line that mayContain() will access.
    [[gnu::always_inline]] inline void prefetchFor(uint64_t key) const {
        if (!bits_) return;
        uint64_t h1 = fmix64(key);
        __builtin_prefetch(bits_ + (h1 & num_blocks_mask_) * 8, 0, 0);
    }

    // Global bit index for hash function hash_i for a given key.
    // Requires the filter to be allocated first.
    [[nodiscard]] [[gnu::always_inline]]
    inline uint64_t globalBitIndex(uint64_t key, size_t hash_i) const {
        uint64_t h1 = fmix64(key);
        uint64_t h2 = fmix64(key + 0x9e3779b97f4a7c15ULL);
        size_t block_idx = h1 & num_blocks_mask_;
        size_t bit_pos   = (h2 >> (hash_i * 9)) & 0x1FF;
        return static_cast<uint64_t>(block_idx * 512 + bit_pos);
    }

    // Set a single bit by its global index.
    [[gnu::always_inline]]
    inline void setBit(uint64_t global_bit_idx) {
        /* __atomic_or_fetch(bits_ + (global_bit_idx >> 6),
                          1ULL << (global_bit_idx & 63),
                          __ATOMIC_RELAXED); */
        bits_[global_bit_idx >> 6] |= (1ULL << (global_bit_idx & 63));
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
