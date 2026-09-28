// huge.h - blocks above 1 MiB (or with alignment above 64 KiB) get their own
// mmap. (D-4, D-10, D-11)
//
//   base (page aligned)                 p (returned to the user)
//   v                                   v
//   +-----------+-----------------------+-------------------------+-------+
//   | [padding] | HugeHeader (64 bytes) | user data (n bytes)     | slack |
//   +-----------+-----------------------+-------------------------+-------+
//   |<--------------------------- map_len (whole pages) ----------------->|
//
// The header always sits directly before p, so free(p) finds it at p - 64.
// Padding only exists when the caller asked for alignment above 64 bytes.
// Huge blocks are never inside a chunk, which is how free() tells them apart
// (see the chunk registry in chunk.h).
#pragma once

#include <cstddef>
#include <cstdint>

namespace memloc {

struct alignas(64) HugeHeader {
  std::uint64_t magic;
  char* base;           // start of the mapping
  std::size_t map_len;  // length of the mapping
};
static_assert(sizeof(HugeHeader) == 64, "header size defines the minimum user-pointer offset");

inline constexpr std::uint64_t kHugeMagic = 0x4d454d4c4f434855ULL;  // "MEMLOCHU"

inline HugeHeader* huge_header(const void* p) {
  return reinterpret_cast<HugeHeader*>(const_cast<char*>(static_cast<const char*>(p)) -
                                       sizeof(HugeHeader));
}

// align must be a power of two; nullptr (errno = ENOMEM) on failure.
void* huge_alloc(std::size_t n, std::size_t align);
void huge_free(void* p);
std::size_t huge_usable_size(const void* p);
// Grows or shrinks with mremap; returns nullptr (p still valid) on failure.
void* huge_realloc(void* p, std::size_t n);

// Live huge blocks and the total length of their mappings (D-28).
std::size_t huge_live_count();
std::size_t huge_live_bytes();

}  // namespace memloc
