#include "os.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>

namespace memloc {

std::size_t os_page_size() {
  // getpagesize() is async-signal-safe and does not allocate. On arm64 it can
  // be 4, 16 or 64 KiB, all of which divide our 64 KiB unit. A racy but
  // idempotent cache, because a function-local static would need
  // __cxa_guard_acquire from libstdc++.
  static constinit std::atomic<std::size_t> cached{0};
  std::size_t page = cached.load(std::memory_order_relaxed);
  if (page == 0) {
    page = static_cast<std::size_t>(getpagesize());
    cached.store(page, std::memory_order_relaxed);
  }
  return page;
}

namespace {

// Bytes currently mapped through this file, and the high-water mark (D-28).
// Updated only next to a system call, so relaxed atomics cost nothing
// measurable.
constinit std::atomic<std::size_t> g_mapped{0};
constinit std::atomic<std::size_t> g_peak_mapped{0};

void account_map(std::size_t len) {
  const std::size_t now = g_mapped.fetch_add(len, std::memory_order_relaxed) + len;
  std::size_t peak = g_peak_mapped.load(std::memory_order_relaxed);
  while (now > peak && !g_peak_mapped.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
  }
}

void account_unmap(std::size_t len) { g_mapped.fetch_sub(len, std::memory_order_relaxed); }

}  // namespace

std::size_t os_mapped_bytes() { return g_mapped.load(std::memory_order_relaxed); }
std::size_t os_peak_mapped_bytes() { return g_peak_mapped.load(std::memory_order_relaxed); }

namespace {

void* map_raw(std::size_t len) {
  void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}

}  // namespace

void* os_map(std::size_t len) {
  void* p = map_raw(len);
  if (p != nullptr) account_map(len);
  return p;
}

void* os_map_aligned(std::size_t len, std::size_t align) {
  // mmap only guarantees page alignment. Map len + align bytes, which must
  // contain an aligned window of len bytes, then give back the slop on both sides.
  // Only the kept window is accounted, so the transient slop never shows up
  // in the peak.
  auto* raw = static_cast<char*>(map_raw(len + align));
  if (raw == nullptr) return nullptr;
  const auto addr = reinterpret_cast<std::uintptr_t>(raw);
  char* aligned = raw + ((align - (addr & (align - 1))) & (align - 1));
  const auto head = static_cast<std::size_t>(aligned - raw);
  const std::size_t tail = align - head;
  if (head != 0) munmap(raw, head);
  if (tail != 0) munmap(aligned + len, tail);
  account_map(len);
  return aligned;
}

void os_unmap(void* p, std::size_t len) {
  if (munmap(p, len) == 0) account_unmap(len);
}

void* os_remap(void* p, std::size_t old_len, std::size_t new_len) {
  void* q = mremap(p, old_len, new_len, MREMAP_MAYMOVE);
  if (q == MAP_FAILED) return nullptr;
  if (new_len > old_len) {
    account_map(new_len - old_len);
  } else {
    account_unmap(old_len - new_len);
  }
  return q;
}

}  // namespace memloc
