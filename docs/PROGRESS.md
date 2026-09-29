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

## Phase 4: Debug mode

**Built**
- Fast/slow split (D-22): `memloc_malloc`, `memloc_free` and `memloc_calloc`
  try the thread cache inline and otherwise call a `noinline` slow path. Only
  the slow path reads `g_debug`. Debug mode disables the thread caches, so
  every call reaches the slow path.
- `src/debug.cpp`, the checking allocator (D-23, D-25):
  - a 192-byte header, then 16 canary bytes on each side of the user block;
  - live and freed magics XORed with the user address;
  - `0xBE` fill on malloc and `0xDD` poison on free;
  - a FIFO quarantine of 4096 blocks / 64 MiB;
  - interior-pointer lookup for invalid frees;
  - leaks at exit, grouped by allocation stack.
- `src/report.cpp` (D-24): one JSON line per finding, written with no
  allocation. It holds the program, pid, tid, detail, and `stack`,
  `alloc_stack` and `free_stack` with their thread ids. It also holds a heap
  snapshot. Frames are module + offset (from `dladdr`), with MemLoc's own
  frames trimmed. There is a recursion guard, and `backtrace()` is warmed up
  at start-up.
- `src/segv.cpp` (D-26): a SIGSEGV/SIGBUS handler on a 64 KiB alternate
  stack. It reports the fault PC and describes the faulting address: null,
  outside the heap, a chunk header, class/block/byte of a small span, or a
  free span.
- `tools/symbolize.py` runs addr2line once per module and has a `--text`
  view. `tools/run_bugs.py` checks each bug program against
  `tests/bugs/expected.json`.
- `tests/bugs/` (D-27) holds 11 programs, one bug each: off-by-one,
  underflow, double free, use-after-free, stale pointer after realloc,
  interior free, free of a non-heap pointer, leaks from 2 sites, null
  dereference, overflow through `new[]`, and a cross-thread use-after-free.
  Each has a ground-truth root cause and fix, which Phase 6 evaluates against.

**Results**
- 52/52 ctest with gcc 15.2 and clang 21.1.8: 34 unit, 7 preload, 11 bugs.
  The preload count includes the whole preload suite re-run under
  `MEMLOC_DEBUG=1` (minus one usable-size test). TSan 34/34. clang-format
  and clang-tidy are clean.
- `make bugs`: **11/11 caught**. Each record has the expected type and
  signal, and names the buggy function in the expected stack, with file:line
  after symbolization. `thread_uaf` reports alloc thread 612 and free
  thread 613, both different from the main thread.
- Zero cost when off: `objdump -d libmemloc.so` shows 0 references to
  `g_debug` in `memloc_malloc` (41 instructions), `memloc_free` (62) and
  `memloc_calloc` (50).
- Throughput with debug off, before → after the split (M malloc/free pairs/s,
  3 runs, working set N; noisy WSL2 box):

  | threads | N | before | after |
  |---|---|---|---|
  | 1 | 100 | 97–224 | 185–205 |
  | 1 | 1000 | 108–152 | 144–177 |
  | 8 | 100 | 604–942 | 850–947 |
  | 8 | 1000 | 267–281 | 268–291 |

  In other words, no regression. glibc on the same probe: 26–40 (1 thread)
  and 138–190 (8 threads).
- Cost of debug mode (wall time, 2 runs, normal → debug):
  - `tools/demo` sqlite3 workload: 1875/1504 ms → 7327/7060 ms (about 4×),
    with byte-identical output;
  - python3 workload: 1322/1226 ms → 2870/2814 ms (about 2.3×);
  - python3 with `PYTHONMALLOC=malloc`, where every object goes through debug
    malloc: about 52–56 s.
- sqlite3 under debug reports 14 leaked blocks (4841 bytes) from 6 sites,
  plus 2 system blocks. These are sqlite's process-lifetime allocations, and
  there are no false errors. python3 reports 70 leak sites, which are
  interpreter-lifetime allocations it deliberately never frees.

**How to run**
```sh
make bugs                                   # 11 programs, reports kept in ~/build/memloc/bug-reports
MEMLOC_DEBUG=1 MEMLOC_REPORT=r.jsonl LD_PRELOAD=~/build/memloc/src/libmemloc.so ./prog
tools/symbolize.py --text r.jsonl
ctest --test-dir ~/build/memloc -L bugs --output-on-failure
```

**Interview questions**
1. *How can a runtime debug flag cost nothing?* Don't test it where it
   would cost. The inline fast path is a thread-cache pop or push and never
   reads the flag. Only the out-of-line slow path does, and it is already
   paying for a lock. Debug mode keeps the caches empty, so it forces every
   call onto that path. objdump confirms 0 references.
2. *How do you detect use-after-free without a page per allocation?* Poison
   the block on free and keep it out of circulation in a quarantine. When it
   leaves the quarantine (or at exit), check that the poison is intact. This
   catches writes after free, but not reads. Catching reads needs guard pages
   (Electric Fence) or shadow memory (ASan).
3. *Why does the report store module + offset rather than function and
   line?* DWARF parsing allocates and is slow, and the process may be
   crashing. Module + offset is stable across ASLR, so an offline tool
   (addr2line) can resolve it later. That is the same split used by crash
   reporters such as Breakpad.
4. *What's unsafe about your SIGSEGV handler and why is it acceptable?*
   `backtrace()` and `dladdr()` are not async-signal-safe. `backtrace()` is
   warmed up at start-up, so it doesn't dlopen libgcc_s inside the handler.
   The process is dying anyway, and a report with a stack is worth the risk
   of a deadlock in a rare corner case.
5. *How do you avoid reporting libc's allocations as leaks?* Group leaks by
   allocation stack. A group counts as "system" (reported only as a count)
   if its innermost frame is in the dynamic loader, if any frame is
   `_IO_file_doallocate` (the stdio buffer), or if no frame is outside
   system libraries.

## Phase 5: Statistics and benchmarks

**Built**
- `memloc_stats()` / `memloc_stats_print(fd)` and `MEMLOC_STATS=1` (D-28,
  D-29). A snapshot takes every central lock, then the page-heap lock (the
  fork order), and walks all chunks. Only mapping (os.cpp) and huge blocks
  (huge.cpp) keep atomic counters, and the fast path counts nothing.
  `os_map_aligned` now accounts only the window it keeps, which removed a
  4 MiB inflation of the peak.
- Four benchmarks in `bench/` (D-31): larson (16-256 B, arrays passed
  between threads), random (log-uniform 8 B-32 KiB, window of 256), rss
  (fill 256 MiB, free half, refill 128 MiB of 1-4 KiB) and realloc growth
  (64 buffers to 1 MiB in 1-64 B steps). They call plain `malloc` and are
  built with `-fno-builtin`.
- `tools/bench.py` picks the allocator with `LD_PRELOAD` (D-30). Runs are
  the outer loop so drift hits all three alike. It writes
  `bench/results/results.csv` and `machine.json`. `tools/plot_bench.py`
  draws `docs/img/*.png` and prints median tables (D-32). The graphs use a
  colour-vision-checked palette and label lines directly.
- Two optimisations found by the benchmarks:
  - D-33: a 65-entry size-class lookup table up to 1 KiB. Mixed 16-256 B
    went from 71 to 110 Mops/s (a branch mispredict at the 128 B boundary).
  - D-34: realloc resizes medium spans in place (`PageHeap::resize_span`).
- `bench_smoke_*` ctests run every benchmark once under MemLoc.

**Results** (i7-13700H, 20 logical CPUs, WSL2 6.18, glibc 2.43, mimalloc
3.2.8, MemLoc 3832b0c, median of 5 runs; full tables in the README)
- Larson M ops/s, MemLoc / mimalloc / glibc: 1 thread 104 / 134 / 77,
  4 threads 260 / 244 / 93, 16 threads 359 / 376 / 187.
- Random M ops/s: 1 thread 71 / 53 / 17, 4 threads 231 / 166 / 55,
  16 threads 383 / 382 / 103.
- RSS / live after refill: 1.32 / 1.59 / 1.02 (1 thread) and
  1.35 / 1.63 / 1.02 (4 threads).
- realloc growth: 53 / 65 / 75 ns per call, and peak RSS 170 / 131 / 71 MiB.
  Before D-34 the full run gave 61 ns for MemLoc.
- The first full run (before D-34) differed from this one by up to 18 %
  at the same thread count (larson at 2 threads: 150 vs 177). That is the
  run-to-run noise of this machine, and why medians and bands are shown.
- ctest: gcc 63/63 (41 unit, 7 preload, 11 bugs, 4 bench smoke), clang
  59/59 (built without benchmarks), TSan 41/41. clang-format and clang-tidy
  are clean.

**How to run**
```sh
make bench                      # build, 5 runs x 3 allocators (~2 min), plots + tables
make plots                      # redraw docs/img from bench/results
tools/bench.py --runs 1 --threads 1 4 --out /tmp/quick
MEMLOC_STATS=1 LD_PRELOAD=~/build/memloc/src/libmemloc.so python3 -c 'print(1)'
```

**Interview questions**
1. *Why walk the heap for statistics instead of keeping counters?* Counters
   on the fast path cost every allocation, and per-thread counters still
   need summing. Statistics are rare, so the snapshot pays instead: it
   locks the central lists and page heap and walks the chunks. Blocks
   sitting in thread caches count as allocated, as the header documents.
2. *How did you find the 1-thread larson slowdown?* By bisecting the
   workload, not the code. Single sizes were fast, and so was a mix below
   128 B. A mix across 128 B was slow at any working-set size. That points
   to a data-dependent branch, and the class computation was the only one.
   A lookup table made it +55 %.
3. *Your peak RSS numbers were identical for every allocator. Why?*
   `getrusage(RUSAGE_SELF).ru_maxrss` survives `exec`, so it reported the
   Python driver's peak. Reading `VmHWM` from `/proc/self/status` gives
   the benchmark's own high-water mark.
4. *How do you make an allocator comparison fair?* Use the same binary for
   all three via `LD_PRELOAD`, with `-fno-builtin` so the compiler can't
   drop malloc/free pairs. Interleave allocators within each run, repeat 5
   times, report the median with the min-max band, and publish the machine
   details.
5. *Why is your RSS worse than glibc after refill?* A small span holds one
   size class. The freed half leaves most spans partly used, and those
   holes can only take blocks of the same class. glibc coalesces free
   chunks of any size and splits them for the 1-4 KiB refill. Fixing this
   would need the span to be empty (it is then returned to the page heap)
   or a way to return pages to the OS (`madvise`), which is a non-goal.

## Phase 6: AI triage tool

**Built**
- `tools/triage.py` (321 lines, D-35 to D-40):
  - Groups debug reports by a signature: the report type plus the innermost
    3 user functions of each stack.
  - Builds one prompt per group from the report and ±12 lines of source per
    user frame. Recorded paths that don't exist are re-rooted onto the checkout.
  - Asks `openai/gpt-oss-120b` on Groq for type, culprit, root cause, fix and
    confidence as strict schema-constrained JSON. It calls Groq through
    `urllib`, with no SDK, and retries after `retry-after` on HTTP 429.
  - The first version used Claude (`claude-opus-5`). D-40 switched to Groq
    because the Claude API is paid and Groq has a free tier.
- `eval` scores the 11 `tests/bugs` programs against `expected.json`.
  Type and culprit are exact matches. Root cause and fix are judged by a
  second model call, or by keyword recall offline. Every call is appended to
  a JSONL trace.
- A rule-based mock is the offline default and doubles as the baseline.
- `tests/triage/test_triage.py`: 14 stdlib unittest tests on 11 checked-in
  reports (real `make bugs` output with paths rewritten). A fake HTTP
  transport checks the request shape, rate-limit retries, truncated output,
  invalid fields, HTTP errors and a rejected key. One test runs the real
  `urllib` client against a local stub server. The tests are registered as
  ctest `triage_unit`.
- `make triage-eval` (`LLM=mock` for offline).

**Results**
- Mock baseline on the 11 bugs: type 11/11, culprit 11/11, root cause 2/11
  (18 %), fix 0/11. 23 trace lines (12 triage calls, since the leak has two
  sites, plus 11 judge calls).
- Groq, `openai/gpt-oss-120b` (`make triage-eval`, 2026-09-29): type 11/11,
  culprit 5/11 (exact match), root cause 11/11, fix 11/11. It made 23 calls
  with no errors, 18,125 prompt and 7,008 completion tokens, in 124 s. 16
  calls were retried after HTTP 429 (the free tier allows 8K tokens per
  minute).
- All 6 culprit misses blame the other function of a two-function bug, and
  the expected function still appears in the root cause:
  - underflow: `bucket_of` (returns -1) instead of `build_histogram` (indexes with it)
  - double_free: `handle_request` (frees early) instead of `release_request` (frees again)
  - use_after_free: `main` (keeps the pointer) instead of `evict` (frees it)
  - realloc_stale: `main` (keeps the pointer) instead of `append` (reallocs)
  - leak: `main` (never frees) instead of `make_cache` (allocates)
  - thread_uaf: `submit_job` (uses the job) instead of `worker_main` (frees it)

  The metric stays strict rather than being loosened after seeing results.
  Every judge verdict was checked by hand against `expected.json`, because
  the judge is the same model.
- ctest: gcc 64/64 (41 unit, 7 preload, 11 bugs, 4 bench smoke, 1 triage).
  The triage tests (all 14) pass on Python 3.14 (WSL) and 3.13 (Windows).

**How to run**
```sh
make triage-eval LLM=mock                         # offline baseline
GROQ_API_KEY=... make triage-eval                 # real eval, trace in ~/build/memloc/triage-trace.jsonl
tools/triage.py report --llm mock tests/triage/reports/*.jsonl
ctest --test-dir ~/build/memloc -L triage
```

**Interview questions**
1. *Why group before calling the model?* Cost and noise. One bug produces a
   report per run, thread or leaked block. Hashing function names, not
   addresses, makes the group stable across ASLR and rebuilds. The cost then
   scales with distinct bugs, not with reports.
2. *How do you test code that calls an LLM without calling it?* Split the
   deterministic parts (grouping, prompt, validation, scoring) from the call.
   Then test the call's contract with a fake client that records the request
   and returns canned replies, including rate limits, truncation and bad
   JSON. A stub HTTP server checks that the real client puts the same
   fields on the wire.
3. *The model got every root cause right but only 5/11 culprits. Why?*
   The misses were all two-function ownership bugs, where the model blamed
   the side that keeps or uses the pointer and the ground truth blames the
   side that frees it. Both are defensible. The lesson is that the label
   was ambiguous, not that the model was wrong. I'd change the ground truth
   to accept a set of culprits, and apply that to the next run, not this one.
   *Why is the baseline so strong on culprit?* The debug allocator already
   records the right stack for each bug type: the allocation stack for
   overflows, the free stack for use-after-free. Blaming the top user frame
   there is correct for all 11 programs. The model's value is the *why* and
   the fix, which the baseline gets 2/11 and 0/11 on.
4. *What would you watch in production?* Schema-valid but wrong answers.
   Track the rate of `confidence: low`, disagreement between repeated runs,
   and judge verdicts sampled against human labels. The trace holds every
   prompt and answer for that audit.
5. *What are the limits of an 11-case eval?* One case is 9 points, and the
   programs are small and written to be diagnosable. It is a regression
   check for the tool, not evidence that it works on real crashes. That
   needs a corpus of historical bugs with known fixes.
