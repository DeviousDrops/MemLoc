// central.h - per-size-class central free lists. (D-5, D-8, D-12)
//
// A central list does not store objects directly. It links the *spans* of
// its class that still have something to give (a recycled object or an
// uncarved tail). Objects stay on their own span's free list:
//
//   CentralList[class 2 = 48 B]
//     spans --> [Span @unit 5] <--> [Span @unit 9] <--> nullptr
//                 free_list: o->o->o     free_list: nullptr
//                 carve..limit: empty    carve..limit: 1203 objects left
//
// fetch(n)    pops up to n objects: recycled ones first, then carves new
//             ones; asks the page heap for a new span when the list is empty.
// release()   pushes each object back onto *its* span (found via span_of);
//             a span whose last object comes back returns to the page heap
//             (unless it is the class's only span, D-12).
//
// Objects move in batches, so the per-object work here is a few pointer
// writes; the expensive part (the lock, Phase 2) is paid once per batch.
#pragma once

#include "chunk.h"
#include "size_class.h"

namespace memloc {

class CentralList {
 public:
  constexpr CentralList() = default;

  // Moves up to n objects of class cls into a nullptr-terminated list at
  // *head. Returns how many were moved; 0 means out of memory.
  unsigned fetch(unsigned cls, unsigned n, void** head);

  // Returns a nullptr-terminated list of objects (all of this list's class).
  void release(void* head);

 private:
  void link(Span* s);
  void unlink(Span* s);

  Span* spans_ = nullptr;
};

extern CentralList g_central[kNumClasses];

// Objects on free lists store the next pointer in their first word.
inline void*& next_of(void* obj) { return *static_cast<void**>(obj); }

}  // namespace memloc
