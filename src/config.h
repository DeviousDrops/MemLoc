// config.h - compile-time constants shared by every part of MemLoc.
//
// Each value here is a design decision; the comment names the DECISIONS.md
// entry that justifies it. The static_asserts pin down the layout assumptions
// the rest of the code relies on.
#pragma once

#include <cstddef>
#include <cstdint>

namespace memloc {

// Every returned pointer is aligned to at least this (alignof(max_align_t)). D-1
inline constexpr std::size_t kMinAlign = 16;

// Largest request served by a size class. D-1, D-4
inline constexpr std::size_t kMaxSmall = std::size_t{32} * 1024;

// Largest request served as a page-heap span; anything bigger is mmap'd. D-4
inline constexpr std::size_t kMaxMedium = std::size_t{1024} * 1024;

// Page-heap granule. D-3
inline constexpr unsigned kUnitShift = 16;
inline constexpr std::size_t kUnitSize = std::size_t{1} << kUnitShift;

// Chunks are kChunkSize-aligned mmap regions carved into units. D-2, D-3
inline constexpr unsigned kChunkShift = 22;
inline constexpr std::size_t kChunkSize = std::size_t{1} << kChunkShift;
inline constexpr unsigned kUnitsPerChunk = kChunkSize / kUnitSize;

// User-space virtual address bits covered by the chunk registry. D-2
inline constexpr unsigned kAddressBits = 48;

constexpr bool is_pow2(std::size_t x) { return x != 0 && (x & (x - 1)) == 0; }
constexpr std::size_t align_up(std::size_t n, std::size_t a) { return (n + a - 1) & ~(a - 1); }

static_assert(kUnitsPerChunk == 64, "chunk occupancy is tracked in a single uint64_t");
static_assert(kMaxSmall < kMaxMedium);
static_assert(kMaxMedium <= (kUnitsPerChunk - 1) * kUnitSize, "a medium span must fit in a chunk");
static_assert(is_pow2(kMinAlign) && kMinAlign >= sizeof(void*));

}  // namespace memloc
