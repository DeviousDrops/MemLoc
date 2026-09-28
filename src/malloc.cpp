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
#include <algorithm>
#include <cerrno>
#include <cstring>

#include "config.h"
#include "huge.h"
#include "memloc/memloc.h"
#include "page_heap.h"
#include "size_class.h"
#include "thread_cache.h"

namespace memloc {
namespace {

void* small_alloc(unsigned cls) { return tc_alloc(cls); }

void small_free(unsigned cls, void* p) { tc_free(cls, p); }

unsigned medium_units(std::size_t n) {
  return static_cast<unsigned>(std::max<std::size_t>(align_up(n, kUnitSize) >> kUnitShift, 1));
}

void* medium_alloc(std::size_t n) {
  Span* s = g_page_heap.alloc_span(medium_units(n));
  if (s == nullptr) {
    errno = ENOMEM;
    return nullptr;
  }
  s->kind = SpanKind::kMedium;
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

}  // namespace
}  // namespace memloc

using namespace memloc;

extern "C" {

void* memloc_malloc(size_t size) { return alloc(size); }

void memloc_free(void* ptr) {
  if (ptr != nullptr) dealloc(ptr);
}

void* memloc_calloc(size_t count, size_t size) {
  size_t total = 0;
  if (__builtin_mul_overflow(count, size, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  void* p = alloc(total);
  // Huge blocks are fresh anonymous mappings and therefore already zero;
  // skipping the memset avoids faulting in every page up front.
  if (p != nullptr && total <= kMaxMedium) std::memset(p, 0, total);
  return p;
}

void* memloc_realloc(void* ptr, size_t size) {
  if (ptr == nullptr) return alloc(size);
  if (size == 0) {  // glibc semantics: free and return NULL. D-9
    dealloc(ptr);
    return nullptr;
  }
  if (in_chunk(ptr)) {
    if (fits_in_place(span_of(ptr), size)) return ptr;
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

size_t memloc_malloc_usable_size(const void* ptr) { return ptr == nullptr ? 0 : usable_size(ptr); }

}  // extern "C"
