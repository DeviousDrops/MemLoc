// page_heap.h - hands out spans (runs of 64 KiB units) from 4 MiB chunks. (D-3)
//
// Each chunk's free units are the zero bits of chunk->used. To find k
// contiguous free units, AND the free mask with shifted copies of itself:
//
//   free          = 1111 0011 1100 0000   (bit j set = unit j free)
//   free>>1       = 0111 1001 1110 0000
//   free>>2       = 0011 1100 1111 0000
//   k=3: m = free & free>>1 & free>>2 = 0011 0000 1100 0000
//   bit j of m is set iff units j..j+2 are all free; first fit = ctz(m)
//
// Freeing a span clears its bits, so it merges with free neighbours for free
// (no coalescing code at all). Chunks are appended to a list and searched
// first-fit from the oldest, which packs the heap toward low addresses.
// Chunks are never unmapped: returning memory to the OS is a non-goal.
//
// One mutex guards the whole page heap. It is the innermost lock: nothing is
// called while holding it except mmap (D-13).
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <mutex>  // std::scoped_lock

#include "chunk.h"
#include "mutex.h"

namespace memloc {

// Index of the lowest run of k consecutive set bits in free_mask, or -1.
constexpr int find_free_run(std::uint64_t free_mask, unsigned k) {
  std::uint64_t m = free_mask;
  for (unsigned i = 1; i < k && m != 0; ++i) m &= free_mask >> i;
  return m == 0 ? -1 : std::countr_zero(m);
}

class PageHeap {
 public:
  constexpr PageHeap() = default;

  // Returns a span of `units` contiguous units of the given kind, with
  // size_class and all list fields zeroed, or nullptr if the OS refuses more
  // memory.
  Span* alloc_span(unsigned units, SpanKind kind);
  void free_span(Span* s);

  // Grows or shrinks a span in place to `units`. Growing needs the units just
  // after it in the same chunk to be free. Returns false (changing nothing)
  // when they are not. Used by realloc on medium blocks.
  bool resize_span(Span* s, unsigned units);

  // fork() support (D-20): hold the heap's lock across fork.
  void lock() { mu_.lock(); }
  void unlock() { mu_.unlock(); }

  // First chunk of the list of all chunks (linked through Chunk::next).
  // Requires mu_; memloc_stats() walks it (D-28).
  Chunk* first_chunk() const { return first_; }

  std::size_t mapped_bytes() const {
    return chunk_count_.load(std::memory_order_relaxed) * kChunkSize;
  }

 private:
  Chunk* grow();

  Mutex mu_;
  Chunk* first_ = nullptr;                   // guarded by mu_
  Chunk* last_ = nullptr;                    // guarded by mu_
  std::atomic<std::size_t> chunk_count_{0};  // written under mu_, read anywhere
};

extern PageHeap g_page_heap;

}  // namespace memloc
