// bench_rss.cpp - memory overhead: resident set size / live bytes. (D-31)
//
//   usage: bench_rss <threads>
//
// Three phases, each thread doing its 1/T share with its own blocks:
//
//   fill     allocate log-uniform 16 B - 64 KiB blocks until 256 MiB is live
//   free     free a random half of them            (fragmentation)
//   refill   allocate 128 MiB of 1-4 KiB blocks     (different sizes: can the
//                                                    holes be reused?)
//
// Every block is fully written so its pages are resident. After each phase
// the program reports (RSS - RSS at start) / live bytes: 1.00 means no
// overhead at all, 1.25 means 25 % extra.
#include <barrier>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "bench_common.h"

namespace {

constexpr std::size_t kFillBytes = std::size_t{256} << 20;
constexpr std::size_t kRefillBytes = std::size_t{128} << 20;

struct Block {
  void* p;
  std::size_t n;
};

}  // namespace

int main(int argc, char** argv) {
  const unsigned threads = bench::arg_threads(argc, argv);
  std::vector<std::vector<Block>> blocks(threads);
  for (auto& v : blocks) v.reserve(kFillBytes / threads / 512);
  std::vector<std::size_t> live(threads, 0);
  const std::size_t base_rss = bench::current_rss_bytes();

  std::barrier sync(static_cast<std::ptrdiff_t>(threads) + 1);
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      bench::Rng rng(t + 1);
      std::vector<Block>& mine = blocks[t];
      auto add = [&](std::size_t n) {
        void* p = std::malloc(n);
        std::memset(p, 0x5A, n);
        mine.push_back({p, n});
        live[t] += n;
      };
      while (live[t] < kFillBytes / threads) add(rng.log_uniform(16, 65536));
      sync.arrive_and_wait();  // fill done
      sync.arrive_and_wait();  // measured

      std::size_t kept = 0;
      for (Block& b : mine) {
        if (rng.below(2) == 0) {
          std::free(b.p);
          live[t] -= b.n;
        } else {
          mine[kept++] = b;
        }
      }
      mine.resize(kept);
      sync.arrive_and_wait();  // free done
      sync.arrive_and_wait();

      const std::size_t target = live[t] + kRefillBytes / threads;
      while (live[t] < target) add(rng.between(1024, 4096));
      sync.arrive_and_wait();  // refill done
      sync.arrive_and_wait();
    });
  }

  const char* phases[] = {"fill", "free", "refill"};
  for (const char* phase : phases) {
    sync.arrive_and_wait();
    std::size_t total = 0;
    for (std::size_t n : live) total += n;
    const std::size_t rss = bench::current_rss_bytes() - base_rss;
    std::string metric = std::string("ratio_") + phase;
    bench::row("rss", threads, metric.c_str(),
               static_cast<double>(rss) / static_cast<double>(total));
    metric = std::string("live_mib_") + phase;
    bench::row("rss", threads, metric.c_str(), static_cast<double>(total) / (1 << 20));
    sync.arrive_and_wait();
  }
  for (auto& th : pool) th.join();
  for (auto& v : blocks) {
    for (Block& b : v) std::free(b.p);
  }
  return 0;
}
