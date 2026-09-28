// bench_random.cpp - random-size malloc/free throughput. (D-31)
//
//   usage: bench_random <threads>
//
// Each thread keeps a private window of 256 live blocks and replaces a
// random one per op. Sizes are log-uniform over 8 B - 32 KiB, so every
// power-of-two range (8-16, 16-32, ..., 16K-32K) is equally likely and all
// of MemLoc's small classes are exercised, not just the tiny ones:
//
//   window: [ p0 | p1 | ... | p255 ]    op: free(p[k]); p[k] = malloc(n)
//                  ^ k random            n = log-uniform(8, 32768)
//
// Metric: million ops per second across all threads (op = free + malloc).
#include <barrier>
#include <cstdlib>
#include <thread>
#include <vector>

#include "bench_common.h"

namespace {

constexpr std::size_t kWindow = 256;
constexpr unsigned kOps = 10'000'000;  // per thread

void* new_block(bench::Rng& rng) {
  auto* p = static_cast<unsigned char*>(std::malloc(rng.log_uniform(8, 32768)));
  p[0] = 1;
  return p;
}

}  // namespace

int main(int argc, char** argv) {
  const unsigned threads = bench::arg_threads(argc, argv);
  std::barrier sync(static_cast<std::ptrdiff_t>(threads) + 1);
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      bench::Rng rng(t + 1);
      void* window[kWindow];
      for (auto& p : window) p = new_block(rng);
      sync.arrive_and_wait();  // start line
      for (unsigned i = 0; i < kOps; ++i) {
        void*& slot = window[rng.below(kWindow)];
        std::free(slot);
        slot = new_block(rng);
      }
      sync.arrive_and_wait();  // finish line
      for (void* p : window) std::free(p);
    });
  }
  sync.arrive_and_wait();
  const double start = bench::now_seconds();
  sync.arrive_and_wait();
  const double elapsed = bench::now_seconds() - start;
  for (auto& th : pool) th.join();

  bench::row("random", threads, "mops", static_cast<double>(threads) * kOps / elapsed / 1e6);
  bench::row("random", threads, "max_rss_mib",
             static_cast<double>(bench::max_rss_bytes()) / (1 << 20));
  return 0;
}
