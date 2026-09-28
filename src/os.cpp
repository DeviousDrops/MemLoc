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

void* os_map(std::size_t len) {
  void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}

void* os_map_aligned(std::size_t len, std::size_t align) {
  // mmap only guarantees page alignment. Map len + align bytes, which must
  // contain an aligned window of len bytes, then give back the slop on both sides.
  auto* raw = static_cast<char*>(os_map(len + align));
  if (raw == nullptr) return nullptr;
  const auto addr = reinterpret_cast<std::uintptr_t>(raw);
  char* aligned = raw + ((align - (addr & (align - 1))) & (align - 1));
  const auto head = static_cast<std::size_t>(aligned - raw);
  const std::size_t tail = align - head;
  if (head != 0) os_unmap(raw, head);
  if (tail != 0) os_unmap(aligned + len, tail);
  return aligned;
}

void os_unmap(void* p, std::size_t len) { munmap(p, len); }

void* os_remap(void* p, std::size_t old_len, std::size_t new_len) {
  void* q = mremap(p, old_len, new_len, MREMAP_MAYMOVE);
  return q == MAP_FAILED ? nullptr : q;
}

}  // namespace memloc
