#include "size_class.h"

#include <gtest/gtest.h>

#include "page_heap.h"

using namespace memloc;

TEST(SizeClass, EverySizeMapsToTheSmallestFittingClass) {
  for (std::size_t n = 0; n <= kMaxSmall; ++n) {
    const unsigned c = size_to_class(n);
    ASSERT_LT(c, kNumClasses) << "n=" << n;
    ASSERT_GE(class_size(c), n) << "n=" << n;
    if (c > 0) {
      ASSERT_LT(class_size(c - 1), n) << "n=" << n;
    }
  }
}

TEST(SizeClass, WasteAbove128BytesIsAtMostOneFifth) {
  for (std::size_t n = 129; n <= kMaxSmall; ++n) {
    const std::size_t waste = class_size(size_to_class(n)) - n;
    ASSERT_LE(waste * 5, class_size(size_to_class(n))) << "n=" << n;
  }
}

TEST(SizeClass, KnownBoundaries) {
  EXPECT_EQ(class_size(size_to_class(1)), 16u);
  EXPECT_EQ(class_size(size_to_class(17)), 32u);
  EXPECT_EQ(class_size(size_to_class(129)), 160u);
  EXPECT_EQ(class_size(size_to_class(257)), 320u);
  EXPECT_EQ(class_size(size_to_class(20481)), 24576u);
  EXPECT_EQ(size_to_class(kMaxSmall), kNumClasses - 1);
}

TEST(SizeClass, BatchSizesAreBounded) {
  for (unsigned c = 0; c < kNumClasses; ++c) {
    EXPECT_GE(batch_size(c), 2u);
    EXPECT_LE(batch_size(c), 32u);
    EXPECT_LE(batch_size(c), objects_per_span(c) * 16);
  }
}

TEST(PageHeap, FindFreeRun) {
  EXPECT_EQ(find_free_run(0, 1), -1);
  EXPECT_EQ(find_free_run(0b1, 1), 0);
  EXPECT_EQ(find_free_run(0b1111'0011'1100'0000, 3), 6);
  EXPECT_EQ(find_free_run(0b1111'0011'1100'0000, 4), 6);
  EXPECT_EQ(find_free_run(0b1111'0011'1100'0000, 5), -1);
  EXPECT_EQ(find_free_run(~std::uint64_t{1}, 63), 1);
  EXPECT_EQ(find_free_run(~std::uint64_t{1}, 64), -1);
}
