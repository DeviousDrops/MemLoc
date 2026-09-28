// bench_larson.cpp - larson-style server workload with remote frees. (D-31)
//
//   usage: bench_larson <threads>
//
// Each of T threads works on a slot array of 1000 blocks (16-256 B). One op
// frees a random slot and fills it with a new block. After every round the
// arrays rotate between threads, so a thread keeps freeing blocks that some
// other thread allocated:
//
//   round r:    thread 0 -> slots[0]   thread 1 -> slots[1]   ...
//   round r+1:  thread 0 -> slots[1]   thread 1 -> slots[2]   ...
//               (a barrier between rounds hands each array over)
//
// Metric: million ops per second across all threads (op = free + malloc).
#include <barrier>
#include <cstdlib>
#include <thread>
#include <vector>

#include "bench_common.h"

namespace {

constexpr std::size_t kSlots = 1000;
constexpr unsigned kRounds = 1000;
constexpr unsigned kOpsPerRound = 20000;

void* new_block(bench::Rng& rng) {
  auto* p = static_cast<unsigned char*>(std::malloc(rng.between(16, 256)));
  p[0] = 1;  // touch it, as a real program would
  return p;
}

}  // namespace

int main(int argc, char** argv) {
  const unsigned threads = bench::arg_threads(argc, argv);
  std::vector<std::vector<void*>> slots(threads, std::vector<void*>(kSlots));
  {
    bench::Rng rng(42);
    for (auto& arr : slots) {
      for (auto& p : arr) p = new_block(rng);
    }
  }

  std::barrier sync(static_cast<std::ptrdiff_t>(threads) + 1);
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      bench::Rng rng(t + 1);
      sync.arrive_and_wait();  // start line
      for (unsigned r = 0; r < kRounds; ++r) {
        std::vector<void*>& arr = slots[(t + r) % threads];
        for (unsigned i = 0; i < kOpsPerRound; ++i) {
          void*& slot = arr[rng.below(kSlots)];
          std::free(slot);
          slot = new_block(rng);
        }
        sync.arrive_and_wait();  // hand the array to the next thread
      }
    });
  }

  sync.arrive_and_wait();
  const double start = bench::now_seconds();
  for (unsigned r = 0; r < kRounds; ++r) sync.arrive_and_wait();
  const double elapsed = bench::now_seconds() - start;
  for (auto& th : pool) th.join();

  for (auto& arr : slots) {
    for (void* p : arr) std::free(p);
  }
  const double ops = static_cast<double>(threads) * kRounds * kOpsPerRound;
  bench::row("larson", threads, "mops", ops / elapsed / 1e6);
  bench::row("larson", threads, "max_rss_mib",
             static_cast<double>(bench::max_rss_bytes()) / (1 << 20));
  return 0;
}
