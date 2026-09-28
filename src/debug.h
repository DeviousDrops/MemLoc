// debug.h - MEMLOC_DEBUG=1: a checking allocator layered on the normal one.
// (D-22 .. D-26)
//
// Every debug block is one ordinary (uncached) MemLoc block:
//
//   raw                                         user = raw + 208      user + n
//   v                                           v                     v
//   +------------------------------+------------+---------------------+------------+
//   | DebugHeader (192 B)          | canary 16B | user bytes (n)      | canary 16B |
//   | magic ^ user, size, raw,     | 0xCA x 16  | 0xBE on malloc,     | 0xCA x 16  |
//   | live-list links, tids,       |            | 0xDD once freed     |            |
//   | alloc stack[8], free stack[8]|            |                     |            |
//   +------------------------------+------------+---------------------+------------+
//
//   live blocks:  doubly linked list        -> leak report at exit
//   freed blocks: FIFO quarantine           -> poison checked on eviction and
//                 (4096 blocks / 64 MiB)       at exit (use-after-free writes)
//
// A problem produces a JSON-lines report (report.h) and abort(). Debug mode is
// switched on by a constructor reading MEMLOC_DEBUG; until then, and for the
// allocations debug mode makes itself, blocks are ordinary and are freed as
// such.
#pragma once

#include <atomic>
#include <cstddef>

namespace memloc {

extern constinit std::atomic<bool> g_debug;

// Read only on slow paths (D-22).
inline bool debug_enabled() { return g_debug.load(std::memory_order_relaxed); }

void* debug_alloc(std::size_t n, std::size_t align, bool zero);
void debug_free(void* p);
void* debug_realloc(void* p, std::size_t n);
std::size_t debug_usable_size(const void* p);

// fork() support: the debug lock is a leaf lock (nothing else is taken while
// it is held), so the atfork handlers may take it first.
void debug_lock();
void debug_unlock();

// The debug allocator's backing store, implemented in malloc.cpp: ordinary
// blocks that never pass through a thread cache.
void* uncached_alloc(std::size_t n);
void uncached_free(void* p);
std::size_t uncached_usable_size(const void* p);

}  // namespace memloc
