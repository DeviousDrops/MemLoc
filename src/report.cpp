#include "report.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>  // std::scoped_lock

#include "mutex.h"

namespace memloc {

// --- Buf -------------------------------------------------------------------

void Buf::add(const char* s, std::size_t n) {
  n = std::min(n, cap_ - len_);
  std::copy_n(s, n, data_ + len_);
  len_ += n;
}

void Buf::add(const char* s) { add(s, std::strlen(s)); }

void Buf::dec(std::uint64_t v) {
  char tmp[20];
  int i = sizeof(tmp);
  do {
    tmp[--i] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);
  add(tmp + i, sizeof(tmp) - static_cast<std::size_t>(i));
}

void Buf::sdec(std::int64_t v) {
  if (v < 0) {
    add("-", 1);
    dec(~static_cast<std::uint64_t>(v) + 1);
  } else {
    dec(static_cast<std::uint64_t>(v));
  }
}

void Buf::hex(std::uintptr_t v) {
  char tmp[18];
  int i = sizeof(tmp);
  do {
    tmp[--i] = "0123456789abcdef"[v & 15];
    v >>= 4;
  } while (v != 0);
  tmp[--i] = 'x';
  tmp[--i] = '0';
  add(tmp + i, sizeof(tmp) - static_cast<std::size_t>(i));
}

namespace {

// --- process-wide report state (written once by report_init) --------------

int g_report_fd = STDERR_FILENO;
char g_report_path[256];
char g_program[512];
std::uintptr_t g_self_lo, g_self_hi;  // MemLoc's code, when it is a separate DSO

Mutex g_report_mu;
char g_report_buf[64 * 1024];  // guarded by g_report_mu

void copy_cstr(char* dst, std::size_t cap, const char* src) {
  const std::size_t n = std::min(std::strlen(src), cap - 1);
  std::copy_n(src, n, dst);
  dst[n] = '\0';
}

struct SelfRange {
  std::uintptr_t probe;
  std::uintptr_t lo, hi;
};

// Finds the loaded object containing probe and records its executable
// segment. Skipped for the main program: when MemLoc is linked statically,
// its frames cannot be told apart from the program's by address range.
int find_self(dl_phdr_info* info, std::size_t, void* arg) {
  auto* r = static_cast<SelfRange*>(arg);
  for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr) & ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) continue;
    const std::uintptr_t lo = info->dlpi_addr + ph.p_vaddr;
    const std::uintptr_t hi = lo + ph.p_memsz;
    if (r->probe < lo || r->probe >= hi) continue;
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
      r->lo = lo;
      r->hi = hi;
    }
    return 1;
  }
  return 0;
}

// JSON writer on top of Buf: tracks whether a comma is due.
class Json {
 public:
  explicit Json(Buf& b) : b_(b) {}
  void open(char c) {
    sep();
    b_.add(&c, 1);
    first_ = true;
  }
  void close(char c) {
    b_.add(&c, 1);
    first_ = false;
  }
  void key(const char* k) {
    sep();
    quoted(k);
    b_.add(":", 1);
    first_ = true;  // the value follows without a comma
  }
  void str(const char* s) {
    sep();
    if (s == nullptr) {
      b_.add("null");
      return;
    }
    quoted(s);
  }
  void num(std::uint64_t v) {
    sep();
    b_.dec(v);
  }
  void snum(std::int64_t v) {
    sep();
    b_.sdec(v);
  }
  void hex(std::uintptr_t v) {
    sep();
    b_.add("\"", 1);
    b_.hex(v);
    b_.add("\"", 1);
  }

 private:
  void sep() {
    if (!first_) b_.add(",", 1);
    first_ = false;
  }
  void quoted(const char* s) {
    b_.add("\"", 1);
    for (std::size_t i = 0; s[i] != '\0' && i < 1024; ++i) {
      const auto c = static_cast<unsigned char>(s[i]);
      if (c == '"' || c == '\\') {
        const char esc[2] = {'\\', static_cast<char>(c)};
        b_.add(esc, 2);
      } else if (c < 0x20) {
        const char esc[6] = {
            '\\', 'u', '0', '0', "0123456789abcdef"[c >> 4], "0123456789abcdef"[c & 15]};
        b_.add(esc, 6);
      } else {
        b_.add(reinterpret_cast<const char*>(&c), 1);
      }
    }
    b_.add("\"", 1);
  }
  Buf& b_;
  bool first_ = true;
};

void emit_stack(Json& j, const char* key, const Stack& s) {
  j.key(key);
  j.open('[');
  for (unsigned i = 0; i < s.depth; ++i) {
    const auto pc = reinterpret_cast<std::uintptr_t>(s.pcs[i]);
    const std::uintptr_t lookup = (i == 0 && s.exact_top) ? pc : pc - 1;
    FrameInfo f{};
    const bool known = describe_pc(lookup, &f);
    j.open('{');
    j.key("pc");
    j.hex(pc);
    j.key("module");
    j.str(known ? f.module : nullptr);
    j.key("offset");
    j.hex(known ? f.offset : lookup);
    j.key("symbol");
    j.str(known ? f.symbol : nullptr);
    j.key("symbol_offset");
    if (known && f.symbol != nullptr) {
      j.hex(f.symbol_offset);
    } else {
      j.str(nullptr);
    }
    j.close('}');
  }
  j.close(']');
}

void write_all(int fd, const char* p, std::size_t n) {
  while (n > 0) {
    const ssize_t w = write(fd, p, n);
    if (w <= 0) return;
    p += w;
    n -= static_cast<std::size_t>(w);
  }
}

bool starts_with(const char* s, const char* prefix) {
  return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

}  // namespace

std::uint32_t current_tid() { return static_cast<std::uint32_t>(syscall(SYS_gettid)); }

void report_init() {
  const ssize_t n = readlink("/proc/self/exe", g_program, sizeof(g_program) - 1);
  g_program[n > 0 ? n : 0] = '\0';

  if (const char* path = std::getenv("MEMLOC_REPORT"); path != nullptr && path[0] != '\0') {
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
      g_report_fd = fd;
      copy_cstr(g_report_path, sizeof(g_report_path), path);
    }
  }

  SelfRange self{.probe = reinterpret_cast<std::uintptr_t>(&report_init), .lo = 0, .hi = 0};
  dl_iterate_phdr(find_self, &self);
  g_self_lo = self.lo;
  g_self_hi = self.hi;

  // The first backtrace() dlopens libgcc_s and allocates; do it now, not in
  // the middle of a report (D-24). The caller holds the recursion guard.
  void* warm[4];
  backtrace(warm, 4);
}

unsigned capture_stack(void** out, unsigned max) {
  void* raw[kReportFrames + 16];
  const int n = backtrace(raw, static_cast<int>(std::min<unsigned>(max + 16, std::size(raw))));
  int i = 0;
  while (i < n && reinterpret_cast<std::uintptr_t>(raw[i]) >= g_self_lo &&
         reinterpret_cast<std::uintptr_t>(raw[i]) < g_self_hi) {
    ++i;
  }
  unsigned depth = 0;
  for (; i < n && depth < max; ++i) out[depth++] = raw[i];
  return depth;
}

bool describe_pc(std::uintptr_t addr, FrameInfo* out) {
  Dl_info info{};
  link_map* map = nullptr;
  if (dladdr1(reinterpret_cast<void*>(addr), &info, reinterpret_cast<void**>(&map),
              RTLD_DL_LINKMAP) == 0 ||
      map == nullptr) {
    return false;
  }
  // The main program's link_map has an empty name and dladdr reports
  // argv[0]; the path from /proc/self/exe is what a symboliser needs.
  out->module = map->l_name[0] == '\0' ? g_program : info.dli_fname;
  out->offset = addr - map->l_addr;
  out->symbol = info.dli_sname;
  out->symbol_offset =
      info.dli_saddr != nullptr ? addr - reinterpret_cast<std::uintptr_t>(info.dli_saddr) : 0;
  return true;
}

bool is_loader_module(const char* module) {
  return module != nullptr && std::strstr(module, "/ld-linux") != nullptr;
}

bool is_system_module(const char* module) {
  if (module == nullptr) return true;
  if (std::strstr(module, "libmemloc") != nullptr) return true;
  return starts_with(module, "/lib/") || starts_with(module, "/lib64/") ||
         starts_with(module, "/usr/lib/") || starts_with(module, "/usr/lib64/");
}

void emit_report_unlocked(const Report& r, Buf& buf) {
  buf.clear();
  Json j(buf);
  j.open('{');
  j.key("type");
  j.str(r.type);
  j.key("pid");
  j.num(static_cast<std::uint64_t>(getpid()));
  j.key("tid");
  j.num(current_tid());
  j.key("program");
  j.str(g_program);
  if (r.address != nullptr) {
    j.key("address");
    j.hex(reinterpret_cast<std::uintptr_t>(r.address));
  }
  if (r.has_size) {
    j.key("size");
    j.num(r.size);
  }
  if (r.has_offset) {
    j.key("offset");
    j.snum(r.offset);
  }
  j.key("detail");
  j.str(r.detail);
  for (unsigned i = 0; i < r.num_extra; ++i) {
    j.key(r.extra[i].key);
    j.num(r.extra[i].value);
  }
  if (r.stack.depth > 0) emit_stack(j, "stack", r.stack);
  if (r.alloc_stack.depth > 0) {
    emit_stack(j, "alloc_stack", r.alloc_stack);
    j.key("alloc_tid");
    j.num(r.alloc_tid);
  }
  if (r.free_stack.depth > 0) {
    emit_stack(j, "free_stack", r.free_stack);
    j.key("free_tid");
    j.num(r.free_tid);
  }
  const HeapSnapshot h = heap_snapshot();
  j.key("heap");
  j.open('{');
  j.key("mapped");
  j.num(h.mapped);
  j.key("live_blocks");
  j.num(h.live_blocks);
  j.key("live_bytes");
  j.num(h.live_bytes);
  j.key("quarantine_blocks");
  j.num(h.quarantine_blocks);
  j.key("quarantine_bytes");
  j.num(h.quarantine_bytes);
  j.close('}');
  j.close('}');
  buf.add("\n", 1);
  write_all(g_report_fd, buf.data(), buf.size());

  if (!r.human) return;
  // "memloc: double-free on 0x55..: <detail>"
  buf.clear();
  buf.add("memloc: ");
  buf.add(r.type);
  if (r.address != nullptr) {
    buf.add(" on ");
    buf.hex(reinterpret_cast<std::uintptr_t>(r.address));
  }
  buf.add(": ");
  buf.add(r.detail);
  buf.add("\n");
  if (g_report_fd != STDERR_FILENO) {
    buf.add("memloc: report appended to ");
    buf.add(g_report_path);
    buf.add("\n");
  }
  write_all(STDERR_FILENO, buf.data(), buf.size());
}

void emit_report(const Report& r) {
  std::scoped_lock lock(g_report_mu);
  Buf buf(g_report_buf, sizeof(g_report_buf));
  emit_report_unlocked(r, buf);
}

}  // namespace memloc
