// Semantics of the public API: alignment, boundary sizes, realloc, calloc and
// the aligned-allocation family. Behaviour follows glibc (D-9).
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#include "config.h"
#include "memloc/memloc.h"

namespace {

bool aligned_to(const void* p, std::size_t a) {
  return reinterpret_cast<std::uintptr_t>(p) % a == 0;
}

void fill(void* p, std::size_t n, unsigned seed) {
  auto* b = static_cast<unsigned char*>(p);
  for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<unsigned char>(seed + i * 31);
}

bool check(const void* p, std::size_t n, unsigned seed) {
  const auto* b = static_cast<const unsigned char*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    if (b[i] != static_cast<unsigned char>(seed + i * 31)) return false;
  }
  return true;
}

// Sizes around every tier boundary.
const std::vector<std::size_t> kBoundarySizes = {
    0,     1,     15,    16,    17,    127,   128,     129,     4095,    4096,     4097,
    32767, 32768, 32769, 65535, 65536, 65537, 1048575, 1048576, 1048577, 4u << 20,
};

}  // namespace

TEST(Api, EveryBoundarySizeIsUsableAnd16ByteAligned) {
  for (std::size_t n : kBoundarySizes) {
    void* p = memloc_malloc(n);
    ASSERT_NE(p, nullptr) << "n=" << n;
    EXPECT_TRUE(aligned_to(p, memloc::kMinAlign)) << "n=" << n;
    EXPECT_GE(memloc_malloc_usable_size(p), n) << "n=" << n;
    fill(p, memloc_malloc_usable_size(p), 7);  // the whole usable size is writable
    EXPECT_TRUE(check(p, memloc_malloc_usable_size(p), 7)) << "n=" << n;
    memloc_free(p);
  }
}

TEST(Api, MallocZeroReturnsUniquePointers) {
  void* a = memloc_malloc(0);
  void* b = memloc_malloc(0);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a, b);
  memloc_free(a);
  memloc_free(b);
}

TEST(Api, FreeNullIsANoOp) { memloc_free(nullptr); }

TEST(Api, UsableSizeOfNullIsZero) { EXPECT_EQ(memloc_malloc_usable_size(nullptr), 0u); }

TEST(Api, FreedSmallBlockIsReused) {
  void* a = memloc_malloc(40);
  memloc_free(a);
  void* b = memloc_malloc(48);  // same class (48)
  EXPECT_EQ(a, b);
  memloc_free(b);
}

TEST(Api, MediumBlocksAre64KiBAlignedAndReused) {
  void* a = memloc_malloc(100 * 1024);
  EXPECT_TRUE(aligned_to(a, memloc::kUnitSize));
  EXPECT_EQ(memloc_malloc_usable_size(a), 128u * 1024);
  memloc_free(a);
  void* b = memloc_malloc(128 * 1024);
  EXPECT_EQ(a, b);
  memloc_free(b);
}

TEST(Api, HugeAllocationFailsCleanly) {
  errno = 0;
  EXPECT_EQ(memloc_malloc(SIZE_MAX - 4096), nullptr);
  EXPECT_EQ(errno, ENOMEM);
  errno = 0;
  EXPECT_EQ(memloc_malloc(std::size_t{1} << 62), nullptr);
  EXPECT_EQ(errno, ENOMEM);
}

TEST(Calloc, ZeroesRecycledMemory) {
  for (std::size_t n : kBoundarySizes) {
    void* dirty = memloc_malloc(n);
    fill(dirty, n, 3);
    memloc_free(dirty);
    auto* p = static_cast<unsigned char*>(memloc_calloc(1, n));
    ASSERT_NE(p, nullptr);
    for (std::size_t i = 0; i < n; ++i) ASSERT_EQ(p[i], 0) << "n=" << n << " i=" << i;
    memloc_free(p);
  }
}

TEST(Calloc, OverflowIsDetected) {
  errno = 0;
  EXPECT_EQ(memloc_calloc(SIZE_MAX / 2 + 1, 2), nullptr);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(memloc_calloc(std::size_t{1} << 33, std::size_t{1} << 33), nullptr);
}

TEST(Realloc, NullBehavesLikeMalloc) {
  void* p = memloc_realloc(nullptr, 100);
  ASSERT_NE(p, nullptr);
  memloc_free(p);
}

TEST(Realloc, ZeroSizeFreesAndReturnsNull) {
  void* p = memloc_malloc(100);
  EXPECT_EQ(memloc_realloc(p, 0), nullptr);
}

TEST(Realloc, StaysInPlaceWithinTheSameClass) {
  void* p = memloc_malloc(130);  // class 160
  EXPECT_EQ(memloc_realloc(p, 160), p);
  EXPECT_EQ(memloc_realloc(p, 129), p);
  memloc_free(p);
}

TEST(Realloc, StaysInPlaceWithinTheSameMediumUnits) {
  void* p = memloc_malloc(70 * 1024);  // 2 units
  EXPECT_EQ(memloc_realloc(p, 120 * 1024), p);
  memloc_free(p);
}

TEST(Realloc, PreservesContentsAcrossEveryTier) {
  // Grow through small -> medium -> huge, then shrink back down.
  std::vector<std::size_t> steps = {1,        24,       200,    5000,  40000, 300000, 2u << 20,
                                    9u << 20, 3u << 20, 500000, 20000, 100,   8};
  std::size_t prev = steps[0];
  void* p = memloc_malloc(prev);
  fill(p, prev, 11);
  for (std::size_t n : steps) {
    void* q = memloc_realloc(p, n);
    ASSERT_NE(q, nullptr);
    const std::size_t kept = std::min(prev, n);
    ASSERT_TRUE(check(q, kept, 11)) << prev << " -> " << n;
    fill(q, n, 11);
    p = q;
    prev = n;
  }
  memloc_free(p);
}

TEST(Realloc, ShrinkingLargeBlockMovesItToASmallerTier) {
  void* p = memloc_malloc(200 * 1024);
  void* q = memloc_realloc(p, 64);
  EXPECT_NE(p, q);
  EXPECT_EQ(memloc_malloc_usable_size(q), 64u);
  memloc_free(q);
}

TEST(Aligned, PosixMemalignHonoursEveryPowerOfTwo) {
  for (std::size_t align = sizeof(void*); align <= (std::size_t{4} << 20); align *= 2) {
    for (std::size_t n : {std::size_t{1}, std::size_t{100}, align, 3 * align + 5,
                          std::size_t{200000}, std::size_t{3} << 20}) {
      void* p = nullptr;
      ASSERT_EQ(memloc_posix_memalign(&p, align, n), 0) << align << "/" << n;
      EXPECT_TRUE(aligned_to(p, align)) << align << "/" << n;
      EXPECT_GE(memloc_malloc_usable_size(p), n);
      fill(p, n, 5);
      EXPECT_TRUE(check(p, n, 5));
      memloc_free(p);
    }
  }
}

TEST(Aligned, PosixMemalignRejectsBadAlignment) {
  void* p = reinterpret_cast<void*>(0x1234);
  errno = 0;
  EXPECT_EQ(memloc_posix_memalign(&p, 24, 10), EINVAL);
  EXPECT_EQ(memloc_posix_memalign(&p, 4, 10), EINVAL);  // not a multiple of sizeof(void*)
  EXPECT_EQ(memloc_posix_memalign(&p, 0, 10), EINVAL);
  EXPECT_EQ(p, reinterpret_cast<void*>(0x1234));  // untouched on failure
  EXPECT_EQ(errno, 0);                            // errno untouched too
}

TEST(Aligned, AlignedAllocRejectsNonPowerOfTwo) {
  errno = 0;
  EXPECT_EQ(memloc_aligned_alloc(48, 96), nullptr);
  EXPECT_EQ(errno, EINVAL);
  void* p = memloc_aligned_alloc(4096, 4096);
  EXPECT_TRUE(aligned_to(p, 4096));
  memloc_free(p);
}

TEST(Aligned, SmallAlignedBlocksComeFromAMatchingClass) {
  void* p = memloc_aligned_alloc(64, 10);
  EXPECT_TRUE(aligned_to(p, 64));
  EXPECT_EQ(memloc_malloc_usable_size(p), 64u);
  memloc_free(p);
}
