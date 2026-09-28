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

/* Exported from libmemloc.so, which is otherwise built -fvisibility=hidden. */
#define MEMLOC_API __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

MEMLOC_API void* memloc_malloc(size_t size);
MEMLOC_API void memloc_free(void* ptr);
MEMLOC_API void* memloc_calloc(size_t count, size_t size);
MEMLOC_API void* memloc_realloc(void* ptr, size_t size);
MEMLOC_API int memloc_posix_memalign(void** out, size_t alignment, size_t size);
MEMLOC_API void* memloc_aligned_alloc(size_t alignment, size_t size);
MEMLOC_API size_t memloc_malloc_usable_size(const void* ptr);

/* Heap statistics (D-28). All sizes are in bytes. */
struct memloc_stats {
  size_t mapped;          /* currently mapped from the OS (chunks + huge blocks) */
  size_t peak_mapped;     /* high-water mark of `mapped` */
  size_t allocated;       /* handed out: in use by the program or in a thread cache */
  size_t small_allocated; /* ...of which small blocks (class size x blocks out) */
  size_t medium_allocated;
  size_t huge_allocated;     /* mapping lengths of live huge blocks */
  size_t huge_count;         /* number of live huge blocks */
  size_t small_span_bytes;   /* chunk space owned by small-class spans */
  size_t unused_chunk_bytes; /* chunk space not owned by any span */
  size_t chunks;             /* 4 MiB chunks mapped */
};

/* Fills *out. Takes every heap lock for the duration of a walk over all
 * chunks, so it is meant for occasional use, not for hot loops. */
MEMLOC_API void memloc_stats(struct memloc_stats* out);

/* Writes a human-readable report (totals and one row per size class) to
 * the file descriptor. This is what MEMLOC_STATS=1 prints at exit (D-29). */
MEMLOC_API void memloc_stats_print(int fd);

/* Version string, e.g. "0.1.0". */
MEMLOC_API const char* memloc_version(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMLOC_MEMLOC_H */
