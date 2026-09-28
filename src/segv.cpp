// segv.cpp - SIGSEGV/SIGBUS reports in debug mode. (D-26)
//
//   fault --> kernel --> handler (on the alternate stack)
//                          1. classify si_addr: null page / MemLoc chunk / elsewhere
//                          2. stack = [faulting pc] + frames below the signal frame
//                          3. one "segv" JSON line via write(2)
//                          4. restore SIG_DFL and return
//             <------------ the instruction faults again and the process dies
//                           with the usual signal, exit status and core dump
//
// Everything the handler touches is preallocated (alternate stack, output
// buffer) and it takes no lock; heap counters and span descriptors are read
// racily, so the snapshot can be torn. backtrace() and dladdr() are not on
// POSIX's async-signal-safe list; after report_init's warm-up they neither
// allocate nor take MemLoc's locks, which is the pragmatic choice most crash
// reporters make.
#include <execinfo.h>
#include <ucontext.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <iterator>

#include "chunk.h"
#include "report.h"
#include "size_class.h"

namespace memloc {
namespace {

alignas(16) char g_alt_stack[64 * 1024];
char g_segv_buf[32 * 1024];
char g_detail[256];
std::atomic<bool> g_in_handler{false};

std::uintptr_t fault_pc(const void* context) {
  const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
  return static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
  return static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
#else
#error "unsupported architecture"
#endif
}

void describe_address(Buf& d, std::uintptr_t a) {
  if (a < 4096) {
    d.add("null pointer dereference (address ");
    d.hex(a);
    d.add(")");
    return;
  }
  const auto* p = reinterpret_cast<const void*>(a);
  if (!in_chunk(p)) {
    d.add("address outside the MemLoc heap (wild or dangling pointer, or a stack overflow)");
    return;
  }
  const Span* s = span_of(p);
  const std::uintptr_t off = a - reinterpret_cast<std::uintptr_t>(span_base(s));
  if (unit_of(p) == 0) {
    d.add("address inside a MemLoc chunk header (heap metadata)");
  } else if (s->kind == SpanKind::kSmall) {
    const std::size_t size = class_size(s->size_class);
    d.add("address inside a MemLoc small span: class ");
    d.dec(s->size_class);
    d.add(" (");
    d.dec(size);
    d.add("-byte blocks), block ");
    d.dec(off / size);
    d.add(", byte ");
    d.dec(off % size);
  } else if (s->kind == SpanKind::kMedium) {
    d.add("address inside a MemLoc medium block of ");
    d.dec(span_bytes(s));
    d.add(" bytes, at offset ");
    d.dec(off);
  } else {
    d.add("address inside free MemLoc memory (a block freed long ago)");
  }
}

void on_fault(int sig, siginfo_t* info, void* context) {
  struct sigaction dfl{};
  dfl.sa_handler = SIG_DFL;
  if (g_in_handler.exchange(true)) {  // a second fault: just die
    sigaction(sig, &dfl, nullptr);
    return;
  }
  const auto addr = reinterpret_cast<std::uintptr_t>(info->si_addr);
  const std::uintptr_t pc = fault_pc(context);

  // backtrace() walks through the signal frame; keep what lies below the
  // faulting pc, or everything after our own frames if the pc is not found.
  void* raw[kReportFrames + 8];
  const int n = backtrace(raw, static_cast<int>(std::size(raw)));
  int start = 0;
  for (int i = 0; i < n; ++i) {
    if (reinterpret_cast<std::uintptr_t>(raw[i]) == pc) {
      start = i + 1;
      break;
    }
  }
  void* pcs[kReportFrames];
  unsigned depth = 0;
  pcs[depth++] = reinterpret_cast<void*>(pc);
  for (int i = start; i < n && depth < kReportFrames; ++i) pcs[depth++] = raw[i];

  Buf d(g_detail, sizeof(g_detail) - 1);
  d.add(sig == SIGBUS ? "SIGBUS: " : "SIGSEGV: ");
  describe_address(d, addr);
  g_detail[d.size()] = '\0';

  Report r;
  r.type = "segv";
  r.address = info->si_addr;
  r.detail = g_detail;
  r.stack = {.pcs = pcs, .depth = depth, .exact_top = true};
  r.extra[0] = {.key = "signal", .value = static_cast<std::uint64_t>(sig)};
  r.num_extra = 1;
  Buf out(g_segv_buf, sizeof(g_segv_buf));
  emit_report_unlocked(r, out);

  sigaction(sig, &dfl, nullptr);  // returning re-executes the faulting instruction
}

}  // namespace

void install_segv_handler() {
  // The alternate stack belongs to the installing (main) thread; faults on
  // other threads run on their own stacks.
  stack_t ss{};
  ss.ss_sp = g_alt_stack;
  ss.ss_size = sizeof(g_alt_stack);
  sigaltstack(&ss, nullptr);
  struct sigaction sa{};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
}

}  // namespace memloc
