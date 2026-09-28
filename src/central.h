// central.h - per-size-class central free lists. (D-5, D-8, D-12, D-17)
//
// A central list has two levels. The *transfer cache* holds whole batches
// exactly as a thread cache released them, chained through each batch's
// head object. Below it, the list links the *spans* of its class that still
// have something to give (a recycled object or an uncarved tail); those
// objects stay on their own span's free list:
//
//   CentralList[class 2 = 48 B]
//     batches --> [o->o->..->o] --> [o->o->..->o] --> nullptr   (<= 1 MiB)
//     spans   --> [Span @unit 5] <--> [Span @unit 9] <--> nullptr
//                   free_list: o->o->o     free_list: nullptr
//                   carve..limit: empty    carve..limit: 1203 objects left
//
// fetch(n)          a full batch is popped from the transfer cache in O(1);
//                   otherwise objects come from spans: recycled ones first,
//                   then carved ones, with a new span from the page heap when
//                   the list is empty.
// release_batch()   pushes a full batch onto the transfer cache in O(1); when
//                   that is full, falls back to release().
// release()         pushes each object back onto *its* span (found via
//                   span_of); a span whose last object comes back returns to
//                   the page heap (unless it is the class's only span, D-12).
//
// The transfer cache keeps the common case - one thread frees a batch,
// another allocates one - from touching every object while holding the lock.
//
// Locking (D-13): each list has its own mutex, and each list sits on its own
// cache line so that two classes never false-share. fetch() and release() may
// take the page-heap lock while holding this one (order: central -> page heap).
#pragma once

#include <mutex>

#include "chunk.h"
#include "size_class.h"

namespace memloc {

class alignas(64) CentralList {
 public:
  constexpr CentralList() = default;

  // Moves up to n objects of class cls into a nullptr-terminated list at
  // *head. Returns how many were moved; 0 means out of memory.
  unsigned fetch(unsigned cls, unsigned n, void** head);

  // Returns a nullptr-terminated list of objects (all of this list's class).
  void release(void* head);

  // Same, for a list of exactly batch_size(cls) objects.
  void release_batch(unsigned cls, void* head);

  // Test hook: number of batches in the transfer cache.
  unsigned cached_batches();

  // Bytes of whole batches one class may park in its transfer cache. D-17
  static constexpr std::size_t kTransferBytes = std::size_t{1} << 20;

 private:
  void release_to_spans(void* head);  // requires mu_
  void link(Span* s);
  void unlink(Span* s);

  std::mutex mu_;
  void* batches_ = nullptr;   // guarded by mu_
  unsigned num_batches_ = 0;  // guarded by mu_
  Span* spans_ = nullptr;     // guarded by mu_
};

static_assert(alignof(CentralList) == 64 && sizeof(CentralList) % 64 == 0,
              "each central list must own whole cache lines");

extern CentralList g_central[kNumClasses];

// Objects on free lists store the next pointer in their first word.
inline void*& next_of(void* obj) { return *static_cast<void**>(obj); }

// The head object of a batch in the transfer cache also stores the next
// batch in its second word (every class is at least 16 bytes).
inline void*& next_batch_of(void* obj) { return static_cast<void**>(obj)[1]; }
static_assert(kMinAlign >= 2 * sizeof(void*));

}  // namespace memloc
