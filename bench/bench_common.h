// bench_common.h - shared helpers for the bench_* programs. (D-30, D-31)
//
// Every benchmark calls plain malloc/free/realloc and is built -fno-builtin,
// so the allocator is whatever the dynamic linker binds:
//
//   ./bench_larson 8                          -> glibc
//   LD_PRELOAD=libmimalloc.so ./bench_larson 8 -> mimalloc
//   LD_PRELOAD=libmemloc.so   ./bench_larson 8 -> MemLoc
//
// Each program prints CSV rows "bench,threads,metric,value" on stdout;
// tools/bench.py adds the allocator and run number. Setup that allocates
// (thread objects, pointer arrays) happens before the clock starts.
#pragma once

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bench {

// xorshift64*: fast, deterministic per seed, and allocation-free.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : s_(seed * 0x9E3779B97F4A7C15ULL + 1) {}
  std::uint64_t next() {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    return s_ * 0x2545F4914F6CDD1DULL;
  }
  std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
  // Uniform in [lo, hi].
  std::size_t between(std::size_t lo, std::size_t hi) { return lo + below(hi - lo + 1); }
  // Log-uniform in [lo, hi): every power-of-two range is equally likely.
  std::size_t log_uniform(std::size_t lo, std::size_t hi) {
    const int lo_bit = 63 - __builtin_clzll(lo);
    const int hi_bit = 63 - __builtin_clzll(hi);
    const std::size_t base =
        std::size_t{1} << (lo_bit +
                           static_cast<int>(below(static_cast<std::size_t>(hi_bit - lo_bit))));
    const std::size_t n = base + below(base);
    return n < lo ? lo : (n >= hi ? hi - 1 : n);
  }

 private:
  std::uint64_t s_;
};

inline double now_seconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

inline unsigned arg_threads(int argc, char** argv) {
  const int t = argc > 1 ? std::atoi(argv[1]) : 1;
  return t > 0 ? static_cast<unsigned>(t) : 1;
}

// Peak resident set size of this process so far: VmHWM from
// /proc/self/status. (getrusage's ru_maxrss is not usable here: it survives
// exec, so it would report the Python driver's peak when that is larger.)
inline std::size_t max_rss_bytes() {
  std::FILE* f = std::fopen("/proc/self/status", "r");
  if (f == nullptr) return 0;
  char line[256];
  unsigned long kib = 0;
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    if (std::sscanf(line, "VmHWM: %lu kB", &kib) == 1) break;
  }
  std::fclose(f);
  return static_cast<std::size_t>(kib) * 1024;
}

// Current resident set size, from /proc/self/statm (pages -> bytes).
inline std::size_t current_rss_bytes() {
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr) return 0;
  unsigned long size = 0;
  unsigned long resident = 0;
  const int got = std::fscanf(f, "%lu %lu", &size, &resident);
  std::fclose(f);
  return got == 2 ? resident * static_cast<std::size_t>(sysconf(_SC_PAGESIZE)) : 0;
}

inline void row(const char* bench, unsigned threads, const char* metric, double value) {
  std::printf("%s,%u,%s,%.6g\n", bench, threads, metric, value);
}

}  // namespace bench
