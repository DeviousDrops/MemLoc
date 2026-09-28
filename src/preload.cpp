// preload.cpp - the standard malloc family, for libmemloc.so only. (D-18)
//
//   $ LD_PRELOAD=libmemloc.so ./program
//
//   symbol lookup order:  program -> libmemloc.so -> libc.so.6 -> ...
//                                    ^ malloc/free/... found here first
//
// Every name the glibc manual requires of a replacement ("Replacing malloc")
// is defined here as a thin wrapper around the memloc_* entry points. glibc's
// own internal allocations (stdio buffers, getaddrinfo, dlerror, ...) resolve
// to these too, and C++ operator new/delete reach them through libstdc++.
// No dlsym, no constructor, no "initialised yet?" flag: the allocator
// works from the first call (D-19).
//
// Declarations come from the libc headers so the signatures (including
// glibc's noexcept) are checked; this file is built with -fno-builtin-* so
// the compiler cannot turn a wrapper into a call to itself.
#include <malloc.h>

#include <cerrno>
#include <cstdlib>

#include "config.h"
#include "memloc/memloc.h"
#include "os.h"

namespace {

// glibc's memalign accepts any alignment and rounds it up to a power of two.
void* memalign_impl(size_t alignment, size_t size) {
  if (alignment > SIZE_MAX / 2 + 1) {
    errno = EINVAL;
    return nullptr;
  }
  if (!memloc::is_pow2(alignment)) alignment = std::size_t{1} << (64 - __builtin_clzl(alignment));
  return memloc_aligned_alloc(alignment, size);
}

}  // namespace

#define MEMLOC_EXPORT extern "C" __attribute__((visibility("default")))

MEMLOC_EXPORT void* malloc(size_t size) noexcept { return memloc_malloc(size); }

MEMLOC_EXPORT void free(void* ptr) noexcept { memloc_free(ptr); }

MEMLOC_EXPORT void* calloc(size_t count, size_t size) noexcept {
  return memloc_calloc(count, size);
}

MEMLOC_EXPORT void* realloc(void* ptr, size_t size) noexcept { return memloc_realloc(ptr, size); }

MEMLOC_EXPORT void* reallocarray(void* ptr, size_t count, size_t size) noexcept {
  size_t total = 0;
  if (__builtin_mul_overflow(count, size, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  return memloc_realloc(ptr, total);
}

MEMLOC_EXPORT int posix_memalign(void** out, size_t alignment, size_t size) noexcept {
  return memloc_posix_memalign(out, alignment, size);
}

MEMLOC_EXPORT void* aligned_alloc(size_t alignment, size_t size) noexcept {
  return memloc_aligned_alloc(alignment, size);
}

MEMLOC_EXPORT void* memalign(size_t alignment, size_t size) noexcept {
  return memalign_impl(alignment, size);
}

MEMLOC_EXPORT void* valloc(size_t size) noexcept {
  return memalign_impl(memloc::os_page_size(), size);
}

MEMLOC_EXPORT void* pvalloc(size_t size) noexcept {
  const size_t page = memloc::os_page_size();
  if (size > SIZE_MAX - page) {
    errno = ENOMEM;
    return nullptr;
  }
  return memalign_impl(page, size == 0 ? page : memloc::align_up(size, page));
}

MEMLOC_EXPORT size_t malloc_usable_size(void* ptr) noexcept {
  return memloc_malloc_usable_size(ptr);
}
