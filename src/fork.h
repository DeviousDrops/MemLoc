// fork.h - keeps the heap usable in the child of a multi-threaded fork().
// (D-20)
//
//   prepare (parent, before fork):  lock debug, central[0..39], page heap
//   parent  (after fork):           unlock page heap, central[39..0], debug
//   child   (after fork):           same as parent
//
// Taking every lock first means no other thread can be half-way through a
// heap update when the address space is copied, so the child's single thread
// inherits a consistent heap with all locks free. The order matches D-13.
#pragma once

namespace memloc {

// Registers the pthread_atfork handlers; idempotent and thread-safe.
void install_fork_handlers();

}  // namespace memloc
