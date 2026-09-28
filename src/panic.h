// panic.h - fatal-error exit for heap misuse the allocator cannot survive
// (e.g. freeing a pointer it never handed out). Async-signal-safe: writes one
// line to stderr with write(2) and aborts, without touching the heap.
#pragma once

namespace memloc {

[[noreturn]] void panic(const char* what, const void* ptr);

}  // namespace memloc
