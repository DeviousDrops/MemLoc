// size_class.h - maps request sizes onto the 40 small size classes. (D-1)
//
//   class:  0   1   2  ...   7    8    9   10   11   12  ...    37     38     39
//   size:  16  32  48  ...  128  160  192  224  256  320  ... 24576  28672  32768
//          '-- 16-byte steps --' '---- 4 steps per power of two: <= 20% waste ----'
//
// Besides its size, each class fixes:
//   span_units(c)  - how many 64 KiB units one span of this class occupies
//   batch_size(c)  - how many objects move between a thread cache and the
//                    central list in one locked operation (Phase 2)
//
// Everything is constexpr, and classes_are_valid() proves the table's
// properties at compile time.
#pragma once

#include <algorithm>
#include <array>
#include <bit>

#include "config.h"

namespace memloc {

inline constexpr unsigned kNumClasses = 40;

constexpr std::size_t compute_class_size(unsigned c) {
  if (c < 8) return std::size_t{16} * (c + 1);
  const unsigned group = (c - 8) / 4;  // which power of two: 128 << group
  const unsigned step = (c - 8) % 4;   // which quarter-step within it
  const std::size_t base = std::size_t{128} << group;
  return base + (step + 1) * (base / 4);
}

inline constexpr auto kClassSize = [] {
  std::array<std::uint32_t, kNumClasses> table{};
  for (unsigned c = 0; c < kNumClasses; ++c) {
    table[c] = static_cast<std::uint32_t>(compute_class_size(c));
  }
  return table;
}();

constexpr std::size_t class_size(unsigned c) { return kClassSize[c]; }

// Smallest class whose size is >= n. Requires n <= kMaxSmall; 0 maps to class 0.
constexpr unsigned size_to_class(std::size_t n) {
  if (n <= 128) return n == 0 ? 0 : static_cast<unsigned>((n - 1) >> 4);
  // 2^lg < n <= 2^(lg+1); the quarter-step width in that range is 2^(lg-2).
  const auto lg = static_cast<unsigned>(std::bit_width(n - 1)) - 1;
  const auto step = static_cast<unsigned>((n - 1 - (std::size_t{1} << lg)) >> (lg - 2));
  return 8 + (lg - 7) * 4 + step;
}

// Classes up to 8 KiB use one 64 KiB unit; larger ones use four, so that the
// unusable tail of a span stays <= 1/16 (e.g. 24 KiB: 2 per 64 KiB wastes
// 25%, 10 per 256 KiB wastes 6.25%). D-3
constexpr unsigned span_units(unsigned c) { return class_size(c) <= 8192 ? 1 : 4; }

constexpr unsigned objects_per_span(unsigned c) {
  return static_cast<unsigned>(span_units(c) * kUnitSize / class_size(c));
}

// Objects moved per central-list operation: ~64 KiB worth, between 2 and 128. D-5, D-17
constexpr unsigned batch_size(unsigned c) {
  return static_cast<unsigned>(std::clamp<std::size_t>(kUnitSize / class_size(c), 2, 128));
}

constexpr bool classes_are_valid() {
  for (unsigned c = 0; c < kNumClasses; ++c) {
    const std::size_t size = class_size(c);
    const std::size_t span_bytes = span_units(c) * kUnitSize;
    if (size % kMinAlign != 0) return false;     // alignment guarantee
    if (size_to_class(size) != c) return false;  // exact fit maps to itself
    if (c > 0 && size_to_class(class_size(c - 1) + 1) != c) return false;  // no gaps
    if ((span_bytes % size) * 16 > span_bytes) return false;               // tail waste <= 1/16
    if (objects_per_span(c) < 2) return false;
  }
  return class_size(kNumClasses - 1) == kMaxSmall;
}
static_assert(classes_are_valid(), "size-class table violates its invariants");

}  // namespace memloc
