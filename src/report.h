// report.h - stack capture and JSON-lines crash reports for debug mode.
// (D-24, D-25, D-26)
//
// One report is one line of JSON, built in a static buffer and written with a
// single write(2) - no stdio, no malloc, because the heap is suspect by the
// time anything is reported:
//
//   {"type":"double-free","pid":4242,"tid":4242,"program":"/path/prog",
//    "address":"0x7f..","size":64,"detail":"...",
//    "stack":[{"pc":"0x55..","module":"/path/prog","offset":"0x11d3",
//              "symbol":"main","symbol_offset":"0x2a"}, ...],
//    "alloc_stack":[...],"alloc_tid":4242,"free_stack":[...],"free_tid":4242,
//    "heap":{"mapped":4194304,"live_blocks":3,...}}
//
// "offset" is the address to look up in "module" (what addr2line wants):
// the pc itself for a faulting instruction, pc - 1 for return addresses, so
// that it lands inside the call instruction. tools/symbolize.py adds file and
// line offline.
#pragma once

#include <cstddef>
#include <cstdint>

namespace memloc {

inline constexpr unsigned kStoredFrames = 8;   // per allocation and per free
inline constexpr unsigned kReportFrames = 32;  // for the current stack

// Fixed-capacity text buffer; appends past the end are dropped.
class Buf {
 public:
  Buf(char* data, std::size_t cap) : data_(data), cap_(cap) {}
  void add(const char* s, std::size_t n);
  void add(const char* s);
  void dec(std::uint64_t v);
  void sdec(std::int64_t v);
  void hex(std::uintptr_t v);  // 0x-prefixed, no padding
  const char* data() const { return data_; }
  std::size_t size() const { return len_; }
  void clear() { len_ = 0; }

 private:
  char* data_;
  std::size_t cap_;
  std::size_t len_ = 0;
};

struct Stack {
  void* const* pcs = nullptr;
  unsigned depth = 0;
  bool exact_top = false;  // pcs[0] is a faulting pc, not a return address
};

struct HeapSnapshot {
  std::size_t mapped;
  std::size_t live_blocks;
  std::size_t live_bytes;
  std::size_t quarantine_blocks;
  std::size_t quarantine_bytes;
};

struct Report {
  const char* type = nullptr;
  const void* address = nullptr;  // omitted when nullptr
  std::size_t size = 0;
  bool has_size = false;
  std::int64_t offset = 0;
  bool has_offset = false;
  const char* detail = "";
  Stack stack, alloc_stack, free_stack;
  std::uint32_t alloc_tid = 0, free_tid = 0;
  struct Extra {
    const char* key;
    std::uint64_t value;
  } extra[5] = {};
  unsigned num_extra = 0;
  bool human = true;  // also print a one-line summary on stderr
};

// Reads MEMLOC_REPORT, finds MemLoc's own code range and warms up
// backtrace(). Called once by the debug-mode constructor.
void report_init();

// Captures the caller's stack, without MemLoc's own frames.
unsigned capture_stack(void** out, unsigned max);

// Heap counters (lock-free reads, safe in a signal handler). debug.cpp
HeapSnapshot heap_snapshot();

// Serialises and writes r. emit_report locks and uses a shared static
// buffer; emit_report_unlocked uses the caller's (the signal handler's).
void emit_report(const Report& r);
void emit_report_unlocked(const Report& r, Buf& buf);

// True if module is a system library or MemLoc itself (leak filtering).
bool is_system_module(const char* module);
bool is_loader_module(const char* module);

// dladdr() wrapper: module path (the main program's real path rather than
// argv[0]) and nearest exported symbol for a code address.
struct FrameInfo {
  const char* module;
  const char* symbol;
  std::uintptr_t offset;         // address within the module
  std::uintptr_t symbol_offset;  // address - symbol start
};
bool describe_pc(std::uintptr_t addr, FrameInfo* out);

std::uint32_t current_tid();

// SIGSEGV/SIGBUS reporting, debug mode only. segv.cpp
void install_segv_handler();

}  // namespace memloc
