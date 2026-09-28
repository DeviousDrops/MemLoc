// memloc_stats() and memloc_stats_print() (D-28, D-29). Other tests in the
// same process allocate too, so the checks compare before/after deltas.
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <thread>

#include "memloc/memloc.h"

namespace {

struct memloc_stats snapshot() {
  struct memloc_stats s{};
  memloc_stats(&s);
  return s;
}

TEST(Stats, TotalsAreConsistent) {
  void* keep = memloc_malloc(100);
  const struct memloc_stats s = snapshot();
  EXPECT_EQ(s.allocated, s.small_allocated + s.medium_allocated + s.huge_allocated);
  EXPECT_GE(s.peak_mapped, s.mapped);
  EXPECT_GE(s.mapped, s.chunks * (std::size_t{4} << 20) + s.huge_allocated);
  EXPECT_LE(s.small_allocated, s.small_span_bytes);
  EXPECT_GT(s.small_allocated, 0U);
  memloc_free(keep);
}

TEST(Stats, SmallBlocksAreCounted) {
  constexpr int kN = 5000;
  static void* blocks[kN];
  const struct memloc_stats before = snapshot();
  for (auto& p : blocks) p = memloc_malloc(48);
  const struct memloc_stats during = snapshot();
  // At least the blocks themselves; more if the thread cache refilled ahead.
  EXPECT_GE(during.small_allocated, before.small_allocated + kN * std::size_t{48});
  for (auto* p : blocks) memloc_free(p);
}

TEST(Stats, MediumBlocksAreCountedExactly) {
  const struct memloc_stats before = snapshot();
  void* p = memloc_malloc(200 * 1024);
  const struct memloc_stats during = snapshot();
  EXPECT_GE(during.medium_allocated - before.medium_allocated, std::size_t{200} * 1024);
  memloc_free(p);
  EXPECT_EQ(snapshot().medium_allocated, before.medium_allocated);
}

TEST(Stats, HugeBlocksAreCountedExactly) {
  const struct memloc_stats before = snapshot();
  void* p = memloc_malloc(std::size_t{8} << 20);
  struct memloc_stats during = snapshot();
  EXPECT_EQ(during.huge_count, before.huge_count + 1);
  EXPECT_GE(during.huge_allocated - before.huge_allocated, std::size_t{8} << 20);
  EXPECT_GE(during.mapped - before.mapped, std::size_t{8} << 20);

  p = memloc_realloc(p, std::size_t{16} << 20);  // mremap path
  during = snapshot();
  EXPECT_GE(during.huge_allocated - before.huge_allocated, std::size_t{16} << 20);

  memloc_free(p);
  const struct memloc_stats after = snapshot();
  EXPECT_EQ(after.huge_count, before.huge_count);
  EXPECT_EQ(after.huge_allocated, before.huge_allocated);
  EXPECT_EQ(after.mapped, before.mapped);
  EXPECT_GE(after.peak_mapped, before.mapped + (std::size_t{16} << 20));
}

TEST(Stats, SafeWhileOtherThreadsAllocate) {
  std::thread worker([] {
    for (int i = 0; i < 200000; ++i) memloc_free(memloc_malloc(static_cast<std::size_t>(i % 5000)));
  });
  for (int i = 0; i < 200; ++i) {
    const struct memloc_stats s = snapshot();
    ASSERT_LE(s.small_allocated, s.small_span_bytes);
  }
  worker.join();
}

TEST(Stats, PrintWritesATable) {
  void* keep = memloc_malloc(64);
  std::FILE* f = std::tmpfile();
  ASSERT_NE(f, nullptr);
  memloc_stats_print(fileno(f));
  std::rewind(f);
  std::string text;
  char buf[4096];
  while (std::size_t n = std::fread(buf, 1, sizeof(buf), f)) text.append(buf, n);
  std::fclose(f);
  EXPECT_NE(text.find("mapped from OS"), std::string::npos) << text;
  EXPECT_NE(text.find("block B"), std::string::npos) << text;
  EXPECT_NE(text.find("     64 "), std::string::npos) << text;  // the 64-byte class row
  memloc_free(keep);
}

}  // namespace
