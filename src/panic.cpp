#include "panic.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace memloc {

namespace {

// A fixed buffer and an append that truncates: no stdio, no malloc.
struct Line {
  char buf[160];
  std::size_t len = 0;

  void add(const char* s, std::size_t n) {
    n = std::min(n, sizeof(buf) - len);
    std::copy_n(s, n, buf + len);
    len += n;
  }
  void add(const char* s) { add(s, std::strlen(s)); }
};

}  // namespace

void panic(const char* what, const void* ptr) {
  // "memloc: <what> (0x<ptr>)\n"
  char hex[16];
  const auto v = reinterpret_cast<std::uintptr_t>(ptr);
  for (int i = 0; i < 16; ++i) hex[i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
  Line line;
  line.add("memloc: ");
  line.add(what);
  line.add(" (0x");
  line.add(hex, sizeof(hex));
  line.add(")\n");
  [[maybe_unused]] const ssize_t written = write(STDERR_FILENO, line.buf, line.len);
  std::abort();
}

}  // namespace memloc
