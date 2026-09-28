// Randomised single-threaded stress: many live blocks of random sizes across
// all tiers, each filled with a pattern derived from its slot, verified before
// every free and realloc. Catches overlapping blocks, lost bytes on realloc and
// free-list corruption.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "memloc/memloc.h"

namespace {

struct Block {
  unsigned char* p = nullptr;
  std::size_t n = 0;
  unsigned char tag = 0;
};

std::size_t random_size(std::mt19937_64& rng) {
  // Mostly small, sometimes medium, rarely huge: roughly what real programs do.
  const unsigned r = rng() % 1000;
  if (r < 900) return rng() % 512;
  if (r < 990) return rng() % (32 * 1024);
  if (r < 999) return 32 * 1024 + rng() % (1024 * 1024);
  return (1u << 20) + rng() % (4u << 20);
}

bool intact(const Block& b) {
  for (std::size_t i = 0; i < b.n; ++i) {
    if (b.p[i] != static_cast<unsigned char>(b.tag + i)) return false;
  }
  return true;
}

void stamp(Block& b, unsigned char tag) {
  b.tag = tag;
  for (std::size_t i = 0; i < b.n; ++i) b.p[i] = static_cast<unsigned char>(tag + i);
}

}  // namespace

TEST(Stress, RandomAllocFreeReallocKeepsContents) {
  std::mt19937_64 rng(42);
  std::vector<Block> slots(4096);
  for (int step = 0; step < 300000; ++step) {
    Block& b = slots[rng() % slots.size()];
    const auto tag = static_cast<unsigned char>(step);
    if (b.p == nullptr) {
      b.n = random_size(rng);
      b.p = static_cast<unsigned char*>(memloc_malloc(b.n));
      ASSERT_NE(b.p, nullptr);
      stamp(b, tag);
    } else if (rng() % 4 == 0) {
      ASSERT_TRUE(intact(b)) << "corrupted before realloc at step " << step;
      const std::size_t n = random_size(rng);
      auto* q = static_cast<unsigned char*>(memloc_realloc(b.p, n == 0 ? 1 : n));
      ASSERT_NE(q, nullptr);
      b.p = q;
      b.n = std::min(b.n, n == 0 ? std::size_t{1} : n);
      ASSERT_TRUE(intact(b)) << "realloc lost bytes at step " << step;
      b.n = n == 0 ? 1 : n;
      stamp(b, tag);
    } else {
      ASSERT_TRUE(intact(b)) << "corrupted before free at step " << step;
      memloc_free(b.p);
      b = Block{};
    }
  }
  for (Block& b : slots) {
    if (b.p == nullptr) continue;
    ASSERT_TRUE(intact(b));
    memloc_free(b.p);
  }
}

TEST(Stress, ManySmallObjectsAreDistinct) {
  // 200k live 16-byte objects span many chunks; none may overlap.
  std::vector<std::uint64_t*> v;
  v.reserve(200000);
  for (std::uint64_t i = 0; i < 200000; ++i) {
    auto* p = static_cast<std::uint64_t*>(memloc_malloc(16));
    ASSERT_NE(p, nullptr);
    p[0] = i;
    p[1] = ~i;
    v.push_back(p);
  }
  for (std::uint64_t i = 0; i < v.size(); ++i) {
    ASSERT_EQ(v[i][0], i);
    ASSERT_EQ(v[i][1], ~i);
    memloc_free(v[i]);
  }
}
