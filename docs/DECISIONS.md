# MemLoc design decisions

Each entry records a real fork in the road: what forced the decision, what the
alternatives were, what was chosen, and what that choice costs. Entries are
written *before* the code that implements them and are never rewritten
afterwards; if a decision is revisited, a new entry supersedes the old one.

Glossary used throughout:

| Term       | Meaning in this codebase                                              |
|------------|-----------------------------------------------------------------------|
| **chunk**  | a 4 MiB, 4 MiB-aligned `mmap` region; header lives in its first unit  |
| **unit**   | a 64 KiB slice of a chunk; the page heap's allocation granule         |
| **span**   | a run of 1..63 contiguous units owned by one size class or one medium allocation |
| **small**  | request ≤ 32 KiB, served from a size class                            |
| **medium** | 32 KiB < request ≤ 1 MiB, served as a whole span from the page heap   |
| **huge**   | request > 1 MiB (or alignment > 64 KiB), served by a direct `mmap`    |

---

## D-1: Size-class scheme            (Phase 0)
Context:      Small requests must be rounded to a fixed set of sizes so that
              freed blocks are interchangeable. More classes means less
              internal waste but more partially-used spans and caches.
Options:      A) powers of two only (8 classes to 32 KiB, up to 50% waste)
              B) 16-byte steps to 128 B, then 4 geometric steps per power of
                 two up to 32 KiB (40 classes, ≤ 20% waste above 128 B)
              C) jemalloc/tcmalloc-style hand-tuned tables (~60-90 classes)
Choice:       B. Classes are 16, 32, …, 128, then 160, 192, 224, 256, 320, …,
              28672, 32768. Size→class is pure arithmetic (one `clz`), and
              class→size is a `constexpr` table.
Why:          Every class is a multiple of 16, which gives the 16-byte
              alignment guarantee without extra work. Worst-case internal waste
              above 128 B is 1/5 (e.g. 129 B → 160 B). The mapping can be
              tested exhaustively for all 32 768 sizes.
Trade-off:    Hand-tuned tables (C) fit real programs' size histograms better
              (e.g. dedicated 24/40/56-byte classes). With B, a 24-byte object
              uses 32 bytes (33% waste). C wins once you have allocation
              profiles from a real fleet.
Interview Q:  "Why not powers of two?" — 2^k classes waste up to 50% on
              average-sized objects. Four steps per doubling caps waste near
              20% while keeping the class count (and per-class caches) small.

## D-2: Finding a pointer's metadata on free            (Phase 0)
Context:      `free(p)` receives only a pointer. We must find its size class
              (small), its span (medium), or its mapping length (huge), in O(1)
              and without a per-object header on small objects.
Options:      A) per-object header before every block (8–16 B per object;
                 doubles the size of 16-byte objects)
              B) global radix-tree page map, like tcmalloc (2-3 dependent
                 loads, a large reserved table, and a general page heap)
              C) aligned chunks: every chunk is 4 MiB-aligned, so
                 `p & ~(4 MiB-1)` is the chunk header. Plus a 1-bit-per-4 MiB
                 "chunk registry" to tell chunk pointers from huge mappings.
Choice:       C. `free(p)`: test the registry bit for `p >> 22`. If set,
              chunk = `p & ~mask`, unit = `(p - chunk) >> 16`, and
              span = `chunk->spans[chunk->owner[unit]]`. If not set, `p` is a
              huge block whose 64-byte header sits directly before `p`.
Why:          No per-object overhead. Constant time with at most 3 dependent
              loads, all to memory we own. Huge blocks keep an ordinary header,
              so they can live at any address. That makes arbitrary alignment
              and `mremap`-based realloc possible (D-4). The registry is 8 MiB of
              zero-filled BSS, and only the pages covering addresses we have
              used ever become resident (usually one or two 4 KiB pages).
Trade-off:    Chunks must be 4 MiB-aligned, so creating one costs an
              over-sized `mmap` plus trimming (rare: once per 4 MiB). Assumes a
              48-bit user address space (true for x86-64 and default arm64
              Linux; 52-bit VA would need a bigger registry). A radix page map
              (B) wins when you need interior-pointer lookup or per-page
              metadata for every page (e.g. returning memory to the OS page by
              page).
Interview Q:  "How does free know the size?" — The pointer's high bits find the
              4 MiB chunk header; the unit index finds the span, and the span
              stores the size class. Small objects carry zero bytes of header.

## D-3: Page-carving granularity            (Phase 0)
Context:      Chunks must be split into spans for size classes and medium
              allocations, and freed spans must be reusable by any class.
Options:      A) 4 KiB pages with first-fit free lists and explicit coalescing
                 (tcmalloc's classic page heap)
              B) fixed 64 KiB units, with a 64-bit occupancy bitmap per 4 MiB chunk
              C) one fixed span size for everything (a simple slab allocator)
Choice:       B. Unit 0 of each chunk holds the header (bitmap, per-unit owner
              table and 64 span descriptors). Units 1–63 are allocatable. A span
              is a run of contiguous free bits, found with a bit scan.
              Small-class spans are 1 unit (classes ≤ 8 KiB) or 4 units (larger
              classes), which keeps tail waste ≤ 6.25% (checked by `static_assert`).
Why:          Freeing a span clears its bits, so coalescing with free
              neighbours happens automatically with no extra code. All span
              metadata sits in the chunk header, so object memory is 100%
              payload, and a span base is always 64 KiB-aligned (useful for
              `posix_memalign`). The whole page heap is about 100 lines.
Trade-off:    Medium requests round up to 64 KiB, so a 33 KiB request
              reserves 64 KiB of address space. RSS grows only for pages that
              are touched, but after reuse that tail can be resident. Unit 0
              wastes 60 KiB of *address space* per chunk (about 1.5%). Option A
              wins on fine-grained medium sizes and when returning pages to
              the OS.
Interview Q:  "How do you coalesce free spans?" — I don't need to: freeing
              clears bits in a 64-bit mask, so adjacent free runs are
              automatically one run. Finding k free units is a bit-scan over
              one word.

## D-4: Large-allocation threshold (and a change to the brief)            (Phase 0)
Context:      The brief proposes two tiers: ≤ 32 KiB uses size classes, and
              everything larger uses a direct mmap. But every direct mmap
              costs an mmap + munmap syscall pair, *and* a page fault on the
              first touch of every page, on every allocation. glibc avoids this
              for blocks under 128 KiB (and raises that threshold dynamically,
              up to 32 MiB) for exactly this reason.
Options:      A) two tiers as briefed (> 32 KiB → mmap)
              B) three tiers: small ≤ 32 KiB; medium ≤ 1 MiB as a span of
                 64 KiB units from the page heap; huge > 1 MiB → direct mmap
              C) raise the size-class limit to 256 KiB+ (like tcmalloc)
Choice:       B. The medium tier reuses the page heap from D-3 with no new
              data structures: a medium block is simply a span whose kind is
              `Medium`. Huge blocks use `mmap` with a 64-byte header before the
              user pointer, and `mremap` to grow on `realloc`.
Why:          Medium blocks recycle already-faulted memory under one lock
              instead of two syscalls and N page faults. The cost is about 30
              extra lines because the bitmap page heap already exists. Huge
              blocks go straight to the kernel, which is the right place for
              them: freeing returns memory to the OS immediately, and `mremap`
              lets `realloc` grow without copying.
Trade-off:    Medium allocations take a single global page-heap lock, and
              medium memory is never returned to the OS (a non-goal). A
              workload that allocates 64 KiB–1 MiB blocks on many threads
              will contend on that lock; tcmalloc (option C) caches them per
              thread instead.
Interview Q:  "Why not mmap everything above 32 KiB?" — Each mmap'd block costs
              two syscalls plus a page fault per 4 KiB page touched. A 64 KiB
              buffer would pay ~18 kernel entries per malloc/free cycle, versus
              zero when it comes from a recycled span.

## D-5: Thread-cache design            (Phase 0)
Context:      Taking a lock on every malloc serialises threads. Almost all
              allocations are small and short-lived, so a per-thread cache
              should satisfy most of them with no synchronisation at all.
Options:      A) per-thread arenas (glibc): each thread owns a whole heap,
                 memory freed by another thread must go back to its owner
              B) per-thread, per-class free lists in front of shared central
                 lists (tcmalloc); objects move between caches in batches
              C) per-CPU caches using restartable sequences (modern tcmalloc)
Choice:       B. Each thread has one singly-linked free list per class, with a
              count and a limit (2 × batch). A miss fetches a *batch* from the
              central list (one lock for up to 32 objects). A push past the
              limit flushes a batch back. Batch size = clamp(64 KiB / size, 2, 32).
              The cache lives in `thread_local` storage with a trivial
              constructor and destructor (initial-exec TLS). A `pthread_key`
              destructor flushes it when the thread exits.
Why:          The fast path is a pointer pop with no atomics and no locks.
              Cross-thread frees need no special case: memory freed by thread B
              enters B's cache and flows back through the central list, so
              "ownership" is never tracked. `pthread_key` destructors never
              allocate, unlike C++ `thread_local` destructors (glibc's
              `__cxa_thread_atexit_impl` calls `calloc`, which would re-enter
              our malloc).
Trade-off:    Memory can pile up in the cache of a thread that frees a lot but
              never allocates (up to 2 × batch objects per class, bounded).
              Per-CPU caches (C) bound total cache memory by CPU count rather
              than thread count and avoid the thread-exit flush entirely, but
              need `rseq` and are far more complex.
Interview Q:  "What happens when thread A allocates and thread B frees?" — B
              pushes the block onto its own cache. When B's list exceeds its
              limit, a batch goes to the central list, where A (or anyone)
              refills from it. It is correct because blocks are fungible.

## D-6: Build, test and CI tooling            (Phase 0)
Context:      CI must reproduce local results exactly, including formatting
              and lint, across gcc and clang.
Options:      A) system packages on the CI runner (versions drift from local)
              B) pin clang-format / clang-tidy through PyPI wheels, and pin
                 GoogleTest through CMake FetchContent
              C) run everything in a custom Docker image
Choice:       B. clang-format and clang-tidy 21.1 come from pip in CI (the
              same major version as local). GoogleTest v1.17.0 comes from
              FetchContent. `tools/check_format.sh` and `tools/run_tidy.sh` are
              the single source of truth, called by both CI and developers.
Why:          clang-format output differs between major versions, so an
              unpinned CI would fail on correctly formatted code. Wheels
              install in seconds, with no image to maintain.
Trade-off:    FetchContent needs network access at configure time. A Docker
              image (C) would pin the compiler too, which matters if results
              must be bit-for-bit reproducible.
Interview Q:  "How do you keep lint green locally and in CI?" — Both call the
              same script with the same pinned tool versions, and CI runs it as
              a separate job, so a style failure is never confused with a test
              failure.

## D-7: Development environment layout            (Phase 0)
Context:      Development happens on Windows with WSL2. The repository lives on
              the NTFS drive (so Windows tools and git see it), but it must
              build and run on Linux.
Options:      A) repo inside the WSL filesystem
              B) repo on NTFS, built from WSL through `/mnt/d`, with build
                 directories in the Linux filesystem
Choice:       B, with `.gitattributes` forcing LF line endings for every text file.
Why:          Keeps a single working tree and a single git identity. Build
              directories under `~/build` avoid the slow 9P filesystem for
              object files and benchmark binaries.
Trade-off:    Source reads over 9P slow the first compile. Option A is faster
              for large codebases.
Interview Q:  "Why do your scripts care about line endings?" — A CRLF shebang
              line (`#!/bin/sh\r`) fails with "bad interpreter". `eol=lf` makes
              the checked-in bytes identical on every platform.
