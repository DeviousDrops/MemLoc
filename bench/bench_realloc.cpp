// bench_realloc.cpp - growing buffers with realloc. (D-31)
//
//   usage: bench_realloc [threads, ignored: always 1]
//
// 64 buffers grow to 1 MiB in steps of 1-64 bytes, taking turns:
//
//   buf 0 += 17 B, buf 1 += 3 B, ..., buf 63 += 40 B, buf 0 += 9 B, ...
//
// The interleaving puts other buffers' blocks next to each one, so a buffer
// cannot simply keep growing in place, which is what a single growing string
// would measure. The newly added bytes are written each time, as an append
// would. The growth path crosses MemLoc's small, medium and huge tiers
// (32 KiB and 1 MiB boundaries sit inside the run).
//
// Metric: nanoseconds per realloc call.
#include <cstdlib>
#include <cstring>

#include "bench_common.h"

namespace {

constexpr int kBuffers = 64;
constexpr std::size_t kFinal = std::size_t{1} << 20;

}  // namespace

int main() {
  char* buf[kBuffers] = {};
  std::size_t len[kBuffers] = {};
  bench::Rng rng(7);
  std::size_t calls = 0;

  const double start = bench::now_seconds();
  for (bool growing = true; growing;) {
    growing = false;
    for (int i = 0; i < kBuffers; ++i) {
      if (len[i] >= kFinal) continue;
      growing = true;
      const std::size_t step = rng.between(1, 64);
      buf[i] = static_cast<char*>(std::realloc(buf[i], len[i] + step));
      std::memset(buf[i] + len[i], static_cast<int>(i), step);
      len[i] += step;
      ++calls;
    }
  }
  const double elapsed = bench::now_seconds() - start;

  unsigned long check = 0;
  for (int i = 0; i < kBuffers; ++i) {
    check += static_cast<unsigned char>(buf[i][len[i] - 1]);
    std::free(buf[i]);
  }
  bench::row("realloc", 1, "ns_per_call", elapsed * 1e9 / static_cast<double>(calls));
  bench::row("realloc", 1, "calls", static_cast<double>(calls));
  bench::row("realloc", 1, "max_rss_mib", static_cast<double>(bench::max_rss_bytes()) / (1 << 20));
  return check == 0 ? 1 : 0;  // keeps the writes observable
}
