// chunk.h - memory layout of a chunk, and O(1) pointer -> span lookup. (D-2, D-3)
//
// A chunk is a 4 MiB mmap region aligned to 4 MiB, split into 64 units of
// 64 KiB. Unit 0 holds the chunk header; units 1..63 are handed out as spans
// (runs of contiguous units) by the page heap.
//
//   chunk base (4 MiB aligned)
//   v
//   +-----------------+---------+---------+---------+---------+-----+---------+
//   | unit 0: header  | unit 1  | unit 2  | unit 3  | unit 4  | ... | unit 63 |
//   |  magic, used,   | span A  |<---- span B: 4 units, class 24 KiB ---->   |
//   |  owner[64],     | 16 B    |                                     |  free   |
//   |  spans[64]      | objects |                                     |         |
//   +-----------------+---------+---------+---------+---------+-----+---------+
//   used  = ...0011111   bit u set when unit u belongs to a span (bit 0 = header)
//   owner = [0,1,2,2,2,2,..]  first unit of the span covering unit u
//   spans[u]           descriptor of the span that *starts* at unit u
//
// Span descriptors live in the header, not in the span, so object memory is
// 100% payload and every span base is 64 KiB-aligned.
//
// Lookup for a pointer p inside a span (3 dependent loads, no search):
//   chunk = p & ~(kChunkSize - 1)
//   span  = &chunk->spans[chunk->owner[(p - chunk) >> kUnitShift]]
//
// The chunk registry holds one bit per 4 MiB of address space: it is set iff
// that region is one of our chunks. free() checks it first; a pointer outside
// any chunk is a huge block (huge.h). Bits are only ever set, never cleared,
// because chunks are never unmapped.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "config.h"

namespace memloc {

enum class SpanKind : std::uint8_t { kFree, kSmall, kMedium };

struct Span {
  void* free_list;  // recycled objects, linked through their first word (D-8)
  char* carve;      // next never-used object
  char* limit;      // end of the last whole object
  Span* prev;       // links in the owning central list (small spans only)
  Span* next;
  std::uint32_t live;  // objects currently handed out of this span
  std::uint8_t units;  // span length in units
  std::uint8_t size_class;
  SpanKind kind;
  bool in_list;  // linked into its class's central list
};

struct Chunk {
  std::uint64_t magic;
  std::uint64_t used;  // occupancy bitmap, bit u = unit u
  Chunk* next;         // page heap's list of all chunks
  std::uint8_t owner[kUnitsPerChunk];
  Span spans[kUnitsPerChunk];
};

inline constexpr std::uint64_t kChunkMagic = 0x4d454d4c4f43434bULL;  // "MEMLOCCK"

static_assert(sizeof(Chunk) <= kUnitSize, "chunk header must fit in unit 0");
static_assert(sizeof(Span) == 48, "span descriptor layout changed; re-check header size");

inline Chunk* chunk_of(const void* p) {
  return reinterpret_cast<Chunk*>(reinterpret_cast<std::uintptr_t>(p) & ~(kChunkSize - 1));
}

inline unsigned unit_of(const void* p) {
  return static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(p) & (kChunkSize - 1)) >>
                               kUnitShift);
}

inline Span* span_of(const void* p) {
  Chunk* c = chunk_of(p);
  return &c->spans[c->owner[unit_of(p)]];
}

// First byte of the memory a span describes. The descriptor lives inside the
// chunk header, so chunk_of() works on the descriptor's own address too.
inline char* span_base(const Span* s) {
  Chunk* c = chunk_of(s);
  return reinterpret_cast<char*>(c) + static_cast<std::size_t>(s - c->spans) * kUnitSize;
}

inline std::size_t span_bytes(const Span* s) { return std::size_t{s->units} * kUnitSize; }

// --- chunk registry -------------------------------------------------------
inline constexpr std::size_t kRegistryWords = std::size_t{1} << (kAddressBits - kChunkShift - 6);
// A plain array accessed through std::atomic_ref: it is zero-initialised BSS
// with no constructors to run, which is what makes it usable before any C++
// static initialiser (and keeps clang's constexpr evaluator out of it).
extern std::uint64_t g_chunk_registry[kRegistryWords];

inline std::atomic_ref<std::uint64_t> registry_word(std::size_t i) {
  return std::atomic_ref<std::uint64_t>(g_chunk_registry[i]);
}

inline bool in_chunk(const void* p) {
  const std::uintptr_t idx = reinterpret_cast<std::uintptr_t>(p) >> kChunkShift;
  if (idx >= kRegistryWords * 64) return false;
  // Relaxed is enough: the bit was set before the chunk handed out any memory,
  // and whoever gave us p synchronised with that hand-out. See D-15.
  return (registry_word(idx >> 6).load(std::memory_order_relaxed) >> (idx & 63)) & 1;
}

}  // namespace memloc
