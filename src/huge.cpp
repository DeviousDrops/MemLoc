#include "huge.h"

#include <atomic>
#include <cerrno>

#include "config.h"
#include "os.h"
#include "panic.h"

namespace memloc {

namespace {

// Live huge blocks and the bytes their mappings cover (D-28).
constinit std::atomic<std::size_t> g_huge_count{0};
constinit std::atomic<std::size_t> g_huge_bytes{0};

// Offset of the user pointer from the mapping base for a given alignment.
std::size_t user_offset(std::size_t align) {
  return align > sizeof(HugeHeader) ? align : sizeof(HugeHeader);
}

bool checked_map_len(std::size_t offset, std::size_t n, std::size_t* out) {
  const std::size_t page = os_page_size();
  if (n > SIZE_MAX - offset - page) return false;
  *out = align_up(offset + n, page);
  return true;
}

// The header of a block we handed out; anything else is a caller bug (most
// often a pointer that never came from this allocator).
HugeHeader* checked_header(const void* p, const char* what) {
  HugeHeader* h = huge_header(p);
  if (h->magic != kHugeMagic) panic(what, p);
  return h;
}

}  // namespace

void* huge_alloc(std::size_t n, std::size_t align) {
  const std::size_t offset = user_offset(align);
  std::size_t len = 0;
  if (!checked_map_len(offset, n, &len)) {
    errno = ENOMEM;
    return nullptr;
  }
  // Up to page alignment, mmap's own alignment plus a fixed offset is enough;
  // beyond that we need an aligned mapping.
  char* base =
      static_cast<char*>(align <= os_page_size() ? os_map(len) : os_map_aligned(len, align));
  if (base == nullptr) {
    errno = ENOMEM;
    return nullptr;
  }
  char* p = base + offset;
  HugeHeader* h = huge_header(p);
  h->magic = kHugeMagic;
  h->base = base;
  h->map_len = len;
  g_huge_count.fetch_add(1, std::memory_order_relaxed);
  g_huge_bytes.fetch_add(len, std::memory_order_relaxed);
  return p;
}

void huge_free(void* p) {
  HugeHeader* h = checked_header(p, "free(): invalid pointer");
  h->magic = 0;
  g_huge_count.fetch_sub(1, std::memory_order_relaxed);
  g_huge_bytes.fetch_sub(h->map_len, std::memory_order_relaxed);
  os_unmap(h->base, h->map_len);
}

std::size_t huge_live_count() { return g_huge_count.load(std::memory_order_relaxed); }
std::size_t huge_live_bytes() { return g_huge_bytes.load(std::memory_order_relaxed); }

std::size_t huge_usable_size(const void* p) {
  const HugeHeader* h = checked_header(p, "malloc_usable_size(): invalid pointer");
  return static_cast<std::size_t>(h->base + h->map_len - static_cast<const char*>(p));
}

void* huge_realloc(void* p, std::size_t n) {
  HugeHeader* h = checked_header(p, "realloc(): invalid pointer");
  const auto offset = static_cast<std::size_t>(static_cast<char*>(p) - h->base);
  std::size_t len = 0;
  if (!checked_map_len(offset, n, &len)) {
    errno = ENOMEM;
    return nullptr;
  }
  if (len == h->map_len) return p;
  // The header travels with the mapping, so after a move it is still at p - 64.
  char* base = static_cast<char*>(os_remap(h->base, h->map_len, len));
  if (base == nullptr) {
    errno = ENOMEM;
    return nullptr;
  }
  g_huge_bytes.fetch_add(len, std::memory_order_relaxed);
  g_huge_bytes.fetch_sub(huge_header(base + offset)->map_len, std::memory_order_relaxed);
  char* q = base + offset;
  huge_header(q)->base = base;
  huge_header(q)->map_len = len;
  return q;
}

}  // namespace memloc
