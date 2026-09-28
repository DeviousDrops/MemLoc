// malloc.cpp - public entry points. Routes each request to the small, medium
// or huge tier and implements the API-level semantics. (D-4, D-9, D-10)
//
//   memloc_malloc(n)
//     n <= 32 KiB  -> small_alloc(size_to_class(n))     thread cache -> central list
//     n <=  1 MiB  -> medium_alloc(n)                   one page-heap span
//     otherwise    -> huge_alloc(n)                     its own mmap
//
//   memloc_free(p)
//     in_chunk(p)?  span_of(p)->kind == kSmall   -> back to its class
//                   span_of(p)->kind == kMedium  -> span back to the page heap
//     otherwise     huge_free(p)                    munmap
//
// malloc, calloc and free are split in two (D-22): an inline fast path that
// only pops or pushes the thread cache, and a noinline slow path for
// everything else. Only the slow paths (and the rarer entry points) test
// whether debug mode is on, so the hot path contains no trace of it.
#include <algorithm>
#include <cerrno>
#include <cstring>

#include "central.h"
#include "config.h"
#include "debug.h"
#include "fork.h"
#include "huge.h"
#include "memloc/memloc.h"
#include "page_heap.h"
#include "size_class.h"
#include "thread_cache.h"

namespace memloc {
namespace {

void* small_alloc(unsigned cls) {
  void* p = tc_pop(cls);
  return p != nullptr ? p : tc_refill(cls);
}

void small_free(unsigned cls, void* p) {
  if (!tc_push(cls, p)) tc_free_slow(cls, p);
}

unsigned medium_units(std::size_t n) {
  return static_cast<unsigned>(std::max<std::size_t>(align_up(n, kUnitSize) >> kUnitShift, 1));
}

void* medium_alloc(std::size_t n) {
  install_fork_handlers();  // small allocations do this on cache activation
  Span* s = g_page_heap.alloc_span(medium_units(n), SpanKind::kMedium);
  if (s == nullptr) {
    errno = ENOMEM;
    return nullptr;
  }
  return span_base(s);
}

void* alloc(std::size_t n) {
  if (n <= kMaxSmall) return small_alloc(size_to_class(n));
  if (n <= kMaxMedium) return medium_alloc(n);
  return huge_alloc(n, kMinAlign);
}

void dealloc(void* p) {
  if (!in_chunk(p)) {
    huge_free(p);
    return;
  }
  Span* s = span_of(p);
  if (s->kind == SpanKind::kSmall) {
    small_free(s->size_class, p);
  } else {
    g_page_heap.free_span(s);
  }
}

std::size_t usable_size(const void* p) {
  if (!in_chunk(p)) return huge_usable_size(p);
  const Span* s = span_of(p);
  return s->kind == SpanKind::kSmall ? class_size(s->size_class) : span_bytes(s);
}

// True when a block of the given span can hold n bytes without moving.
bool fits_in_place(const Span* s, std::size_t n) {
  if (s->kind == SpanKind::kSmall) return n <= kMaxSmall && size_to_class(n) == s->size_class;
  return n > kMaxSmall && n <= kMaxMedium && medium_units(n) == s->units;
}

void* aligned(std::size_t align, std::size_t n) {
  if (debug_enabled()) [[unlikely]] {
    return debug_alloc(n, align, false);
  }
  if (align <= kMinAlign) return alloc(n);
  if (align <= kUnitSize) {
    // Span bases are 64 KiB-aligned, so any class whose size is a multiple of
    // align yields only aligned objects. D-10
    if (n <= kMaxSmall) {
      for (unsigned c = size_to_class(std::max(n, align)); c < kNumClasses; ++c) {
        if (class_size(c) % align == 0) return small_alloc(c);
      }
    }
    if (n <= kMaxMedium) return medium_alloc(n);
  }
  return huge_alloc(n, align);
}

__attribute__((noinline)) void* malloc_slow(std::size_t n) {
  if (debug_enabled()) return debug_alloc(n, kMinAlign, false);
  return alloc(n);
}

__attribute__((noinline)) void* calloc_slow(std::size_t n) {
  if (debug_enabled()) return debug_alloc(n, kMinAlign, true);
  void* p = alloc(n);
  // Huge blocks are fresh anonymous mappings and therefore already zero;
  // skipping the memset avoids faulting in every page up front.
  if (p != nullptr && n <= kMaxMedium) std::memset(p, 0, n);
  return p;
}

__attribute__((noinline)) void free_slow(void* p) {
  if (debug_enabled()) {
    debug_free(p);
    return;
  }
  dealloc(p);
}

}  // namespace

// --- uncached allocation, the debug allocator's backing store --------------

void* uncached_alloc(std::size_t n) {
  if (n > kMaxSmall) return n <= kMaxMedium ? medium_alloc(n) : huge_alloc(n, kMinAlign);
  const unsigned cls = size_to_class(n);
  void* p = nullptr;
  if (g_central[cls].fetch(cls, 1, &p) == 0) {
    errno = ENOMEM;
    return nullptr;
  }
  return p;
}

void uncached_free(void* p) {
  if (!in_chunk(p)) {
    huge_free(p);
    return;
  }
  Span* s = span_of(p);
  if (s->kind == SpanKind::kSmall) {
    next_of(p) = nullptr;
    g_central[s->size_class].release(p);
  } else {
    g_page_heap.free_span(s);
  }
}

std::size_t uncached_usable_size(const void* p) { return usable_size(p); }

}  // namespace memloc

using namespace memloc;

extern "C" {

void* memloc_malloc(size_t size) {
  if (size <= kMaxSmall) [[likely]] {
    if (void* p = tc_pop(size_to_class(size))) return p;
  }
  return malloc_slow(size);
}

void memloc_free(void* ptr) {
  if (ptr == nullptr) return;
  if (in_chunk(ptr)) [[likely]] {
    const Span* s = span_of(ptr);
    if (s->kind == SpanKind::kSmall && tc_push(s->size_class, ptr)) return;
  }
  free_slow(ptr);
}

void* memloc_calloc(size_t count, size_t size) {
  size_t total = 0;
  if (__builtin_mul_overflow(count, size, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  if (total <= kMaxSmall) [[likely]] {
    if (void* p = tc_pop(size_to_class(total))) return std::memset(p, 0, total);
  }
  return calloc_slow(total);
}

void* memloc_realloc(void* ptr, size_t size) {
  if (debug_enabled()) [[unlikely]] {
    return debug_realloc(ptr, size);
  }
  if (ptr == nullptr) return alloc(size);
  if (size == 0) {  // glibc semantics: free and return NULL. D-9
    dealloc(ptr);
    return nullptr;
  }
  if (in_chunk(ptr)) {
    Span* s = span_of(ptr);
    if (fits_in_place(s, size)) return ptr;
    // A medium block staying medium can often grow into the free units
    // right after it (or give back its tail) instead of moving.
    if (s->kind == SpanKind::kMedium && size > kMaxSmall && size <= kMaxMedium &&
        g_page_heap.resize_span(s, medium_units(size))) {
      return ptr;
    }
  } else if (size > kMaxMedium) {
    return huge_realloc(ptr, size);  // mremap: no copy. D-11
  }
  void* q = alloc(size);
  if (q == nullptr) return nullptr;  // the original block stays valid
  std::memcpy(q, ptr, std::min(size, usable_size(ptr)));
  dealloc(ptr);
  return q;
}

int memloc_posix_memalign(void** out, size_t alignment, size_t size) {
  if (!is_pow2(alignment) || alignment % sizeof(void*) != 0) return EINVAL;
  const int saved_errno = errno;  // POSIX: report through the return value only
  void* p = aligned(alignment, size);
  errno = saved_errno;
  if (p == nullptr) return ENOMEM;
  *out = p;
  return 0;
}

void* memloc_aligned_alloc(size_t alignment, size_t size) {
  if (!is_pow2(alignment)) {
    errno = EINVAL;
    return nullptr;
  }
  return aligned(alignment, size);
}

size_t memloc_malloc_usable_size(const void* ptr) {
  if (ptr == nullptr) return 0;
  if (debug_enabled()) [[unlikely]] {
    return debug_usable_size(ptr);
  }
  return usable_size(ptr);
}

}  // extern "C"
