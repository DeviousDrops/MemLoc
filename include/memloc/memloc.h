/*
 * memloc.h - public C API of the MemLoc allocator.
 *
 * The memloc_* functions are the allocator proper and follow glibc's semantics
 * for the corresponding standard function (see D-9 in docs/DECISIONS.md). The
 * shared library (libmemloc.so) additionally exports the standard malloc family
 * as aliases of these, so it can replace glibc malloc via LD_PRELOAD. Tests and
 * benchmarks call the memloc_* names directly.
 */
#ifndef MEMLOC_MEMLOC_H
#define MEMLOC_MEMLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* memloc_malloc(size_t size);
void memloc_free(void* ptr);
void* memloc_calloc(size_t count, size_t size);
void* memloc_realloc(void* ptr, size_t size);
int memloc_posix_memalign(void** out, size_t alignment, size_t size);
void* memloc_aligned_alloc(size_t alignment, size_t size);
size_t memloc_malloc_usable_size(const void* ptr);

/* Version string, e.g. "0.1.0". */
const char* memloc_version(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMLOC_MEMLOC_H */
