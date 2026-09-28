// Multi-threaded behaviour: concurrent stress, producer/consumer cross-thread
// frees, thread-exit flushing and the post-exit (Dead) path. Run under
// ThreadSanitizer in CI (`make tsan`).
#include <gtest/gtest.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "central.h"
#include "chunk.h"
#include "memloc/memloc.h"
#include "size_class.h"
#include "thread_cache.h"

namespace {

// Stamps a block with its own address so that any overlap between two live
// blocks, or a block handed out twice, is detected on verification.
void stamp(void* p, std::size_t n) {
  const auto v = reinterpret_cast<std::uintptr_t>(p);
  for (std::size_t i = 0; i + sizeof(v) <= n; i += sizeof(v))
    std::memcpy(static_cast<char*>(p) + i, &v, sizeof(v));
}

bool verify(const void* p, std::size_t n) {
  const auto v = reinterpret_cast<std::uintptr_t>(p);
  for (std::size_t i = 0; i + sizeof(v) <= n; i += sizeof(v)) {
    std::uintptr_t got = 0;
    std::memcpy(&got, static_cast<const char*>(p) + i, sizeof(v));
    if (got != v) return false;
  }
  return true;
}

std::size_t random_size(std::mt19937& rng) {
  const unsigned r = rng() % 100;
  if (r < 90) return 8 + rng() % 512;
  if (r < 99) return rng() % (32 * 1024);
  return 32 * 1024 + rng() % (512 * 1024);
}

}  // namespace

TEST(Threads, ConcurrentRandomStress) {
  constexpr int kThreads = 8;
  constexpr int kOps = 100000;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t, &failures] {
      std::mt19937 rng(static_cast<unsigned>(t) * 7919u);
      struct Slot {
        void* p;
        std::size_t n;
      };
      std::vector<Slot> slots(512, Slot{nullptr, 0});
      for (int i = 0; i < kOps; ++i) {
        Slot& s = slots[rng() % slots.size()];
        if (s.p != nullptr) {
          if (!verify(s.p, s.n)) failures.fetch_add(1);
          memloc_free(s.p);
          s.p = nullptr;
        } else {
          s.n = random_size(rng);
          s.p = memloc_malloc(s.n);
          stamp(s.p, s.n);
        }
      }
      for (Slot& s : slots) {
        if (s.p != nullptr && !verify(s.p, s.n)) failures.fetch_add(1);
        memloc_free(s.p);
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST(Threads, ProducerConsumerCrossThreadFrees) {
  // Producers allocate, consumers verify and free: every small block is freed
  // by a thread other than the one that allocated it.
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kPerProducer = 50000;
  struct Item {
    void* p;
    std::size_t n;
  };
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Item> queue;
  int producers_left = kProducers;
  std::atomic<int> failures{0};
  std::atomic<long> consumed{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kProducers; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<unsigned>(t) + 1);
      for (int i = 0; i < kPerProducer; ++i) {
        const std::size_t n = 16 + rng() % 2048;
        void* p = memloc_malloc(n);
        stamp(p, n);
        {
          const std::lock_guard lock(mu);
          queue.push_back({p, n});
        }
        cv.notify_one();
      }
      const std::lock_guard lock(mu);
      if (--producers_left == 0) cv.notify_all();
    });
  }
  for (int t = 0; t < kConsumers; ++t) {
    threads.emplace_back([&] {
      for (;;) {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return !queue.empty() || producers_left == 0; });
        if (queue.empty()) return;
        Item item = queue.front();
        queue.pop_front();
        lock.unlock();
        if (!verify(item.p, item.n)) failures.fetch_add(1);
        memloc_free(item.p);
        consumed.fetch_add(1);
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(consumed.load(), static_cast<long>(kProducers) * kPerProducer);
}

TEST(Threads, CacheRespectsHighWaterMark) {
  // Free many objects on one thread: its cache must stay within 2 * batch.
  constexpr std::size_t kSize = 96;
  const unsigned cls = memloc::size_to_class(kSize);
  std::vector<void*> blocks;
  for (int i = 0; i < 5000; ++i) blocks.push_back(memloc_malloc(kSize));
  std::thread([&] {
    for (void* p : blocks) {
      memloc_free(p);
      ASSERT_LE(memloc::tc_cached_count(cls), 2u * memloc::batch_size(cls));
    }
    EXPECT_GE(memloc::tc_cached_count(cls), memloc::batch_size(cls));
  }).join();
}

TEST(Threads, TransferCacheHandsBatchesBetweenThreads) {
  constexpr std::size_t kSize = 256;
  const unsigned cls = memloc::size_to_class(kSize);
  const unsigned batch = memloc::batch_size(cls);
  const unsigned before = memloc::g_central[cls].cached_batches();
  std::vector<void*> blocks(2 * batch + 1);
  for (auto& p : blocks) p = memloc_malloc(kSize);
  // One thread overflows its cache and releases one full batch...
  std::thread([&] {
    for (void* p : blocks) memloc_free(p);
  }).join();
  // ...(its exit flush is a partial list, which goes to the spans instead).
  EXPECT_EQ(memloc::g_central[cls].cached_batches(), before + 1);
  // A fresh thread's first refill takes that batch whole.
  void* got = nullptr;
  std::thread([&] { got = memloc_malloc(kSize); }).join();
  EXPECT_EQ(memloc::g_central[cls].cached_batches(), before);
  EXPECT_NE(std::find(blocks.begin(), blocks.end(), got), blocks.end());
}

TEST(Threads, ExitingThreadFlushesItsCache) {
  // Class 20 KiB: nothing else in this test uses it, and one span holds
  // exactly 12 objects.
  constexpr std::size_t kSize = 20 * 1024;
  const unsigned cls = memloc::size_to_class(kSize);
  ASSERT_EQ(memloc::objects_per_span(cls), 12u);
  std::vector<void*> blocks(12);
  // Allocate on one thread and free on another; both exit, so both caches
  // (the allocator's leftover batch and the freer's list) must be flushed.
  std::thread([&] {
    for (auto& p : blocks) p = memloc_malloc(kSize);
  }).join();
  std::thread([&] {
    for (void* p : blocks) memloc_free(p);
    EXPECT_GT(memloc::tc_cached_count(cls), 0u);
  }).join();
  // Nothing is stranded in a dead cache: a new thread gets all 12 back
  // without a second span being carved.
  std::vector<void*> again(12);
  std::thread([&] {
    for (auto& p : again) p = memloc_malloc(kSize);
  }).join();
  std::sort(blocks.begin(), blocks.end());
  std::sort(again.begin(), again.end());
  EXPECT_EQ(blocks, again);
  for (void* p : again) memloc_free(p);
}

namespace {
pthread_key_t g_late_key;
std::atomic<int> g_late_ok{0};

// Runs after MemLoc's own key destructor (keys run in creation order), i.e.
// after this thread's cache was flushed and marked Dead.
void late_destructor(void*) {
  std::vector<void*> v;
  for (int i = 0; i < 1000; ++i) {
    void* p = memloc_malloc(64);
    stamp(p, 64);
    v.push_back(p);
  }
  bool ok = true;
  for (void* p : v) {
    ok = ok && verify(p, 64);
    memloc_free(p);
  }
  if (ok) g_late_ok.fetch_add(1);
}
}  // namespace

TEST(Threads, AllocationAfterCacheTeardownStillWorks) {
  memloc_free(memloc_malloc(64));  // make sure MemLoc's key exists first
  ASSERT_EQ(pthread_key_create(&g_late_key, late_destructor), 0);
  std::thread([] {
    memloc_free(memloc_malloc(64));  // activate this thread's cache
    pthread_setspecific(g_late_key, reinterpret_cast<void*>(1));
  }).join();
  EXPECT_EQ(g_late_ok.load(), 1);
  pthread_key_delete(g_late_key);
}

TEST(Threads, ManyShortLivedThreads) {
  // Thread churn: each thread's cache is created, used and flushed.
  for (int round = 0; round < 50; ++round) {
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([] {
        void* v[64];
        for (auto& p : v) p = memloc_malloc(48);
        for (auto* p : v) memloc_free(p);
      });
    }
    for (auto& th : threads) th.join();
  }
  SUCCEED();
}
