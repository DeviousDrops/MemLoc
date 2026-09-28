/*
 * memloc.h - public C API of the MemLoc allocator.
 *
 * The memloc_* functions are the allocator proper. The shared library
 * (libmemloc.so) additionally exports the standard malloc family as aliases of
 * these functions, so it can replace glibc malloc via LD_PRELOAD. Tests and
 * benchmarks call the memloc_* names directly.
 */
#ifndef MEMLOC_MEMLOC_H
#define MEMLOC_MEMLOC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Version string, e.g. "0.1.0". */
const char* memloc_version(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMLOC_MEMLOC_H */
