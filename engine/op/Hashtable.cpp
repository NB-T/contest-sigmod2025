#include "infra/JoinTiming.hpp"
#include "infra/QueryMemory.hpp"
#include "infra/Scheduler.hpp"
#include "infra/helper/BitOps.hpp"
#include "op/Hashtable.hpp"
#include "query/DataSource.hpp"
#include "query/RuntimeValue.hpp"

#include <parlay/internal/bucket_sort.h>
#include <parlay/internal/semisort.h>
#include <parlay/parallel.h>
#include <parlay/primitives.h>
#include <parlay/slice.h>

#include <algorithm>
#include <numeric>
#include <unordered_set>
//---------------------------------------------------------------------------
namespace engine {
//---------------------------------------------------------------------------
/// The size of a chunk for entry allocation
static constexpr size_t chunk_size = 8048;
/// The number of uint64_t values in a chunk (minus header)
static constexpr size_t chunk_count = chunk_size / sizeof(uint64_t) - 2;
/// The number of chunks in a block
static constexpr size_t block_chunks = HashtableBuild::maxPartitions - 1;
static_assert(block_chunks < (1ull << 16));
//---------------------------------------------------------------------------
/// A chunk for entry allocation
struct HashtableBuild::Chunk {
   /// The next chunk
   Chunk* next;
   /// Current position in data
   size_t pos;
   /// The data
   uint64_t data[chunk_count];
};
static_assert(sizeof(HashtableBuild::Chunk) == chunk_size);

/// A block containing multiple chunks
struct alignas(4096) HashtableBuild::Block {
   /// The next block
   Block* next;
   /// The next chunk to allocate
   size_t current_chunk;
   /// The chunks
   Chunk chunks[block_chunks];
};
//---------------------------------------------------------------------------
std::string HashtableBuild::getPretty() const {
   return "ht";
}
//---------------------------------------------------------------------------
std::string HashtableProbe::getPretty() const {
   return ht->pretty;
}
//---------------------------------------------------------------------------
// bloom filter false positive tracking
std::atomic<size_t> HashtableProbe::bloom_probes{0};
std::atomic<size_t> HashtableProbe::bloom_passes{0};
std::atomic<size_t> HashtableProbe::bloom_false_positives{0};

void HashtableProbe::resetBloomStats() {
   bloom_probes.store(0, std::memory_order_relaxed);
   bloom_passes.store(0, std::memory_order_relaxed);
   bloom_false_positives.store(0, std::memory_order_relaxed);
}

void HashtableProbe::printBloomStats() {
   auto probes = bloom_probes.load(std::memory_order_relaxed);
   auto passes = bloom_passes.load(std::memory_order_relaxed);
   auto fps = bloom_false_positives.load(std::memory_order_relaxed);
   auto true_positives = passes - fps;
   auto true_negatives = probes - passes;
   double fp_rate_among_passes = passes > 0 ? 100.0 * fps / passes : 0.0;
   double fp_rate_actual = (true_negatives + fps) > 0 ? 100.0 * fps / (true_negatives + fps) : 0.0;
   double rejection_rate = probes > 0 ? 100.0 * true_negatives / probes : 0.0;
   fprintf(stderr, "Bloom filter stats: probes=%zu, passes=%zu (%.1f%%), true_pos=%zu, false_pos=%zu\n",
           probes, passes, probes > 0 ? 100.0 * passes / probes : 0.0, true_positives, fps);
   fprintf(stderr, "  -> BF rejection rate: %.2f%%, Actual FP rate: %.4f%%, FP rate among passes: %.2f%%\n",
           rejection_rate, fp_rate_actual, fp_rate_among_passes);
   fflush(stderr);
}
//---------------------------------------------------------------------------
HashtableBuild::LocalState::LocalState(HashtableBuild& build) {
   next = build.local_state_refs.exchange(this);
}
//---------------------------------------------------------------------------
Hashtable::Entry* HashtableBuild::LocalState::allocateEntry(size_t attr_count_inp) {
   // entry has data and next pointer
   size_t entry_size = 1 + attr_count_inp; // next pointer counts as 1 uint64_t

   // if chunk has no space
   if (!chunk_pos || chunk_pos + entry_size > chunk_end) {
      // allocate new chunk
      if (!blocks || blocks->current_chunk == block_chunks) {
         // block
         static_assert(sizeof(Block) % DataSource::PAGE_SIZE == 0);
         auto* new_block = static_cast<Block*>(querymemory::allocate(sizeof(Block)));
         new_block->next = blocks;
         new_block->current_chunk = 0;
         blocks = new_block;
      }

      attr_count = attr_count_inp;

      // chunk
      auto* new_chunk = &blocks->chunks[blocks->current_chunk++];
      new_chunk->next = current_chunk;
      new_chunk->pos = 0;
      current_chunk = new_chunk;

      chunk_pos = new_chunk->data;
      chunk_end = new_chunk->data + chunk_count - (chunk_count % entry_size);
   }

   // allocate entry from current chunk
   auto* entry = reinterpret_cast<Hashtable::Entry*>(chunk_pos);
   chunk_pos += entry_size;
   return entry;
}
//---------------------------------------------------------------------------
void Hashtable::allocateHashtable(size_t num_elements) {
   // obsolete
}
//---------------------------------------------------------------------------
void Hashtable::filterEq(const EqRestriction& restriction) {
   // filter entries in leaf pages
   for (auto* leaf : leaf_pages_) {
      size_t write_pos = 0;
      for (size_t i = 0; i < leaf->header.num_keys; ++i) {
         auto* entry = leaf->entries[i];
         if (entry && entry->tuple[restriction.offset + 1] == restriction.value) {
            // Keep this entry
            if (write_pos != i) {
               leaf->keys[write_pos] = leaf->keys[i];
               leaf->entries[write_pos] = leaf->entries[i];
            }
            write_pos++;
         } else {
            // remove entry
            if (num_tuples > 0) num_tuples--;
         }
      }
      leaf->header.num_keys = write_pos;
   }

   // recount unique keys
   std::unordered_set<uint64_t> unique_keys;
   for (auto* leaf : leaf_pages_) {
      for (size_t i = 0; i < leaf->header.num_keys; ++i) {
         unique_keys.insert(leaf->keys[i]);
      }
   }
   num_keys = unique_keys.size();
}
//---------------------------------------------------------------------------
Vector<HashtableBuild::BufferEntry> HashtableBuild::collectAndSort() {
   // count
   size_t total_tuples = 0;
   for (auto* ls = local_state_refs.load(); ls; ls = ls->next) {
      total_tuples += ls->num_tuples;
   }

   if (total_tuples == 0) {
      return Vector<BufferEntry>();
   }

   // merge local buffers
   Vector<BufferEntry> all_entries;
   all_entries.reserve(total_tuples);

   for (auto* ls = local_state_refs.load(); ls; ls = ls->next) {
      for (const auto& entry : ls->buffer) {
         all_entries.push_back(entry);
      }
   }

   // sort with parlay::integer_sort_inplace
   auto slice = parlay::make_slice(all_entries.data(), all_entries.data() + all_entries.size());
   // parlay::integer_sort_inplace(slice, [](const BufferEntry& e) { return e.key; });

   // bucket sort
   // parlay::internal::bucket_sort(slice, [](const BufferEntry& a, const BufferEntry& b) { return a.key < b.key; }, false);

   // sample sort
   parlay::internal::sample_sort_inplace(slice, [](const BufferEntry& a, const BufferEntry& b) { return a.key < b.key; });

   // merge sort
   // parlay::internal::merge_sort_inplace(slice, [](const BufferEntry& a, const BufferEntry& b) { return a.key < b.key; });
   /* std::sort(all_entries.begin(), all_entries.end(), [](const BufferEntry& a, const BufferEntry& b)
			{
				return a.key < b.key;
			});
			*/

   return all_entries;
}
//---------------------------------------------------------------------------
void HashtableBuild::buildBloomFilter(const Vector<BufferEntry>& sorted_data, size_t unique_keys) {
   if (sorted_data.empty()) return;

   // 1. Allocate bloom filter
   size_t bits = bloom_filter_bits_;
   if (bits == 0) {
      // default 20 bits per unique key for ~0.036% false positive rate with 3 hashes
      bits = std::max(unique_keys * 20, size_t(2048));
   }
   ht.bloom_filter_.allocate(bits);

   size_t n = sorted_data.size();
   constexpr size_t NUM_HASHES = BloomFilter::NUM_HASHES;

   // 2. Compute all n × NUM_HASHES global bit positions in parallel
   Vector<uint64_t> bit_indices(n * NUM_HASHES);
   parlay::parallel_for(0, n, [&](size_t i) {
      for (size_t h = 0; h < NUM_HASHES; ++h) {
         bit_indices[i * NUM_HASHES + h] = ht.bloom_filter_.globalBitIndex(sorted_data[i].key, h);
      }
   });

   // 3. Semisort to group duplicate bit positions contiguously
   auto slice = parlay::make_slice(bit_indices.data(), bit_indices.data() + bit_indices.size());
   parlay::internal::semisort_equal_inplace(slice, [](uint64_t x) { return x; });

   // 4. Set each unique bit once (atomics kept for word-level race safety)
   parlay::parallel_for(0, bit_indices.size(), [&](size_t i) {
      if (i == 0 || bit_indices[i] != bit_indices[i - 1]) {
         ht.bloom_filter_.setBit(bit_indices[i]);
      }
   });
}
//---------------------------------------------------------------------------
void HashtableBuild::buildTreeBottomUp(Vector<BufferEntry>& sorted_data) {
   if (sorted_data.empty()) {
      ht.root_ = nullptr;
      ht.height_ = 0;
      return;
   }

   size_t n = sorted_data.size();
   size_t entries_per_page = Hashtable::LeafPage::MAX_ENTRIES;
   size_t num_leaf_pages = (n + entries_per_page - 1) / entries_per_page;

   // create leaf pages in parallel
   ht.leaf_pages_.resize(num_leaf_pages);

   if (num_leaf_pages > 16) {
      Scheduler::parallelFor(0, num_leaf_pages, [&](size_t, size_t page_idx) {
         auto* page = static_cast<Hashtable::LeafPage*>(
            querymemory::allocate(BTREE_PAGE_SIZE));

         page->header.is_leaf = 1;
         page->header.num_keys = 0;
         page->header.reserved = 0;
         page->header.next_page_idx = (page_idx + 1 < num_leaf_pages) ? static_cast<uint32_t>(page_idx + 1) : 0xFFFFFFFF;

         size_t start = page_idx * entries_per_page;
         size_t end = std::min(start + entries_per_page, n);

         for (size_t i = start; i < end; ++i) {
            size_t slot = i - start;
            page->keys[slot] = sorted_data[i].key;
            page->entries[slot] = sorted_data[i].entry;
            page->header.num_keys++;
         }

         ht.leaf_pages_[page_idx] = page;
      });
   } else {
      for (size_t page_idx = 0; page_idx < num_leaf_pages; ++page_idx) {
         auto* page = static_cast<Hashtable::LeafPage*>(
            querymemory::allocate(BTREE_PAGE_SIZE));

         page->header.is_leaf = 1;
         page->header.num_keys = 0;
         page->header.reserved = 0;
         page->header.next_page_idx = (page_idx + 1 < num_leaf_pages) ? static_cast<uint32_t>(page_idx + 1) : 0xFFFFFFFF;

         size_t start = page_idx * entries_per_page;
         size_t end = std::min(start + entries_per_page, n);

         for (size_t i = start; i < end; ++i) {
            size_t slot = i - start;
            page->keys[slot] = sorted_data[i].key;
            page->entries[slot] = sorted_data[i].entry;
            page->header.num_keys++;
         }

         ht.leaf_pages_[page_idx] = page;
      }
   }

   // singleton tree just has root
   if (num_leaf_pages == 1) {
      ht.root_ = ht.leaf_pages_[0];
      ht.height_ = 1;
      return;
   }

   // build internal levels bottom up
   Vector<void*> current_level;
   current_level.reserve(num_leaf_pages);
   for (auto* leaf : ht.leaf_pages_) {
      current_level.push_back(leaf);
   }

   bool children_are_leaves = true;
   size_t height = 1;

   while (current_level.size() > 1) {
      size_t num_children = current_level.size();
      size_t children_per_page = Hashtable::InternalPage::MAX_KEYS + 1;
      size_t num_parent_pages = (num_children + children_per_page - 1) / children_per_page;

      Vector<Hashtable::InternalPage*> parent_pages;
      parent_pages.resize(num_parent_pages);

      if (num_parent_pages > 16) {
         Scheduler::parallelFor(0, num_parent_pages, [&](size_t, size_t page_idx) {
            auto* page = static_cast<Hashtable::InternalPage*>(
               querymemory::allocate(BTREE_PAGE_SIZE));

            page->header.is_leaf = 0;
            page->header.num_keys = 0;
            page->header.reserved = 0;
            page->header.next_page_idx = 0xFFFFFFFF;

            size_t start = page_idx * children_per_page;
            size_t end = std::min(start + children_per_page, num_children);
            size_t num_children_in_page = end - start;

            // children
            // get high keys
            for (size_t i = 0; i < num_children_in_page; ++i) {
               page->children[i] = current_level[start + i];

               if (i < num_children_in_page - 1) {
                  uint64_t high_key;
                  if (children_are_leaves) {
                     auto* leaf = static_cast<Hashtable::LeafPage*>(current_level[start + i]);
                     high_key = leaf->keys[leaf->header.num_keys - 1];
                  } else {
                     // DEBUG 19d:
                     // For internal pages, we need the high key of the rightmost leaf
                     // in the subtree, not the last separator key
                     void* rightmost = current_level[start + i];
                     while (true) {
                        auto* internal_node = static_cast<Hashtable::InternalPage*>(rightmost);
                        rightmost = internal_node->children[internal_node->header.num_keys];
                        if (static_cast<Hashtable::PageHeader*>(rightmost)->is_leaf) break;
                     }
                     auto* leaf = static_cast<Hashtable::LeafPage*>(rightmost);
                     high_key = leaf->keys[leaf->header.num_keys - 1];
                  }
                  page->keys[page->header.num_keys++] = high_key;
               }
            }

            parent_pages[page_idx] = page;
         });
      } else {
         for (size_t page_idx = 0; page_idx < num_parent_pages; ++page_idx) {
            auto* page = static_cast<Hashtable::InternalPage*>(
               querymemory::allocate(BTREE_PAGE_SIZE));

            page->header.is_leaf = 0;
            page->header.num_keys = 0;
            page->header.reserved = 0;
            page->header.next_page_idx = 0xFFFFFFFF;

            size_t start = page_idx * children_per_page;
            size_t end = std::min(start + children_per_page, num_children);
            size_t num_children_in_page = end - start;

            for (size_t i = 0; i < num_children_in_page; ++i) {
               page->children[i] = current_level[start + i];

               if (i < num_children_in_page - 1) {
                  uint64_t high_key;
                  if (children_are_leaves) {
                     auto* leaf = static_cast<Hashtable::LeafPage*>(current_level[start + i]);
                     high_key = leaf->keys[leaf->header.num_keys - 1];
                  } else {
                     void* rightmost = current_level[start + i];
                     while (true) {
                        auto* internal_node = static_cast<Hashtable::InternalPage*>(rightmost);
                        rightmost = internal_node->children[internal_node->header.num_keys];
                        if (static_cast<Hashtable::PageHeader*>(rightmost)->is_leaf) break;
                     }
                     auto* leaf = static_cast<Hashtable::LeafPage*>(rightmost);
                     high_key = leaf->keys[leaf->header.num_keys - 1];
                  }
                  page->keys[page->header.num_keys++] = high_key;
               }
            }

            parent_pages[page_idx] = page;
         }
      }

      // next level
      current_level.clear();
      for (auto* p : parent_pages) {
         current_level.push_back(p);
      }
      children_are_leaves = false;
      height++;
   }

   ht.root_ = current_level[0];
   ht.height_ = height;
}
//---------------------------------------------------------------------------
void HashtableBuild::finishConsume() {
#ifdef ENABLE_LOG
   ManualTimer totalBuildTimer;
#endif

   // collect and sort
#ifdef ENABLE_LOG
   ManualTimer collectSortTimer;
#endif
   auto sorted_data = collectAndSort();
#ifdef ENABLE_LOG
   collectSortTimer.record("htCollectAndSort", "tuples=" + std::to_string(sorted_data.size()));
#endif

   ht.num_tuples = sorted_data.size();

   if (sorted_data.empty()) {
      ht.root_ = nullptr;
      ht.num_tuples = 0;
      ht.num_keys = 0;
      ht.height_ = 0;
#ifdef ENABLE_LOG
      totalBuildTimer.record("htBuildTotal", "empty");
#endif
      return;
   }

   // count unique keys
#ifdef ENABLE_LOG
   ManualTimer countKeysTimer;
#endif
   size_t unique_keys = 1;
   bool has_duplicates = false;
   for (size_t i = 1; i < sorted_data.size(); ++i) {
      if (sorted_data[i].key != sorted_data[i - 1].key) {
         unique_keys++;
      } else {
         has_duplicates = true;
      }
   }

   ht.num_keys = unique_keys;
   ht.is_certainly_duplicate_free = !has_duplicates;

   // duplicates
   if constexpr (config::handleMultiplicity) {
      for (const auto& entry : sorted_data) {
         if (entry.entry->tuple[0] > 1) {
            ht.is_certainly_duplicate_free = false;
            break;
         }
      }
   }
#ifdef ENABLE_LOG
   countKeysTimer.record("htCountUniqueKeys", "keys=" + std::to_string(unique_keys));
#endif

   // bloom filter
#ifdef ENABLE_LOG
   ManualTimer bloomTimer;
#endif
   buildBloomFilter(sorted_data, unique_keys);
#ifdef ENABLE_LOG
   bloomTimer.record("htBuildBloomFilter", "bits=" + std::to_string(ht.bloom_filter_.getTotalBits()));
#endif

   // build tree bottom up
#ifdef ENABLE_LOG
   ManualTimer treeTimer;
#endif
   buildTreeBottomUp(sorted_data);
#ifdef ENABLE_LOG
   treeTimer.record("htBuildTree", "height=" + std::to_string(ht.height_) + " leaves=" + std::to_string(ht.leaf_pages_.size()));
#endif

   // cleanup local states
   if constexpr (!std::is_trivially_destructible_v<LocalState>) {
      for (auto* current = local_state_refs.load(); current; current = current->next) {
         current->~LocalState();
      }
   }

#ifdef ENABLE_LOG
   totalBuildTimer.record("htBuildTotal", "tuples=" + std::to_string(ht.num_tuples));
#endif
}
//---------------------------------------------------------------------------
HashtableBuild::HashtableBuild(Hashtable& ht, size_t card_estimate, size_t bloom_filter_bits)
   : ht(ht), bloom_filter_bits_(bloom_filter_bits) {
}
//---------------------------------------------------------------------------
} // namespace engine
//---------------------------------------------------------------------------
