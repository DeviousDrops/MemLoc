// thread_cache.h - per-thread, per-class free lists: the lock-free fast path.
// (D-5, D-14, D-16)
//
//   thread T1 (static TLS)                thread T2 (static TLS)
//   ThreadCache                           ThreadCache
//    lists[0]  16 B: o->o->o   3/64        lists[0]  16 B: o         1/64
//    lists[5]  96 B: o->o      2/64        lists[9] 192 B: o->o->o   3/64
//        |                ^                     |               ^
//        | miss: fetch    | over 2*batch:       |               |
//        v one batch      | release one batch   v               |
//   +--------------------------------------------------------------------+
//   | CentralList[c]  (one mutex per class, alignas(64))                  |
//   +--------------------------------------------------------------------+
//
// malloc: pop lists[c]; free: push lists[c]. No locks and no atomics.
// A block freed by a thread other than the one that allocated it just joins
// the freeing thread's list: blocks are fungible, so ownership is never
// tracked. Batches carry blocks back to the central list and to other threads.
#pragma once

#include <cstddef>

namespace memloc {

void* tc_alloc(unsigned cls);         // nullptr (errno = ENOMEM) when out of memory
void tc_free(unsigned cls, void* p);  // p must belong to class cls

// Test hook: number of objects of class cls cached by the calling thread.
std::size_t tc_cached_count(unsigned cls);

}  // namespace memloc
