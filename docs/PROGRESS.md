# Progress log

Every number in this file was measured on the development machine unless stated
otherwise:
Intel Core i7-13700H (14 cores / 20 threads), 7.6 GiB RAM visible to WSL2,
Ubuntu 26.04 on WSL2 (kernel 6.18.33.2-microsoft-standard-WSL2), GCC 15.2, Clang 21.1.8.

---

## Phase 0: Skeleton and design

**Built**
- CMake project: `src/` (static `memloc_core`), `include/memloc/memloc.h`,
  `tests/` (GoogleTest v1.17.0 via FetchContent), `bench/`, `tools/`, `docs/`.
- `-Wall -Wextra -Werror` on every target via the `memloc_warnings` interface
  library. The core is built with `-fno-exceptions -fno-rtti`.
- CI (`.github/workflows/ci.yml`): build + ctest with gcc and clang on
  ubuntu-24.04, a clang-format check, and a clang-tidy check. The tools are
  pinned via pip (D-6).
- `docs/DECISIONS.md` D-1…D-7: size classes, metadata lookup, carving
  granularity, the three-tier threshold (which changes the brief's two-tier
  plan), thread-cache design, tooling, and environment.
- README skeleton with the architecture diagram and non-goals.

**Results**
- gcc 15.2 and clang 21.1.8 both build cleanly; 1/1 smoke test passes.
- `tools/check_format.sh`: OK. `tools/run_tidy.sh`: OK.
- CI has not run yet (no remote). The workflow uses the same scripts as the
  local runs above.

**How to run**
```sh
make test                       # gcc, RelWithDebInfo, ~/build/memloc
make format tidy
```

**Interview questions**
1. *Why does free() not need a size argument in your design?*
   The chunk registry and the 4 MiB alignment turn any pointer into its chunk
   header with a mask. The chunk header maps the 64 KiB unit to a span, and
   the span records the size class.
2. *What is the worst-case internal fragmentation of your size classes?*
   16 B steps up to 128 B (worst case 15/16 for 1-byte requests; in bytes, at
   most 15). Above that, 4 classes per doubling, so at most ~20% (129 → 160).
3. *Why is the unit 64 KiB rather than the 4 KiB OS page?*
   A 4 MiB chunk then has exactly 64 units, so a single `uint64_t` bitmap
   tracks occupancy and coalescing is automatic. 64 KiB is also a multiple of
   every arm64 page size (4/16/64 KiB).
4. *Why pin clang-format in CI?*
   Its output changes between major versions; an unpinned runner would fail on
   code that is correctly formatted locally.
5. *What did you change from the original architecture and why?*
   I added a medium tier (32 KiB–1 MiB from the page heap). A direct mmap per
   allocation costs two syscalls plus a page fault per page, per malloc/free
   cycle.

---

## Phase 1: Single-threaded core allocator

**Built**
- `size_class.h`: 40 classes (D-1). `size_to_class` is arithmetic; the
  class table and its invariants (16-byte multiples, no gaps, span tail
  waste ≤ 1/16) are proved by `static_assert(classes_are_valid())`.
- `chunk.h`: 4 MiB aligned chunks of 64 × 64 KiB units, span descriptors in
  the chunk header, and a 1-bit-per-4 MiB chunk registry (D-2, D-3).
- `page_heap.{h,cpp}`: first-fit run search over a 64-bit occupancy mask per
  chunk; freeing a span clears bits, so coalescing is implicit.
- `central.{h,cpp}`: per-class list of spans with free objects, lazy carving
  (D-8), and empty spans returned to the page heap with one kept warm (D-12).
- `huge.{h,cpp}`: direct mmap with a 64-byte header before the pointer,
  `mremap` for realloc (D-11), and arbitrary alignment by padding (D-10).
- `malloc.cpp`: `memloc_malloc/free/calloc/realloc/posix_memalign/aligned_alloc/malloc_usable_size`
  with glibc semantics (D-9).

**Results**
- 27/27 tests pass with gcc 15.2, clang 21.1.8 and clang + UBSan
  (`halt_on_error=1`). Format and clang-tidy are clean.
  - `SizeClass.*`: exhaustive over every size 0…32768.
  - `Api.*`, `Calloc.*`, `Realloc.*`, `Aligned.*`: boundary sizes around each
    tier, `malloc(0)`, overflow, in-place realloc rules, every power-of-two
    alignment from 8 B to 4 MiB, and EINVAL cases.
  - `Stress.RandomAllocFreeReallocKeepsContents`: 300 000 random ops on 4 096
    slots across all tiers, with a per-byte pattern verified before every free
    and realloc. `Stress.ManySmallObjectsAreDistinct`: 200 000 live 16 B objects.
- Measurement supporting D-4 (throwaway microbenchmark, 200 000 iterations,
  3 runs): a 64 KiB alloc + touch every page + free cycle costs **41–58 ns**
  through the medium tier versus **20.9–21.7 µs** with direct `mmap`/`munmap`
  (≈ 400× slower), because each fresh mapping pays two syscalls and 16 page faults.

**How to run**
```sh
make test
ctest --test-dir ~/build/memloc -R Stress --output-on-failure
```

**Interview questions**
1. *How does `free` distinguish a small object from a huge block?*
   It tests the chunk-registry bit for `p >> 22`. Only chunks set bits, so a
   clear bit means `p` came from a direct mmap, and its header is at `p - 64`.
2. *Why is carving lazy?*
   Writing a free list through a new 256 KiB span would fault in all 64 pages
   up front. With a bump pointer, only pages that objects actually occupy
   become resident.
3. *How does `posix_memalign(4096, 100)` work without an offset header?*
   Span bases are 64 KiB-aligned, so any class whose size is a multiple of
   4096 yields page-aligned objects. The 4096 class is chosen, and `free`
   needs no special case.
4. *Why does `calloc` skip `memset` for huge blocks?*
   They are fresh anonymous mappings, which the kernel guarantees are zero.
   Zeroing them anyway would fault in every page immediately, turning a lazy
   allocation into an eager one.
5. *What is the cost of `realloc` growing a 100 MiB block?*
   `mremap(MREMAP_MAYMOVE)`: the kernel re-points page-table entries, so no
   bytes are copied. The header lives in the mapping and moves with it.
