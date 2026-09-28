// os.h - the only place MemLoc talks to the kernel for memory.
//
// All mappings are private and anonymous: MAP_PRIVATE | MAP_ANONYMOUS gives
// zero-filled, lazily-faulted memory that is not backed by any file. Every
// function returns nullptr on failure (with errno set by the kernel) and
// never allocates.
#pragma once

#include <cstddef>

namespace memloc {

std::size_t os_page_size();

// Maps len bytes (a multiple of the page size) of zeroed read/write memory.
void* os_map(std::size_t len);

// Maps len bytes at an address that is a multiple of align (a power of two
// >= page size). It over-maps by align, then unmaps the unaligned head and tail.
void* os_map_aligned(std::size_t len, std::size_t align);

void os_unmap(void* p, std::size_t len);

// Resizes a mapping; the kernel may move it (MREMAP_MAYMOVE). D-11
void* os_remap(void* p, std::size_t old_len, std::size_t new_len);

}  // namespace memloc
