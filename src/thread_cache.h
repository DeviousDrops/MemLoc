// thread_cache.h - per-thread, per-class free lists: the lock-free fast path.
// (D-5, D-14, D-16, D-22)
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
//
// The fast paths (tc_pop, tc_push) are inline and fail rather than doing any
// slow work, so callers can put everything else - refills, activation, and
// the debug-mode check (D-22) - behind one out-of-line call.
#pragma once

#include <cstddef>
#include <cstdint>

#include "central.h"
#include "size_class.h"

namespace memloc {

enum class TcState : std::uint8_t { kUninit, kActive, kDead };

struct FreeList {
  void* head;
  std::uint32_t count;
};

struct ThreadCache {
  FreeList lists[kNumClasses];
  TcState state;
};

// Zero-initialised, trivially destructible and in static TLS: usable from
// the very first malloc of a thread without running any code. D-14
extern constinit thread_local ThreadCache t_cache __attribute__((tls_model("initial-exec")));

void tc_release_batch(unsigned cls, FreeList& fl);  // out of line: takes a lock

// Pops a cached object of class cls, or returns nullptr if the list is empty.
inline void* tc_pop(unsigned cls) {
  FreeList& fl = t_cache.lists[cls];
  void* p = fl.head;
  if (p == nullptr) [[unlikely]] {
    return nullptr;
  }
  fl.head = next_of(p);
  --fl.count;
  return p;
}

// Caches p (of class cls). Returns false, doing nothing, unless this thread's
// cache is active.
inline bool tc_push(unsigned cls, void* p) {
  ThreadCache& tc = t_cache;
  if (tc.state != TcState::kActive) [[unlikely]] {
    return false;
  }
  FreeList& fl = tc.lists[cls];
  next_of(p) = fl.head;
  fl.head = p;
  if (++fl.count > 2 * batch_size(cls)) [[unlikely]] {
    tc_release_batch(cls, fl);
  }
  return true;
}

// Slow paths. Both activate the cache on a thread's first use.
void* tc_refill(unsigned cls);  // nullptr (errno = ENOMEM) when out of memory
void tc_free_slow(unsigned cls, void* p);

// Returns this thread's cached objects to the central lists and stops caching
// on this thread (used when debug mode starts). D-22
void tc_disable();

// Test hook: number of objects of class cls cached by the calling thread.
std::size_t tc_cached_count(unsigned cls);

}  // namespace memloc
