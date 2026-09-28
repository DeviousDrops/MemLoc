#include "debug.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>  // std::scoped_lock

#include "chunk.h"
#include "config.h"
#include "fork.h"
#include "huge.h"
#include "mutex.h"
#include "page_heap.h"
#include "report.h"
#include "size_class.h"
#include "thread_cache.h"

namespace memloc {

constinit std::atomic<bool> g_debug{false};

namespace {

struct DebugHeader {
  std::uint64_t magic;  // kLiveMagic or kFreedMagic, XORed with the user address
  std::size_t size;     // requested size
  char* raw;            // the underlying ordinary block
  DebugHeader* prev;    // live list
  DebugHeader* next;
  std::uint32_t alloc_tid;
  std::uint32_t free_tid;
  void* alloc_stack[kStoredFrames];
  void* free_stack[kStoredFrames];
  std::uint8_t alloc_depth;
  std::uint8_t free_depth;
  std::uint8_t reserved[14];
};

inline constexpr std::size_t kCanary = 16;
inline constexpr std::size_t kUserOffset = sizeof(DebugHeader) + kCanary;
static_assert(sizeof(DebugHeader) == 192, "D-23 layout");
static_assert(kUserOffset == 208 && kUserOffset % kMinAlign == 0,
              "user pointers must keep the normal alignment");

inline constexpr std::uint64_t kLiveMagic = 0x4d454d4c4f43444cULL;   // "MEMLOCDL"
inline constexpr std::uint64_t kFreedMagic = 0x4d454d4c4f434446ULL;  // "MEMLOCDF"
inline constexpr unsigned char kCanaryByte = 0xCA;
inline constexpr unsigned char kAllocFill = 0xBE;
inline constexpr unsigned char kFreeFill = 0xDD;

inline constexpr unsigned kQuarantineBlocks = 4096;
inline constexpr std::size_t kQuarantineBytes = std::size_t{64} << 20;

// --- state -------------------------------------------------------------------
// Lock order: g_mu -> central lists -> page heap. Nothing that may allocate
// (backtrace, dladdr) runs under g_mu except while reporting a fatal error.

Mutex g_mu;
DebugHeader* g_live = nullptr;                               // guarded by g_mu
DebugHeader* g_quarantine[kQuarantineBlocks];                // FIFO ring, guarded by g_mu
unsigned g_q_head = 0;                                       // guarded by g_mu
std::atomic<std::size_t> g_live_blocks{0}, g_live_bytes{0};  // written under g_mu
std::atomic<std::size_t> g_q_blocks{0}, g_q_bytes{0};        // written under g_mu

// Set while the debug allocator itself is running on this thread: anything it
// allocates indirectly (backtrace, dladdr) gets an ordinary block. D-24
constinit thread_local bool t_busy __attribute__((tls_model("initial-exec"))) = false;

struct Busy {
  bool was = t_busy;
  Busy() { t_busy = true; }
  ~Busy() { t_busy = was; }
  Busy(const Busy&) = delete;
  Busy& operator=(const Busy&) = delete;
};

DebugHeader* header_of(const void* user) {
  return reinterpret_cast<DebugHeader*>(const_cast<char*>(static_cast<const char*>(user)) -
                                        kUserOffset);
}
char* user_of(DebugHeader* h) { return reinterpret_cast<char*>(h) + kUserOffset; }
std::uint64_t live_magic(const void* user) {
  return kLiveMagic ^ reinterpret_cast<std::uintptr_t>(user);
}
std::uint64_t freed_magic(const void* user) {
  return kFreedMagic ^ reinterpret_cast<std::uintptr_t>(user);
}

// --- classifying a pointer handed to free/realloc ----------------------------

enum class Kind : std::uint8_t { kOrdinary, kLive, kFreed, kInvalid };

struct Lookup {
  Kind kind = Kind::kInvalid;
  DebugHeader* h = nullptr;          // kLive / kFreed: the block's header
  DebugHeader* enclosing = nullptr;  // kInvalid: a debug block containing the pointer
};

// The debug block whose raw block starts at b, if any (unaligned blocks only).
DebugHeader* debug_block_at(char* b) {
  auto* h = reinterpret_cast<DebugHeader*>(b);
  const char* user = user_of(h);
  return h->magic == live_magic(user) || h->magic == freed_magic(user) ? h : nullptr;
}

Lookup classify(const void* p) {
  const auto addr = reinterpret_cast<std::uintptr_t>(p);
  if (addr < 4096) return {};
  // Every block and every user pointer is 16-aligned; a misaligned pointer
  // can at most point into one.
  const bool aligned = addr % kMinAlign == 0;
  char* const cp = const_cast<char*>(static_cast<const char*>(p));
  char* block = nullptr;  // start of the ordinary block containing p
  if (in_chunk(p)) {
    const Span* s = span_of(p);
    if (s->kind == SpanKind::kFree) return {};
    char* base = span_base(s);
    const std::size_t unit =
        s->kind == SpanKind::kSmall ? class_size(s->size_class) : span_bytes(s);
    block = base + static_cast<std::size_t>(cp - base) / unit * unit;
  } else if (!aligned) {
    return {};
  } else if (huge_header(p)->magic == kHugeMagic) {
    return {.kind = Kind::kOrdinary};
  }
  if (aligned && (block == nullptr || cp - block >= static_cast<std::ptrdiff_t>(kUserOffset))) {
    DebugHeader* h = header_of(p);
    if (h->magic == live_magic(p)) return {.kind = Kind::kLive, .h = h};
    if (h->magic == freed_magic(p)) return {.kind = Kind::kFreed, .h = h};
  }
  if (block == nullptr) return {};
  DebugHeader* enclosing = debug_block_at(block);
  if (cp == block && enclosing == nullptr) return {.kind = Kind::kOrdinary};
  return {.enclosing = enclosing};
}

// --- reporting ---------------------------------------------------------------

struct Detail {
  char data[256];
  Buf buf{data, sizeof(data) - 1};
  const char* c_str() {
    data[buf.size()] = '\0';
    return data;
  }
};

void add_block_stacks(Report& r, const DebugHeader* h) {
  if (h == nullptr) return;
  r.alloc_stack = {.pcs = h->alloc_stack, .depth = h->alloc_depth};
  r.alloc_tid = h->alloc_tid;
  if (h->free_depth > 0) {
    r.free_stack = {.pcs = h->free_stack, .depth = h->free_depth};
    r.free_tid = h->free_tid;
  }
}

// Emits r with the current stack attached, then aborts.
[[noreturn]] void fail(Report& r) {
  t_busy = true;  // the process is going down: no more debug bookkeeping
  void* pcs[kReportFrames];
  r.stack = {.pcs = pcs, .depth = capture_stack(pcs, kReportFrames)};
  emit_report(r);
  std::abort();
}

[[noreturn]] void fail_block(const char* type, const DebugHeader* h, const void* user,
                             const char* detail, std::int64_t offset, bool has_offset) {
  Report r;
  r.type = type;
  r.address = user;
  r.size = h->size;
  r.has_size = true;
  r.offset = offset;
  r.has_offset = has_offset;
  r.detail = detail;
  add_block_stacks(r, h);
  fail(r);
}

[[noreturn]] void fail_bad_pointer(const char* fn, const void* p, const Lookup& l) {
  Detail d;
  Report r;
  r.address = p;
  if (l.kind == Kind::kFreed) {
    r.type = "double-free";
    d.buf.add(fn);
    d.buf.add("() of a block that was already freed (");
    d.buf.dec(l.h->size);
    d.buf.add(" bytes)");
    r.size = l.h->size;
    r.has_size = true;
    add_block_stacks(r, l.h);
  } else if (l.enclosing != nullptr) {
    const char* user = user_of(l.enclosing);
    r.type = "invalid-free";
    r.offset = static_cast<const char*>(p) - user;
    r.has_offset = true;
    r.size = l.enclosing->size;
    r.has_size = true;
    d.buf.add(fn);
    d.buf.add("() of a pointer ");
    d.buf.sdec(r.offset);
    d.buf.add(" bytes into a ");
    d.buf.add(l.enclosing->magic == live_magic(user) ? "live " : "freed ");
    d.buf.dec(l.enclosing->size);
    d.buf.add("-byte block starting at ");
    d.buf.hex(reinterpret_cast<std::uintptr_t>(user));
    add_block_stacks(r, l.enclosing);
  } else {
    r.type = "invalid-free";
    d.buf.add(fn);
    d.buf.add("() of a pointer that MemLoc never returned (stack, global or wild address)");
  }
  r.detail = d.c_str();
  fail(r);
}

// Returns the offset of the first byte of [p, p+n) that is not `expect`, or
// -1; *bad counts all such bytes.
std::int64_t first_mismatch(const unsigned char* p, std::size_t n, unsigned char expect,
                            std::size_t* bad) {
  std::int64_t first = -1;
  *bad = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (p[i] != expect) {
      if (first < 0) first = static_cast<std::int64_t>(i);
      ++*bad;
    }
  }
  return first;
}

void check_canaries(const DebugHeader* h, const char* user) {
  const auto* u = reinterpret_cast<const unsigned char*>(user);
  std::size_t bad = 0;
  Detail d;
  if (const std::int64_t i = first_mismatch(u - kCanary, kCanary, kCanaryByte, &bad); i >= 0) {
    const std::int64_t off = i - static_cast<std::int64_t>(kCanary);
    d.buf.add("wrote before the start of a ");
    d.buf.dec(h->size);
    d.buf.add("-byte block: ");
    d.buf.dec(bad);
    d.buf.add(" of the 16 guard bytes in front of it changed, first at offset ");
    d.buf.sdec(off);
    fail_block("heap-buffer-underflow", h, user, d.c_str(), off, true);
  }
  if (const std::int64_t i = first_mismatch(u + h->size, kCanary, kCanaryByte, &bad); i >= 0) {
    const std::int64_t off = static_cast<std::int64_t>(h->size) + i;
    d.buf.add("wrote past the end of a ");
    d.buf.dec(h->size);
    d.buf.add("-byte block: ");
    d.buf.dec(bad);
    d.buf.add(" of the 16 guard bytes after it changed, first at offset ");
    d.buf.sdec(off);
    fail_block("heap-buffer-overflow", h, user, d.c_str(), off, true);
  }
}

// `when` says where the write was noticed: the current stack is that place,
// not the faulty write.
void check_poison(const DebugHeader* h, const char* when) {
  const char* user = user_of(const_cast<DebugHeader*>(h));
  std::size_t bad = 0;
  const std::int64_t i =
      first_mismatch(reinterpret_cast<const unsigned char*>(user), h->size, kFreeFill, &bad);
  if (i < 0) return;
  Detail d;
  d.buf.dec(bad);
  d.buf.add(" bytes of a freed ");
  d.buf.dec(h->size);
  d.buf.add("-byte block were written after free, first at offset ");
  d.buf.sdec(i);
  if (h->alloc_tid != h->free_tid) {
    d.buf.add(" (allocated by thread ");
    d.buf.dec(h->alloc_tid);
    d.buf.add(", freed by thread ");
    d.buf.dec(h->free_tid);
    d.buf.add(")");
  }
  d.buf.add("; detected ");
  d.buf.add(when);
  fail_block("use-after-free", h, user, d.c_str(), i, true);
}

// --- live list and quarantine (all require g_mu) ----------------------------

void link_live(DebugHeader* h) {
  h->prev = nullptr;
  h->next = g_live;
  if (g_live != nullptr) g_live->prev = h;
  g_live = h;
  g_live_blocks.fetch_add(1, std::memory_order_relaxed);
  g_live_bytes.fetch_add(h->size, std::memory_order_relaxed);
}

void unlink_live(DebugHeader* h) {
  if (h->prev != nullptr) h->prev->next = h->next;
  if (h->next != nullptr) h->next->prev = h->prev;
  if (g_live == h) g_live = h->next;
  g_live_blocks.fetch_sub(1, std::memory_order_relaxed);
  g_live_bytes.fetch_sub(h->size, std::memory_order_relaxed);
}

DebugHeader*& quarantine_slot(std::size_t i) {
  return g_quarantine[(g_q_head + i) % kQuarantineBlocks];
}

void evict_oldest() {
  DebugHeader* h = g_quarantine[g_q_head];
  g_q_head = (g_q_head + 1) % kQuarantineBlocks;
  g_q_blocks.fetch_sub(1, std::memory_order_relaxed);
  g_q_bytes.fetch_sub(h->size, std::memory_order_relaxed);
  check_poison(h, "when the block left quarantine");
  uncached_free(h->raw);
}

void quarantine(DebugHeader* h) {
  while (g_q_blocks.load(std::memory_order_relaxed) > 0 &&
         (g_q_blocks.load(std::memory_order_relaxed) >= kQuarantineBlocks ||
          g_q_bytes.load(std::memory_order_relaxed) + h->size > kQuarantineBytes)) {
    evict_oldest();
  }
  quarantine_slot(g_q_blocks.load(std::memory_order_relaxed)) = h;
  g_q_blocks.fetch_add(1, std::memory_order_relaxed);
  g_q_bytes.fetch_add(h->size, std::memory_order_relaxed);
}

void free_live(DebugHeader* h, void* p) {
  void* stack[kStoredFrames];
  unsigned depth = 0;
  {
    Busy busy;
    depth = capture_stack(stack, kStoredFrames);
  }
  std::scoped_lock lock(g_mu);
  if (h->magic != live_magic(p)) {  // lost a race with another free of p
    fail_bad_pointer("free", p, classify(p));
  }
  check_canaries(h, static_cast<char*>(p));
  unlink_live(h);
  h->magic = freed_magic(p);
  h->free_tid = current_tid();
  h->free_depth = static_cast<std::uint8_t>(depth);
  std::memcpy(h->free_stack, stack, sizeof(stack));
  std::memset(p, kFreeFill, h->size);
  quarantine(h);
}

// --- leak report at exit -----------------------------------------------------

struct LeakGroup {
  const DebugHeader* sample;  // nullptr = empty slot
  std::size_t blocks;
  std::size_t bytes;
  bool system;
  bool reported;
};

constexpr unsigned kLeakGroups = 1024;
LeakGroup g_groups[kLeakGroups];  // used once, at exit, under g_mu

bool same_stack(const DebugHeader* a, const DebugHeader* b) {
  return a->alloc_depth == b->alloc_depth &&
         std::memcmp(a->alloc_stack, b->alloc_stack, a->alloc_depth * sizeof(void*)) == 0;
}

LeakGroup* group_for(const DebugHeader* h) {
  std::uintptr_t hash = h->alloc_depth;
  for (unsigned i = 0; i < h->alloc_depth; ++i) {
    hash = (hash ^ reinterpret_cast<std::uintptr_t>(h->alloc_stack[i])) * 0x9e3779b97f4a7c15ULL;
  }
  for (unsigned probe = 0; probe < kLeakGroups; ++probe) {
    LeakGroup& g = g_groups[(hash + probe) % kLeakGroups];
    if (g.sample == nullptr) {
      g.sample = h;
      return &g;
    }
    if (same_stack(g.sample, h)) return &g;
  }
  return nullptr;
}

// Allocations made by system code on its own behalf (stdio buffers, thread
// TLS set up by the loader, library initialisers) are not the program's leaks.
bool is_system_stack(const DebugHeader* h) {
  static constexpr const char* kSystemCallers[] = {"_IO_file_doallocate", "_dl_allocate_tls"};
  bool all_system = true;
  for (unsigned i = 0; i < h->alloc_depth; ++i) {
    FrameInfo f{};
    if (!describe_pc(reinterpret_cast<std::uintptr_t>(h->alloc_stack[i]) - 1, &f)) continue;
    if (i == 0 && is_loader_module(f.module)) return true;
    for (const char* name : kSystemCallers) {
      if (f.symbol != nullptr && std::strcmp(f.symbol, name) == 0) return true;
    }
    all_system = all_system && is_system_module(f.module);
  }
  return all_system;
}

void report_leaks() {
  std::size_t blocks = 0, bytes = 0, system_blocks = 0, system_bytes = 0, sites = 0;
  std::size_t ungrouped_blocks = 0;
  for (const DebugHeader* h = g_live; h != nullptr; h = h->next) {
    LeakGroup* g = group_for(h);
    if (g == nullptr) {
      ++ungrouped_blocks;
      continue;
    }
    ++g->blocks;
    g->bytes += h->size;
  }
  for (LeakGroup& g : g_groups) {
    if (g.sample == nullptr) continue;
    g.system = is_system_stack(g.sample);
    if (g.system) {
      system_blocks += g.blocks;
      system_bytes += g.bytes;
    } else {
      blocks += g.blocks;
      bytes += g.bytes;
      ++sites;
    }
  }
  // One record per site, largest first; the tail only appears in the summary.
  constexpr unsigned kMaxSiteReports = 25;
  unsigned reported = 0;
  for (; reported < kMaxSiteReports; ++reported) {
    LeakGroup* top = nullptr;
    for (LeakGroup& g : g_groups) {
      if (g.sample != nullptr && !g.system && !g.reported &&
          (top == nullptr || g.bytes > top->bytes)) {
        top = &g;
      }
    }
    if (top == nullptr) break;
    top->reported = true;
    Detail d;
    d.buf.dec(top->blocks);
    d.buf.add(top->blocks == 1 ? " block" : " blocks");
    d.buf.add(" (");
    d.buf.dec(top->bytes);
    d.buf.add(" bytes) allocated here ");
    d.buf.add(top->blocks == 1 ? "was never freed" : "were never freed");
    Report r;
    r.type = "leak";
    r.address = user_of(const_cast<DebugHeader*>(top->sample));
    r.size = top->sample->size;
    r.has_size = true;
    r.detail = d.c_str();
    r.extra[0] = {.key = "blocks", .value = top->blocks};
    r.extra[1] = {.key = "bytes", .value = top->bytes};
    r.num_extra = 2;
    add_block_stacks(r, top->sample);
    r.human = false;
    emit_report(r);
  }
  if (sites == 0 && ungrouped_blocks == 0) return;
  Detail d;
  d.buf.dec(bytes);
  d.buf.add(" bytes in ");
  d.buf.dec(blocks);
  d.buf.add(" blocks leaked from ");
  d.buf.dec(sites);
  d.buf.add(sites == 1 ? " allocation site" : " allocation sites");
  Report r;
  r.type = "leak-summary";
  r.detail = d.c_str();
  r.extra[0] = {.key = "blocks", .value = blocks};
  r.extra[1] = {.key = "bytes", .value = bytes};
  r.extra[2] = {.key = "sites", .value = sites};
  r.extra[3] = {.key = "system_blocks", .value = system_blocks};
  r.extra[4] = {.key = "system_bytes", .value = system_bytes};
  r.num_extra = 5;
  emit_report(r);
}

// --- start-up and exit -------------------------------------------------------

__attribute__((constructor)) void debug_start() {
  const char* env = std::getenv("MEMLOC_DEBUG");
  if (env == nullptr || env[0] == '\0' || env[0] == '0') return;
  {
    Busy busy;
    report_init();
  }
  install_segv_handler();
  install_fork_handlers();  // uncached blocks never activate a thread cache
  // From here on this thread never caches; threads created later never
  // activate their caches because every slow path sees g_debug first.
  tc_disable();
  g_debug.store(true, std::memory_order_relaxed);
}

__attribute__((destructor)) void debug_finish() {
  if (!debug_enabled()) return;
  Busy busy;
  std::scoped_lock lock(g_mu);
  // Blocks still in quarantine are checked for late writes, live blocks for
  // overflows that were never followed by a free, and then the leaks.
  const std::size_t queued = g_q_blocks.load(std::memory_order_relaxed);
  for (std::size_t i = 0; i < queued; ++i) check_poison(quarantine_slot(i), "at exit");
  for (const DebugHeader* h = g_live; h != nullptr; h = h->next) {
    check_canaries(h, user_of(const_cast<DebugHeader*>(h)));
  }
  report_leaks();
}

}  // namespace

HeapSnapshot heap_snapshot() {
  constexpr auto kRelaxed = std::memory_order_relaxed;
  return {.mapped = g_page_heap.mapped_bytes(),
          .live_blocks = g_live_blocks.load(kRelaxed),
          .live_bytes = g_live_bytes.load(kRelaxed),
          .quarantine_blocks = g_q_blocks.load(kRelaxed),
          .quarantine_bytes = g_q_bytes.load(kRelaxed)};
}

void debug_lock() { g_mu.lock(); }
void debug_unlock() { g_mu.unlock(); }

void* debug_alloc(std::size_t n, std::size_t align, bool zero) {
  if (t_busy && align <= kMinAlign) {  // internal allocation: an ordinary block
    void* p = uncached_alloc(n);
    if (p != nullptr && zero) std::memset(p, 0, n);
    return p;
  }
  align = align < kMinAlign ? kMinAlign : align;
  const std::size_t slack = align > kMinAlign ? align : 0;
  std::size_t total = 0;
  if (__builtin_add_overflow(n, kUserOffset + kCanary + slack, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  auto* raw = static_cast<char*>(uncached_alloc(total));
  if (raw == nullptr) return nullptr;
  auto* user =
      reinterpret_cast<char*>(align_up(reinterpret_cast<std::uintptr_t>(raw) + kUserOffset, align));
  DebugHeader* h = header_of(user);
  std::memset(h, 0, sizeof(DebugHeader));
  h->size = n;
  h->raw = raw;
  h->alloc_tid = current_tid();
  {
    Busy busy;
    h->alloc_depth = static_cast<std::uint8_t>(capture_stack(h->alloc_stack, kStoredFrames));
  }
  std::memset(user - kCanary, kCanaryByte, kCanary);
  std::memset(user, zero ? 0 : kAllocFill, n);
  std::memset(user + n, kCanaryByte, kCanary);
  std::scoped_lock lock(g_mu);
  h->magic = live_magic(user);
  link_live(h);
  return user;
}

void debug_free(void* p) {
  const Lookup l = classify(p);
  switch (l.kind) {
    case Kind::kOrdinary:
      uncached_free(p);
      return;
    case Kind::kLive:
      free_live(l.h, p);
      return;
    case Kind::kFreed:
    case Kind::kInvalid:
      fail_bad_pointer("free", p, l);
  }
}

void* debug_realloc(void* p, std::size_t n) {
  if (p == nullptr) return debug_alloc(n, kMinAlign, false);
  if (n == 0) {  // glibc semantics, as in the normal path. D-9
    debug_free(p);
    return nullptr;
  }
  const Lookup l = classify(p);
  if (l.kind == Kind::kFreed || l.kind == Kind::kInvalid) fail_bad_pointer("realloc", p, l);
  // Always move, so the old address goes through quarantine and a stale
  // pointer to it is caught like any other use after free. D-23
  const std::size_t old_size = l.kind == Kind::kLive ? l.h->size : uncached_usable_size(p);
  void* q = debug_alloc(n, kMinAlign, false);
  if (q == nullptr) return nullptr;
  std::memcpy(q, p, old_size < n ? old_size : n);
  if (l.kind == Kind::kLive) {
    free_live(l.h, p);
  } else {
    uncached_free(p);
  }
  return q;
}

std::size_t debug_usable_size(const void* p) {
  const Lookup l = classify(p);
  switch (l.kind) {
    case Kind::kOrdinary:
      return uncached_usable_size(p);
    case Kind::kLive:
      return l.h->size;  // exactly what was asked for: the rest is canary
    case Kind::kFreed:
    case Kind::kInvalid:
      break;
  }
  fail_bad_pointer("malloc_usable_size", p, l);
}

}  // namespace memloc
