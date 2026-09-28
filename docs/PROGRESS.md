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

## Phase 2: Thread caches and concurrency

**Built**
- `thread_cache.{h,cpp}`: a per-thread, per-class free list in
  `constinit thread_local` storage (initial-exec TLS, zero-initialised, so it
  works without constructor code). Push and pop need no locks or atomics.
  A miss fetches one batch, and a push past 2 × batch releases one (D-5, D-16).
  A `pthread_key` destructor flushes the cache at thread exit, and the cache
  then turns *Dead*. Frees and mallocs from later TSD destructors go straight
  to the central lists (D-14).
- `central.{h,cpp}`: one `std::mutex` per class, each list `alignas(64)` on
  its own cache line (static_assert). The page heap has its own mutex, and the
  lock order is central → page heap (D-13). The **transfer cache** (D-17) moves
  whole batches between threads in O(1) under the lock.
- Memory-ordering argument for the relaxed chunk-registry atomics written
  down in D-15.
- CI: a `tsan` job (clang, `-fsanitize=thread`, unit tests with
  `halt_on_error=1`).

**Results**
- 34/34 tests pass under gcc 15.2, clang 21.1.8 and **ThreadSanitizer (no
  warnings)**. Format and clang-tidy are clean.
  - `Threads.ConcurrentRandomStress`: 8 threads × 100 000 ops across all
    tiers, with address stamps verified before each free.
  - `Threads.ProducerConsumerCrossThreadFrees`: 4 producers and 4 consumers
    exchange 200 000 blocks, so every free happens on a thread other than the
    one that allocated the block.
  - `Threads.TransferCacheHandsBatchesBetweenThreads`,
    `Threads.ExitingThreadFlushesItsCache` (a new thread gets back exactly the
    same 12 objects, so nothing is stranded in a dead cache),
    `Threads.AllocationAfterCacheTeardownStillWorks` (malloc from a TSD
    destructor that runs after ours), `Threads.CacheRespectsHighWaterMark`,
    `Threads.ManyShortLivedThreads`.
- TSan negative control: a copy with the lock removed from
  `CentralList::release` reports `WARNING: ThreadSanitizer: data race` on the
  producer/consumer test, so the TSan job does detect races here.
- Throwaway probe (each thread allocates, then frees, N × 64 B blocks; M
  alloc+free pairs/s, 2 runs each). Proper benchmarks are in Phase 5.

  | design                         | 1 thr, N=1000 | 8 thr, N=100 | 8 thr, N=1000 |
  |--------------------------------|--------------:|-------------:|--------------:|
  | glibc 2.43                     | 24–35         | 136–165      | 128–159       |
  | D-5/D-16 only (batch ≤ 32)     | –             | 24–26        | 23            |
  | + transfer cache               | 119–133       | 107–111      | 74–76         |
  | + batch ≤ 128 (final, D-17)    | 101–128       | 553–853      | 265–268       |

  The first version was 4–6× *slower* than glibc at 8 threads. The reason was
  that each batch walked 32 remote cache lines while holding the class mutex.

**How to run**
```sh
make test
make tsan        # clang + ThreadSanitizer build in ~/build/memloc-tsan
ctest --test-dir ~/build/memloc -R Threads --output-on-failure
```

**Interview questions**
1. *Why doesn't a cross-thread free need to go back to the allocating thread?*
   Blocks of one class are interchangeable. The freeing thread keeps the block
   in its own cache, and batches carry surplus back to the central list,
   where it can reach other threads. Ownership would matter only if spans
   were per-thread (as in mimalloc), and here they are not.
2. *How do you flush a `thread_local` cache at thread exit if it has no
   destructor?* A `pthread_key` with a destructor, set on first use. It runs
   during thread teardown. After it the cache is marked Dead, because other
   TSD destructors may still call malloc/free and there would be nobody left
   to flush a revived cache.
3. *Why initial-exec TLS?* The general-dynamic model may call
   `__tls_get_addr`, which can allocate on first access in a dlopen'ed
   library. Inside malloc that means recursion. Initial-exec is a fixed
   offset from the thread pointer: one instruction, no calls.
4. *Why `alignas(64)` on each central list?* Without it, two classes' mutexes
   could share a cache line. Threads hammering different classes would then
   bounce that line between cores (false sharing) even though they never
   contend logically.
5. *What did TSan catch, and how do you know it works on your code?* It
   caught nothing in the final build. The evidence that it is watching is the
   negative control: removing one lock guard produced a data-race report
   pointing at `CentralList::release`.

## Phase 3: libmemloc.so, an LD_PRELOAD drop-in

**Built**
- `src/preload.cpp` → `libmemloc.so`: the full glibc malloc family as thin
  wrappers (D-18). Built `-fvisibility=hidden` and linked with
  `-Bsymbolic-functions -z defs --as-needed -nostdlib++`. `ldd` shows
  **only libc** for both the gcc and the clang build, and `nm -D` lists
  exactly 19 exported symbols: the 11 glibc names plus the memloc_* API.
- Bootstrap without dlsym (D-19): all state is `constinit`. `std::mutex` was
  replaced by a 20-line `Mutex` over `pthread_mutex_t`, because its throwing
  `lock()` pulled in libstdc++. A function-local static in `os_page_size`
  needed `__cxa_guard_acquire`, so it became a relaxed atomic.
- fork safety (D-20): `pthread_atfork` handlers take all 40 central locks
  and then the page-heap lock, in lock order.
- `panic()`: a signal-safe message and `abort()` when `free` / `realloc` /
  `malloc_usable_size` receives a pointer MemLoc never returned. Checked by
  hand: `free(stack_buf + 128)` prints
  `memloc: free(): invalid pointer (0x00007ffc40c551e0)` and exits 134.
- `tests/preload/preload_test.cpp` (5 tests, run with `LD_PRELOAD`, not
  linked with the core) and `tools/preload_demo.sh` with `tools/demo/`
  workloads (D-21). Both are registered with ctest under the label `preload`.
  CI installs sqlite3.

**Results**
- 40/40 tests pass with gcc 15.2 and clang 21.1.8. The 34 unit tests also pass
  under TSan (the preload tests are excluded there by design). Format and
  clang-tidy are clean.
- `Preload.EveryFamilyMemberResolvesIntoMemloc`: `dladdr` confirms all 11
  functions resolve into libmemloc.so. The other tests cover glibc-only
  entry points (memalign's rounding, valloc/pvalloc page alignment,
  reallocarray overflow), C++ `new`/`delete` including `alignas(256)`, and
  blocks allocated inside glibc (`strdup`, `asprintf`, `realpath`,
  `open_memstream`) being freed by MemLoc.
- `Preload.ForkWhileOtherThreadsAllocate`: 200 forks while 4 threads churn
  every size class. **Negative control:** with the atfork registration
  disabled, the *first* child deadlocked on all 3 runs (killed by its
  5-second alarm). With it, 200/200 children succeed.
- Real programs, `tools/preload_demo.sh`, 3 runs. The `LD_DEBUG=bindings`
  check passes for both programs, and output is byte-identical with and
  without MemLoc.

  | workload                                   | glibc (ms)  | MemLoc (ms) |
  |--------------------------------------------|------------:|------------:|
  | sqlite3 3.46.1: 200k rows, 2 indexes, update/delete, VACUUM | 1958–2022 | 1956–2108 |
  | python 3.14.4: dicts/json/re/zlib, 8 threads, fork pool     | 1352–1594 | 1496–1516 |
  | same with `PYTHONMALLOC=malloc` (no pymalloc)               | 1564–1853 | 1466–1584 |

  These are end-to-end wall times on WSL2 with a noisy spread, so the honest
  reading is "on par". The one consistent gap is the raw-malloc Python run,
  where every object goes through the allocator: MemLoc wins all 3 runs.
  Controlled benchmarks are in Phase 5.
- Found on the way: python3 has a *canonical PLT entry* for `malloc`, so
  libc and libexpat bind `malloc` to the python3 executable, which in turn
  binds it to libmemloc.so. The demo's check accepts bindings to the program
  and rejects any binding into another shared library.

**How to run**
```sh
make test                                   # includes the preload label
ctest --test-dir ~/build/memloc -L preload --output-on-failure
tools/preload_demo.sh                       # defaults to ~/build/memloc/src/libmemloc.so
LD_PRELOAD=~/build/memloc/src/libmemloc.so python3
```

**Interview questions**
1. *How does LD_PRELOAD make glibc's own internal allocations use your
   malloc?* glibc calls `malloc` through its PLT, not directly, precisely so
   it can be interposed. The dynamic linker resolves every reference in
   search order (executable, then preloaded objects, then libc), and the
   first definition wins.
2. *Why doesn't your preload library call dlsym?* It has nothing to look up:
   it replaces malloc instead of wrapping it. That also avoids the classic
   recursion (dlsym → calloc → your malloc → dlsym …) that wrapper-style
   interposers need a static bootstrap buffer for.
3. *Why did you replace std::mutex?* `std::mutex::lock()` throws
   `std::system_error` on failure. That made `libmemloc.so` reference
   `std::__throw_system_error`, and therefore need libstdc++, inside every C
   program. The allocator cannot handle a lock failure anyway, so a
   pthread wrapper with a constexpr initializer is more honest.
4. *What exactly goes wrong on fork without atfork handlers?* The child
   gets a copy of memory in which some other thread's mutex is locked, but
   that thread does not exist in the child. Its first malloc that needs that
   lock blocks forever. The test reproduced this on the first fork.
5. *Why does `-z defs` matter for a shared library?* Without it, an
   unresolved symbol (like the `__cxa_guard_acquire` found here) links
   fine and only fails at load time, possibly inside a customer's process.
   With it, the build fails.
