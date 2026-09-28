// stats.cpp - memloc_stats(), memloc_stats_print() and MEMLOC_STATS=1. (D-28, D-29)
//
// Nothing on the malloc/free paths counts anything. The numbers are computed
// on demand by walking the heap with every lock held:
//
//   lock order (same as the fork handler, D-20):
//     central[0] .. central[39]  ->  page heap
//
//   for each chunk (page heap list):         Chunk::used bitmap
//     unit 0            header               |1|1|1|1|0|0|1|1|...|
//     used run at u     spans[u]              ^ ^     ^
//        kSmall  -> class, units, live        | |     +- free unit
//        kMedium -> units                     | +------- span continues
//     unused unit       unused_chunk_bytes    +--------- header
//
//   small blocks handed out = sum(span.live) - blocks parked in transfer batches
//
// Huge blocks and the mapped totals come from atomics kept next to the mmap
// calls (huge.cpp, os.cpp). "Allocated" includes blocks sitting in thread
// caches, which cannot be told apart from blocks in use without counting on
// the fast path.
#include <unistd.h>

#include <cstdint>
#include <cstdlib>

#include "central.h"
#include "chunk.h"
#include "config.h"
#include "huge.h"
#include "memloc/memloc.h"
#include "os.h"
#include "page_heap.h"
#include "report.h"
#include "size_class.h"

namespace memloc {
namespace {

struct ClassStats {
  std::size_t spans = 0;
  std::size_t span_bytes = 0;
  std::size_t blocks_out = 0;
};

struct Snapshot {
  struct memloc_stats totals{};
  ClassStats cls[kNumClasses]{};
};

void lock_heap() {
  for (auto& list : g_central) list.lock();
  g_page_heap.lock();
}

void unlock_heap() {
  g_page_heap.unlock();
  for (unsigned c = kNumClasses; c-- > 0;) g_central[c].unlock();
}

void take_snapshot(Snapshot& snap) {
  struct memloc_stats& t = snap.totals;
  lock_heap();
  for (Chunk* c = g_page_heap.first_chunk(); c != nullptr; c = c->next) {
    ++t.chunks;
    unsigned u = 1;  // unit 0 is the chunk header
    while (u < kUnitsPerChunk) {
      if (((c->used >> u) & 1) == 0) {
        t.unused_chunk_bytes += kUnitSize;
        ++u;
        continue;
      }
      const Span& s = c->spans[u];
      if (s.kind == SpanKind::kSmall) {
        ClassStats& cs = snap.cls[s.size_class];
        ++cs.spans;
        cs.span_bytes += span_bytes(&s);
        cs.blocks_out += s.live;
      } else if (s.kind == SpanKind::kMedium) {
        t.medium_allocated += span_bytes(&s);
      }
      u += s.units;
    }
  }
  for (unsigned c = 0; c < kNumClasses; ++c) {
    ClassStats& cs = snap.cls[c];
    cs.blocks_out -= std::size_t{g_central[c].cached_batches_locked()} * batch_size(c);
    t.small_span_bytes += cs.span_bytes;
    t.small_allocated += cs.blocks_out * class_size(c);
  }
  unlock_heap();

  t.huge_allocated = huge_live_bytes();
  t.huge_count = huge_live_count();
  t.allocated = t.small_allocated + t.medium_allocated + t.huge_allocated;
  t.mapped = os_mapped_bytes();
  t.peak_mapped = os_peak_mapped_bytes();
}

// Right-aligns v in a field of `width` characters.
void pad_dec(Buf& b, std::uint64_t v, unsigned width) {
  char tmp[24];
  Buf digits(tmp, sizeof(tmp));
  digits.dec(v);
  for (std::size_t i = digits.size(); i < width; ++i) b.add(" ");
  b.add(digits.data(), digits.size());
}

void add_mib(Buf& b, std::size_t bytes) {
  // Two decimals without floating point: bytes * 100 / MiB, rounded.
  const std::uint64_t centi = (std::uint64_t{bytes} * 100 + (1U << 19)) >> 20;
  b.dec(centi / 100);
  b.add(".");
  if (centi % 100 < 10) b.add("0");
  b.dec(centi % 100);
  b.add(" MiB");
}

void total_line(Buf& b, const char* label, std::size_t bytes) {
  b.add("  ");
  b.add(label);
  add_mib(b, bytes);
  b.add("\n");
}

void flush(Buf& b, int fd) {
  const char* p = b.data();
  std::size_t left = b.size();
  while (left > 0) {
    const ssize_t n = write(fd, p, left);
    if (n <= 0) break;
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  b.clear();
}

// Runs late (low priority), after most of the program's own destructors.
__attribute__((destructor(101))) void stats_at_exit() {
  const char* env = std::getenv("MEMLOC_STATS");
  if (env != nullptr && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0')) {
    memloc_stats_print(STDERR_FILENO);
  }
}

}  // namespace
}  // namespace memloc

extern "C" {

void memloc_stats(struct memloc_stats* out) {
  memloc::Snapshot snap;
  memloc::take_snapshot(snap);
  *out = snap.totals;
}

void memloc_stats_print(int fd) {
  using memloc::Buf;
  memloc::Snapshot snap;
  memloc::take_snapshot(snap);
  const struct memloc_stats& t = snap.totals;

  char storage[1024];
  Buf b(storage, sizeof(storage));
  b.add("memloc stats (pid ");
  b.dec(static_cast<std::uint64_t>(getpid()));
  b.add(")\n");
  memloc::total_line(b, "mapped from OS     ", t.mapped);
  memloc::total_line(b, "peak mapped        ", t.peak_mapped);
  memloc::total_line(b, "allocated          ", t.allocated);
  memloc::total_line(b, "  small            ", t.small_allocated);
  memloc::total_line(b, "  medium           ", t.medium_allocated);
  memloc::total_line(b, "  huge             ", t.huge_allocated);
  memloc::total_line(b, "small span space   ", t.small_span_bytes);
  memloc::total_line(b, "unused chunk space ", t.unused_chunk_bytes);
  b.add("  chunks ");
  b.dec(t.chunks);
  b.add(", huge blocks ");
  b.dec(t.huge_count);
  b.add("\n  class  block B  spans  span KiB  blocks out  used %\n");
  memloc::flush(b, fd);

  for (unsigned c = 0; c < memloc::kNumClasses; ++c) {
    const memloc::ClassStats& cs = snap.cls[c];
    if (cs.spans == 0) continue;
    memloc::pad_dec(b, c, 7);
    memloc::pad_dec(b, memloc::class_size(c), 9);
    memloc::pad_dec(b, cs.spans, 7);
    memloc::pad_dec(b, cs.span_bytes >> 10, 10);
    memloc::pad_dec(b, cs.blocks_out, 12);
    memloc::pad_dec(b, cs.blocks_out * memloc::class_size(c) * 100 / cs.span_bytes, 8);
    b.add("\n");
    memloc::flush(b, fd);
  }
}

}  // extern "C"
