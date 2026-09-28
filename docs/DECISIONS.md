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
              limit flushes a batch back. Batch size = clamp(64 KiB / size, 2, 32)
              (upper bound raised to 128 after measurement, see D-17).
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

---

## D-8: Free-object representation and span carving            (Phase 1)
Context:      A span must track which of its objects are free, cheaply, and
              without touching memory the program has not used yet.
Options:      A) per-span bitmap of free slots
              B) intrusive singly-linked free list (the first 8 bytes of a free
                 object point to the next one), threaded through the whole span
                 when it is created
              C) intrusive free list for *returned* objects, plus a bump
                 pointer for never-used objects ("lazy carving")
Choice:       C. `Span` holds `free_list` (recycled objects), `carve` (next
              never-used object) and `limit` (end of the last whole object).
              Allocation pops `free_list`, else bumps `carve`.
Why:          Zero metadata per object, O(1) push and pop, and a fresh span
              touches only the pages actually handed out. With option B,
              creating a 256 KiB span would write to every page and fault it
              in, even if the program only needs one object.
Trade-off:    Intrusive lists are fragile: a use-after-free write corrupts the
              `next` pointer and the damage shows up later as a wild pointer.
              That is exactly what debug mode (Phase 4) exists to catch. A
              bitmap (A) is immune to this and gives cheap "is this slot free?"
              queries (useful for double-free detection), but costs a scan
              per allocation.
Interview Q:  "Where do you store the free list?" — Inside the free objects
              themselves: a free block is dead memory, so its first word is
              the `next` link. That is why the minimum class is 16 bytes
              (at least one pointer, with 16-byte alignment).

## D-9: Edge-case semantics of the malloc API            (Phase 1)
Context:      Where the C standard is loose, drop-in replacement means matching
              glibc, because real programs rely on glibc's behaviour.
Options:      A) follow glibc everywhere; B) follow the letter of C23 (e.g.
              `realloc(p, 0)` is undefined behaviour); C) mimic jemalloc/mimalloc
Choice:       A.
              - `malloc(0)` returns a unique 16-byte block.
              - `realloc(p, 0)` frees `p` and returns NULL.
              - `realloc(NULL, n)` behaves like `malloc(n)`.
              - `calloc` checks `n*size` overflow → NULL + `ENOMEM`.
              - Every failure sets `errno = ENOMEM`. `posix_memalign` returns an
                error code and leaves `errno` untouched.
              - `realloc` stays in place when the new size maps to the same
                small class (or the same number of medium units), and moves
                otherwise, including large shrinks, so a shrunk block never
                pins a big one.
Why:          Programs under LD_PRELOAD were written and tested against glibc.
              Any difference is a latent bug that only shows up with our
              allocator.
Trade-off:    Freeing on `realloc(p, 0)` is exactly the case C23 made
              undefined; a strictly portable program can't rely on it.
              jemalloc returns a minimal block instead, which avoids the classic
              "realloc returned NULL, so was p freed?" ambiguity.
Interview Q:  "What does realloc(p, 0) do?" — In glibc (and here) it frees p
              and returns NULL. Since C23 it is UB, precisely because
              implementations disagreed.

## D-10: Aligned allocation strategy            (Phase 1)
Context:      `posix_memalign` and `aligned_alloc` can ask for any power-of-two
              alignment, and the result must still be freeable by plain `free`.
Options:      A) over-allocate `n + align` and store an offset before the
                 pointer (breaks header-free small objects)
              B) serve from a size class whose size is a multiple of `align`:
                 span bases are 64 KiB-aligned, so every object in such a class
                 is aligned. Medium spans are 64 KiB-aligned by construction.
                 Anything else goes to the huge path, which can pad arbitrarily.
Choice:       B. align ≤ 16 → plain malloc. align ≤ 32 KiB and n ≤ 32 KiB →
              the smallest class ≥ max(n, align) that is a multiple of align.
              align ≤ 64 KiB → a medium span. Larger → huge mmap, padded so
              that the header still sits directly before the pointer.
Why:          No new metadata and no special case in `free`: an aligned block
              is an ordinary block.
Trade-off:    `aligned_alloc(4096, 100)` uses a 4 KiB object (the price of a
              page-aligned small allocation in any allocator). Very large
              alignments waste up to `align` bytes of address space (not RSS)
              in the huge path.
Interview Q:  "How does free() know an aligned block was over-allocated?" —
              It wasn't. Small aligned blocks come from classes whose size is
              a multiple of the alignment, and huge blocks record their real
              mapping base in the header before the pointer.

## D-11: Huge-block realloc via mremap            (Phase 1)
Context:      Growing a large buffer by repeated `realloc` (vectors, string
              builders) is O(n²) bytes copied if every step copies.
Options:      A) malloc + memcpy + free
              B) `mremap(MREMAP_MAYMOVE)`: the kernel moves page-table entries
                 and copies no data
Choice:       B for huge → huge; A for any transition between tiers.
Why:          The 64-byte header lives inside the same mapping, so it moves
              with the data. Only the pointer offset from the mapping base
              needs preserving, and it is.
Trade-off:    Linux-specific (fine, D-7 non-goals). After a move, an alignment
              above the page size is not preserved, but `realloc` never
              promises more than fundamental alignment.
Interview Q:  "How can realloc of a 100 MiB block be O(1)?" — `mremap`
              relocates page-table entries, not bytes. Only the page tables
              are touched.

## D-12: When an empty span goes back to the page heap            (Phase 1)
Context:      When the last live object of a small span is freed, the span can
              either stay in its class or return to the page heap for any use.
Options:      A) never return (memory stays bound to one class forever)
              B) always return (alloc/free of one object can thrash span
                 setup and teardown)
              C) return unless it is the class's only span with free space
Choice:       C.
Why:          A workload that shifts from small to large objects can reuse
              memory across classes (this matters for the fragmentation
              benchmark), while a class that alternates between 0 and 1 live
              objects keeps one warm span.
Trade-off:    Up to 40 idle spans (one per class, 64–256 KiB each) are held
              forever. A time-based decay (jemalloc) is smarter, but needs a
              clock and a background purge.
Interview Q:  "What is hysteresis in an allocator?" — Keeping a little slack
              (one empty span) so that a boundary oscillation doesn't turn into
              repeated expensive transitions.

---

## D-13: Central locking and lock ordering            (Phase 2)
Context:      Central lists and the page heap are shared by all threads and
              must be protected. Lock granularity decides both scalability and
              deadlock risk.
Options:      A) one global allocator lock
              B) one mutex per size class plus one page-heap mutex, each
                 central list padded to its own cache line
              C) lock-free central lists (a CAS'd Treiber stack per class)
Choice:       B. `struct alignas(64) CentralList { std::mutex mu; ... }`. The
              only lock nesting is central[c] -> page heap, which happens when
              a class needs a new span or returns an empty one. The page heap
              never calls back into a central list, so no cycle, and so no
              deadlock, is possible. Plain `std::mutex` (a futex): uncontended
              lock/unlock is one atomic RMW each.
Why:          Threads allocating different classes never touch the same lock
              or cache line. Threads on the same class meet once per *batch*
              (up to 32 objects), not once per object. `alignas(64)` stops two
              hot neighbouring locks from sharing a line and ping-ponging
              between cores (false sharing).
Trade-off:    Growing the heap (mmap) happens under the page-heap lock and
              briefly stalls other classes that need a span. Lock-free lists
              (C) avoid convoying but bring ABA hazards with intrusive nodes,
              and the brief lists them as a non-goal.
Interview Q:  "How do you prove it can't deadlock?" - There is a global lock
              order (central before page heap) and no code path acquires them
              the other way round. Holding two central locks at once never
              happens outside fork(), which takes them all in index order.

## D-14: Thread-local storage model and thread-exit flush            (Phase 2)
Context:      The thread cache must be reachable with no function call and no
              allocation, including from inside malloc itself and before any
              C++ initialiser has run. Its contents must go back to the central
              lists when the thread exits.
Options:      A) C++ `thread_local` object with a destructor
              B) `pthread_getspecific` lookup on every call
              C) trivially-constructible `constinit thread_local` in the
                 initial-exec TLS model, plus a `pthread_key` whose destructor
                 flushes the cache
Choice:       C. Each thread's cache starts zeroed in the static TLS block
              (state = Uninit). On its first slow path the thread registers
              the key (`pthread_once` + `pthread_setspecific`, neither of which
              allocates for the first 32 keys). At exit the key destructor
              flushes every list and sets state = Dead. From then on, that
              thread's malloc/free go straight to the central lists.
Why:          A C++ `thread_local` destructor is registered via
              `__cxa_thread_atexit_impl`, which calls `calloc`: malloc would
              recurse into itself on every new thread. Initial-exec TLS
              compiles to a `%fs`-relative load, while the default model for a
              shared library (general-dynamic) calls `__tls_get_addr`, which
              can itself allocate.
Trade-off:    Initial-exec TLS in a shared library only works if the library
              is loaded at startup (LD_PRELOAD or a link-time dependency).
              `dlopen("libmemloc.so")` later may fail with "cannot allocate
              memory in static TLS block". That is acceptable for a malloc
              replacement. The Dead state costs a lock per operation for any
              allocations made by other TSD destructors after ours.
Interview Q:  "Why not use a thread_local with a destructor?" - Registering
              that destructor calls calloc, and calloc is us. The pthread key
              destructor path has no allocation, and initial-exec TLS avoids
              `__tls_get_addr`.

## D-15: Memory ordering            (Phase 2)
Context:      Every piece of shared state needs a stated synchronisation
              story. A data race is undefined behaviour even when the hardware
              "happens to work", and ThreadSanitizer will flag it.
Options:      A) seq_cst atomics everywhere; B) mutexes for all mutable shared
              structures, with explicitly justified relaxed atomics for the
              few lock-free reads
Choice:       B.
              - Central lists, span fields (`free_list`, `carve`, `live`,
                `in_list`) and the page heap: only touched under their mutex,
                which supplies acquire (lock) and release (unlock) ordering.
              - Chunk registry: `fetch_or(relaxed)` under the page-heap lock,
                `load(relaxed)` in `free`. The chunk bit is set before any
                block of that chunk exists. Any thread that frees block p
                obtained p through some happens-before chain from its
                allocation, so the bit is visible (write->read coherence).
              - `span->kind`, `size_class` and `owner[]`: written while the
                span is created (under locks) and read unlocked in `free(p)`.
                There is no race: they only change while the span is free, and
                a free span contains no live block that could be passed to
                `free`.
              - The thread cache: owned by one thread and never read by another.
Why:          Every cross-thread transfer of a block goes through a mutex
              (central list) or through the user's own synchronisation, which
              is where the happens-before edge must come from anyway.
Trade-off:    Relaxed loads are only correct because of the argument above. A
              future change that mutates `kind` or `owner[]` on a live span
              would silently break it, so the reasoning is written next to the
              code. seq_cst (A) would be easier to reason about, but costs a
              full fence on every free on arm64.
Interview Q:  "Why is a relaxed load of the registry bit enough in free()?" -
              The bit was set before the chunk's first block was handed out,
              and the freeing thread received the block through a
              synchronising chain (at minimum the user's own). Happens-before
              is transitive, so the store is visible.

## D-16: Thread-cache limits (high-water mark)            (Phase 2)
Context:      An unbounded cache lets a thread that frees heavily hoard memory
              that other threads then have to allocate afresh.
Options:      A) fixed per-class cap; B) a per-thread byte budget with
              dynamic per-class caps (tcmalloc's "slow start"); C) no cache
              for large classes
Choice:       A. Cap = 2 x batch_size(class). When a push exceeds it, one
              batch goes back to the central list, so the list settles
              between batch and 2 x batch objects. Refill always fetches a
              full batch.
Why:          The hysteresis band of width `batch` means a thread oscillating
              around the limit pays at most one lock per `batch` operations.
              The worst-case cache is 2 x 64 KiB of objects per class
              (about 5 MiB per thread if all 40 classes are saturated, in
              practice far less).
Trade-off:    Threads with very skewed size profiles would benefit from
              larger caps on their hot classes and smaller ones elsewhere (B).
              That needs per-thread tuning state and a stealing policy.
Interview Q:  "What bounds memory held in thread caches?" - The per-class cap:
              at most 2 batches per class per thread. Flushing on thread exit
              returns everything.

## D-17: Transfer cache and larger batches for tiny classes            (Phase 2)
Context:      Added mid-phase after measuring. A throwaway probe (8 threads,
              each allocating then freeing a working set of N 64-byte blocks)
              ran at 24 M ops/s with the D-5/D-16 design, against 158 M for
              glibc. At 1 thread MemLoc was 2-3x faster than glibc, so the
              central lists were the bottleneck: every refill and every
              release walked ~32 objects *under the class mutex*, and each of
              those objects was a cache miss because another core wrote it last.
Options:      A) shorten the critical section by moving whole batches
                 (tcmalloc's transfer cache); B) shard each central list;
              C) a lock-free central list (explicit non-goal)
Choice:       A. Each central list keeps a stack of full batches, chained
              through the second word of each batch's head object. A thread
              releasing a full batch pushes it in O(1); a refill of a full
              batch pops one in O(1). Only when that stack is empty (or holds
              more than 1 MiB for the class) do objects go to or come from
              spans. With O(1) critical sections the lock is paid per batch,
              so the batch cap was raised from 32 to 128. A batch still holds
              at most 64 KiB, so only classes below 512 B are affected.
Why:          Measured on the same probe (M alloc+free pairs/s, 8 threads):
              N=100: 25 -> 95 (transfer cache) -> 550-850 (cap 128);
              N=1000: 24 -> 75 -> 265, where glibc gets 140-155.
              The design is still "thread cache + central mutex" as
              required. The change only shortens what happens while the mutex is held.
Trade-off:    Up to 1 MiB per class can sit in the transfer cache, where it
              cannot be returned to the spans (and so to the page heap for
              other classes). Batches of 128 tiny objects make a thread's
              cache up to 256 objects deep for those classes (at most
              2 x 64 KiB per class, as before). Partial lists (thread-exit
              flushes) still take the per-object span path.
Interview Q:  "Your allocator was slower than glibc at 8 threads. How did you
              find and fix it?" - Single-thread speed was fine, so the shared
              part was suspect. The critical section touched every object
              in the batch (a remote cache miss each). Moving whole batches
              made it O(1), and then a bigger batch amortised the lock further.

## D-18: How libmemloc.so replaces glibc malloc            (Phase 3)
Context:      With `LD_PRELOAD`, the dynamic linker searches the preloaded
              object right after the executable and before libc.so.6, so its
              definition of `malloc` wins every lookup (including glibc's own
              internal calls, which go through the PLT for exactly this
              purpose).
Options:      A) define the full malloc family ourselves; B) wrap glibc: define
              `malloc` and forward to the real one via `dlsym(RTLD_NEXT)`
              (only useful for tracing); C) glibc's `__malloc_hook` (removed
              in glibc 2.34)
Choice:       A. `src/preload.cpp` defines every function the glibc manual
              ("Replacing malloc") lists: malloc, free, calloc, realloc, and
              also aligned_alloc, malloc_usable_size, memalign,
              posix_memalign, pvalloc and valloc, plus reallocarray. All are
              thin wrappers over the memloc_* functions. The shared library
              is compiled with `-fvisibility=hidden`, so only those names and
              the memloc_* API are exported. It is linked with
              `-Bsymbolic-functions`, so internal calls never go through
              the PLT, and without libstdc++, so a C program such as sqlite3
              does not pull it in.
              C++ `operator new/delete` are not redefined: libstdc++'s
              versions call malloc/free/aligned_alloc, which are ours.
Why:          Missing even one family member is a classic crash. For
              example, a block from glibc's `memalign` reaching our `free`.
              A pointer we did not allocate is detected (bad huge-header
              magic) and aborts with a message, like glibc's "free(): invalid
              pointer".
Trade-off:    glibc-specific introspection (`mallinfo2`, `malloc_stats`,
              `malloc_trim`, `mallopt`) still reaches glibc's own, unused
              arena and reports nothing useful. That is harmless, and
              documented as a limitation.
Interview Q:  "What happens if you interpose malloc and free but not
              posix_memalign?" - glibc's posix_memalign returns a glibc block,
              and the program later passes it to your free. At best that is
              an invalid-pointer abort. At worst it corrupts your heap.

## D-19: Bootstrap without dlsym            (Phase 3)
Context:      A preloaded malloc runs before any constructor, and possibly
              before libc has finished initialising. The classic trap: a
              wrapper-style malloc calls `dlsym(RTLD_NEXT, "malloc")` on
              first use, and dlsym itself calls `calloc` for its error state.
              That re-enters the wrapper before the pointer is set, so the
              program recurses infinitely or needs a static bootstrap buffer.
Options:      A) dlsym plus a static bump-allocator for the recursive calls;
              B) make the allocator usable with zero initialisation code
Choice:       B. Every piece of global state is `constinit`: the chunk
              registry, page heap, central lists and their mutexes (a small
              `Mutex` wrapper over `pthread_mutex_t` with
              `PTHREAD_MUTEX_INITIALIZER`), plus the thread caches
              (zero-filled static TLS). `std::mutex` was used until this
              phase. It is constexpr too, but its `lock()` throws on error,
              which pulled libstdc++'s exception runtime into
              libmemloc.so. The only lazy step is per-thread cache activation,
              which is recursion-safe (D-14). MemLoc never calls dlsym. On
              the allocation path, the only calls that could reach malloc are
              `pthread_key_create`, `pthread_setspecific` and `pthread_atfork`,
              and they run only during activation.
Why:          There is no "not yet initialised" state, so there is no
              window where a call can observe one.
Trade-off:    Relies on the preloaded object's TLS being static (true for LD_PRELOAD; `dlopen`ing libmemloc.so later is
              unsupported).
Interview Q:  "Why is dlsym dangerous inside malloc?" - dlsym can allocate
              (calloc for its thread-local error buffer), so the first call
              recurses into your malloc before the real-malloc pointer is
              known.

## D-20: fork() safety            (Phase 3)
Context:      fork() copies only the calling thread. If another thread held a
              central or page-heap mutex at that moment, the child inherits a
              lock nobody will ever release, and its next malloc deadlocks.
              Python's `multiprocessing` (fork start method) hits this in
              practice.
Options:      A) `pthread_atfork` handlers that take every allocator lock
              before fork and release them in both processes afterwards;
              B) re-initialise the locks in the child (undefined behaviour for
              a mutex locked by another thread)
Choice:       A. The prepare handler locks all 40 central mutexes in index
              order and then the page-heap mutex, which respects the
              central -> page heap order (D-13). The parent and child
              handlers unlock in reverse. The handlers are registered once,
              on the first thread-cache activation, so the static library
              used by tests gets them too.
Why:          It is the standard solution (glibc's own malloc does the
              same). Because fork happens with every lock held by the forking
              thread, the child's heap is consistent.
Trade-off:    Objects sitting in *other* threads' caches are unreachable in
              the child, a bounded leak (at most 2 batches per class per
              thread). fork() also becomes slightly slower (41 lock
              operations).
Interview Q:  "Why must the prepare handler respect the lock order?" - It
              runs while other threads are still allocating. Taking locks
              out of order could deadlock against a thread holding a central
              lock and waiting for the page heap.

## D-21: Verifying the drop-in            (Phase 3)
Context:      "It didn't crash" is weak evidence. A preload that silently
              failed to load would pass every test.
Options:      A) trust LD_PRELOAD; B) assert interposition explicitly
Choice:       B. `memloc_preload_tests` (GoogleTest, not linked against
              memloc_core) first checks with `dladdr(&malloc)` that malloc
              resolves into libmemloc.so, then exercises every family member,
              C++ new/delete (including over-aligned new), threads and
              fork-while-allocating. `tools/preload_demo.sh` runs a sqlite3
              and a python3 workload with and without the preload,
              diffs their outputs, and checks from `LD_DEBUG=bindings` that
              the program's own malloc binds to libmemloc.so. Both run under
              ctest with the label `preload` and are skipped in sanitizer
              builds (sanitizers interpose malloc themselves).
Why:          Every claim ("sqlite3 runs on MemLoc") is backed by a check
              that would fail if it were not true.
Trade-off:    The demo depends on sqlite3 and python3 being installed. When
              either is missing, the corresponding step is skipped with a
              message.
Interview Q:  "How do you prove the program actually used your allocator?" -
              Ask the dynamic linker. `dladdr` on the resolved malloc
              address names the defining object, and `LD_DEBUG=bindings`
              logs each symbol binding.

## D-22: A debug mode that costs nothing when off            (Phase 4)
Context:      `MEMLOC_DEBUG=1` must turn on heavy checking at run time. The
              brief also requires that the normal build pay nothing for it.
Options:      A) a separate debug build of the library; B) test a global
              flag at the top of every entry point; C) keep the fast paths
              flag-free and consult the flag only on paths that are already slow
Choice:       C. malloc and free (and calloc) are restructured as
              "try the thread cache inline, else call a noinline slow path".
              Only the slow path reads `g_debug`. In debug mode the thread
              caches are never used: they stay empty, and the main thread's
              pre-debug cache is flushed when debug mode starts. So every
              call falls through to the slow path, where the debug
              allocator takes over. realloc, memalign and malloc_usable_size
              test the flag once at entry (a predictable branch). They are not
              the paths that dominate allocation-heavy programs.
              The flag is set by a constructor that reads the environment, so
              the few allocations made before it ran are ordinary blocks.
              The debug allocator recognises them (see D-23) and frees them
              normally.
Why:          It keeps the promise literally for the hot paths: the
              objdump of the malloc/free fast paths contains no reference to
              the flag (checked in Phase 4, recorded in PROGRESS). And one
              binary serves both uses.
Trade-off:    In debug mode, thread caches are off, so allocation takes the
              central locks every time. Debug mode is for finding bugs,
              not for speed.
Interview Q:  "How can a runtime flag be zero-cost?" - Put the check where
              the code is already paying for something slow. Debug mode
              forces every call onto that slow path by keeping the caches
              empty.

## D-23: Debug block layout and what it detects            (Phase 4)
Context:      The brief lists overflow/underflow (canaries), double free,
              use-after-free (FIFO quarantine with 0xDD poison, checked on
              eviction) and leaks grouped by call site.
Options:      A) redzones in separate guard pages (Electric Fence: exact, but
              one page per allocation); B) inline header + canaries around
              each block, with a quarantine (ASan-like without shadow memory)
Choice:       B. A debug block is an ordinary MemLoc block of
              `208 + n + 16` bytes (plus alignment slack):

                raw                                 user = raw+208      user+n
                | DebugHeader (192 B)  | canary 16 B | user bytes ...   | canary 16 B |
                  magic^user, size, raw, live-list links, tids,
                  alloc stack[8], free stack[8]

              - The magic is XORed with the user address, so a stray copy of it
                in user data cannot make an unrelated address look like a
                debug block. Live and freed blocks use different magics.
              - free(): freed magic -> **double free**. No debug magic and not
                the start of a normal block -> **invalid free**. Damaged front
                or back canary -> **underflow** / **overflow**, with the first
                bad offset.
              - Freed blocks are filled with 0xDD and parked in a FIFO
                quarantine (at most 4096 blocks / 64 MiB). On eviction, and for
                every block still queued at exit, a byte that is no longer
                0xDD means a **use-after-free write**.
              - malloc() fills new blocks with 0xBE, which makes reads of
                uninitialised memory visible. calloc still zeroes.
              - realloc() always moves the block, so the old pointer goes
                through quarantine. Stale pointers after realloc are then
                caught like any other use-after-free.
              - Live blocks are on a doubly linked list. At exit, blocks
                still live are **leaks**, grouped by allocation stack.
              malloc_usable_size() returns the requested size, so programs
              that trust it never touch the canary.
Why:          It finds every bug class in the brief with a fixed, small
              per-block overhead. It needs no shadow memory and no compiler
              instrumentation, so it works on unmodified binaries via
              LD_PRELOAD.
Trade-off:    Detection is deferred: an overflow is reported at free, and a
              use-after-free write at eviction or exit. Neither is reported
              at the faulting instruction as with ASan. Reads after free are
              not detected. Blocks freed while debug mode starts up (before
              the constructor) are ordinary and unchecked.
Interview Q:  "Why XOR the magic with the address?" - A live header contains
              the magic, so user data copied from another block's header
              (or a pointer-sized value that happens to match) would fool a
              constant magic. XORing with the block's own address makes a
              header valid only at the one place it belongs.

## D-24: Capturing and symbolising stacks            (Phase 4)
Context:      Reports are only useful with call stacks: where the block was
              allocated, freed and misused.
Options:      A) frame-pointer walking (fast, but fails when the program is
              built without frame pointers); B) glibc `backtrace()` (DWARF
              unwinding via libgcc_s); C) libunwind (an extra dependency)
Choice:       B, with three precautions:
              - The first call of backtrace() dlopens libgcc_s, which
                allocates. The constructor makes one warm-up call under the
                recursion guard, so this never happens mid-report.
              - A thread-local recursion guard sends any allocation made
                *while* the debug allocator is working to the plain
                allocator.
              - Stacks are capped: 8 frames stored per allocation and per free,
                32 frames for the current stack in a report. MemLoc's own
                frames are skipped.
              In-process symbolisation uses dladdr() only: module path,
              offset in the module, and the nearest exported symbol (bug
              programs are linked with `-rdynamic`). File and line come
              offline from `tools/symbolize.py` (addr2line on module +
              offset), because that needs DWARF parsing and memory, neither of
              which belongs inside a crashing process.
Why:          Correct stacks for any binary, and no extra dependency.
Trade-off:    backtrace() costs microseconds per call, so debug mode slows
              allocation-heavy programs considerably.
Interview Q:  "Why not call addr2line from inside the crashing process?" -
              The process is in an unknown state. Fork/exec and DWARF
              parsing allocate and take locks. Record module + offset,
              which is enough, and symbolise out of process.

## D-25: Crash report format            (Phase 4)
Context:      Reports feed both humans and the Phase 6 triage tool.
Options:      A) human-readable text like ASan; B) JSON lines
Choice:       B. One JSON object per line, written with `write(2)` from a
              static buffer (no stdio, no malloc) to `$MEMLOC_REPORT`
              (opened O_APPEND) or to stderr. A one-line human summary
              always goes to stderr. Fields: `type`, `pid`, `tid`,
              `program`, `address`, `size`, `detail`, `stack`,
              `alloc_stack`, `free_stack`, and `heap` (mapped bytes, live
              debug blocks and bytes, quarantine fill). Each frame is
              `{pc, module, offset, symbol, symbol_offset}`. Errors then
              `abort()`. Leaks do not abort. They produce one `leak`
              record per allocation site and a `leak-summary`.
              Leak groups whose allocation stack never leaves system libraries
              (libc, libstdc++, ld.so, libmemloc itself) are counted
              in the summary as `system_blocks`, not reported as sites.
              Example: stdio's never-freed buffer.
              *Amended during implementation:* stdio's buffer is allocated
              from printf with `main` still on the stack, so the "never
              leaves system libraries" rule alone misses it. A group also
              counts as system when its innermost frame is in the dynamic
              loader (thread TLS blocks) or any frame is
              `_IO_file_doallocate`. Canary and use-after-free records carry
              a numeric `offset`, leak records `blocks`/`bytes`, `segv`
              records `signal`. At most 25 sites are reported individually.
Why:          Machine-readable from the start. The triage tool groups by
              `type` + top user frames without parsing text.
Trade-off:    A leak here means "not freed at exit" with no reachability
              scan (LSan scans roots). The system-library filter removes
              most of the noise that causes.
Interview Q:  "Why write reports with write(2) and a static buffer?" - The
              heap is known to be corrupt when the report is written. Any
              path that allocates, or takes stdio's lock (which may be held
              by the thread that just crashed), can deadlock or crash again.

## D-26: SIGSEGV handler            (Phase 4)
Context:      Wild and null dereferences never reach the allocator. The brief
              asks for a signal-safe handler that dumps allocator state.
Options:      A) none (rely on core dumps); B) a handler that reports and
              then lets the default action happen
Choice:       B, installed only in debug mode, for SIGSEGV and SIGBUS. It runs
              on a sigaltstack, so a stack overflow can still be reported.
              The handler only uses async-signal-safe operations: write,
              readlink, getpid, and lock-free reads of allocator counters
              and span descriptors. It classifies the fault address as a
              null page (< 4096), inside a MemLoc chunk (with span kind,
              class and offset), or outside the heap. It then emits a `segv`
              record, restores SIG_DFL, and returns: the fault repeats and
              the process dies with the usual signal and core dump.
              The stack comes from backtrace() plus the faulting PC from the
              ucontext.
Why:          The report is written, and the exit status and core dump are
              exactly what they would have been without MemLoc.
Trade-off:    backtrace() is not on POSIX's async-signal-safe list. After
              the warm-up it neither allocates nor locks in practice (the
              same pragmatic choice many crash reporters make). The heap
              snapshot is read without locks, so it can be torn.
              *Found during implementation:* frames are named with dladdr(),
              which takes the loader's (recursive) lock and is not
              async-signal-safe either; without it the record has no module
              offsets and cannot be symbolised. The alternate stack is
              per-thread, so only the main thread can report its own stack
              overflow.
Interview Q:  "What can you safely do in a SIGSEGV handler?" - Only
              async-signal-safe functions: no malloc, no printf, no mutexes.
              Pre-allocate everything (alt stack, buffers), format by hand,
              write(2), then re-raise with the default disposition.

## D-27: Bug-program suite            (Phase 4)
Context:      "Debug mode catches all bug programs" must be checkable.
Options:      A) gtest death tests (in-process, and they fight LD_PRELOAD);
              B) standalone programs plus a runner
Choice:       B. `tests/bugs/*.c|cpp`: 11 small programs, each with one
              realistic bug (off-by-one strcpy, underflow, double free,
              write-after-free, stale pointer after realloc, interior free,
              freeing a stack address, leaks from two sites, null
              dereference, C++ new[] overflow, cross-thread use-after-free).
              They are built `-O0 -g -fno-builtin -rdynamic`.
              `tests/bugs/expected.json` records each program's expected
              report type, exit signal, the function the report must point
              at, and the ground-truth root cause and fix (reused by the
              Phase 6 evaluation). `tools/run_bugs.py` runs one program under
              LD_PRELOAD with MEMLOC_DEBUG=1, symbolises the report and checks
              it. It is registered with ctest (label `bugs`), so CI runs it.
Why:          Each program is a regression test for one detector, run in
              the configuration a user would really use.
Trade-off:    Depends on addr2line (binutils) being installed. It is part of
              build-essential, so CI has it.

## D-28: How memloc_stats() gets its numbers            (Phase 5)
Context:      `memloc_stats()` must report how much memory the allocator holds
              and how much of it the program is using. D-22 already paid to
              keep the malloc/free fast paths free of anything except the
              thread-cache list operation.
Options:      A) counters updated on every malloc/free (per-thread, summed on
              demand), as tcmalloc does; B) counters updated only on slow
              paths; C) compute everything on demand by walking the heap
              under its locks
Choice:       C, plus two atomics on the OS-mapping path. `memloc_stats()`
              takes every central lock and then the page-heap lock (the fork
              handler's order, D-20). It then walks the chunk list: each
              span's kind, length and `live` count give small-span bytes,
              blocks outside the span, and medium bytes. Blocks parked in
              transfer batches are subtracted. Huge mappings and the
              current/peak mapped total are atomics updated in `huge.cpp`
              and `os.cpp`, whose paths already make a system call.
              `allocated_bytes` therefore means "blocks the shared heap has
              handed out". That is in use by the program *or* sitting in some
              thread's cache, and each thread cache is bounded by 2 batches
              per class (D-14).
Why:          Zero cost on the hot path, and the walk is exact for everything
              the heap owns. Stats are read rarely (at exit, in benchmarks).
Trade-off:    Calling it is not free: it stops every allocating thread for
              a walk over all chunks (microseconds for a few hundred MiB).
              It cannot separate "in use" from "thread-cached" without
              per-thread counters. Reading other threads' caches would race.
Interview Q:  "Why not keep a live-bytes counter?" - A shared atomic on every
              malloc/free would be the hottest cache line in the
              process. Per-thread counters work but still cost an add per
              operation. The walk moves the cost to the rare reader.

## D-29: MEMLOC_STATS=1 exit report            (Phase 5)
Context:      The brief asks for `MEMLOC_STATS=1`: statistics without code
              changes.
Options:      A) JSON on stderr; B) a human-readable table on stderr;
              C) both, selected by the value
Choice:       B. At exit, a destructor prints totals (mapped, peak mapped,
              allocated, huge) and one row per size class that owns spans:
              block size, spans, span bytes, blocks out, and utilisation. It
              uses a stack buffer and `write(2)`, never malloc. The
              machine-readable form is the `memloc_stats()` struct, which
              the benchmarks use.
Why:          A person runs `MEMLOC_STATS=1 prog` to look. Programs that want
              data link the API.
Trade-off:    The report is a snapshot taken shortly before exit. Frees done
              by destructors that run after ours are not in it.
Interview Q:  "How do you print from inside an allocator at exit?" - No
              printf (it allocates its buffer). Format into a stack buffer,
              `write()` it, and run from a late destructor so most of the
              program has already cleaned up.

## D-30: Benchmark method            (Phase 5)
Context:      The README must compare MemLoc with glibc and mimalloc honestly.
Options:      A) link each benchmark three times, once against each
              allocator; B) one set of benchmark binaries calling plain
              `malloc`, with the allocator chosen by `LD_PRELOAD`
Choice:       B. The `bench/` programs are built `-O2 -fno-builtin` against
              plain libc. `tools/bench.py` runs each one with no preload
              (glibc 2.43), with `libmimalloc.so` (Ubuntu's mimalloc 3.2.8),
              and with `libmemloc.so`, using thread counts 1/2/4/8/16. It
              reports the median of 5 runs per point plus the min-max range.
              Results go to `bench/results/*.csv` together with
              `machine.json` (CPU, cores, RAM, kernel, compilers, library
              versions). The plots are generated from those files only.
Why:          The same machine code runs against all three allocators, so
              only the allocator differs. Every figure in the README
              can be regenerated from committed data.
Trade-off:    The machine is a WSL2 VM on a laptop (i7-13700H, 6P+8E
              cores, 20 threads). Turbo and the hybrid cores add noise, which
              the median of 5 only partly removes. Thread counts above 8
              run on E-cores and hyperthreads, so scaling there is not linear
              for any allocator. Distribution packages may be built with
              different flags from upstream.
Interview Q:  "How do you make an allocator benchmark fair?" - Same binary,
              same inputs and seeds, and only the allocator swapped. Report
              the spread, not the best run, and publish the machine and the
              raw data.

## D-31: Benchmark workloads            (Phase 5)
Context:      The brief lists larson-style and random-size throughput,
              RSS / live, and realloc growth.
Choice:       Four programs, each printing CSV:
              - `bench_larson`: each thread owns 1000 slots of 16-256 B and
                replaces random slots. At every round boundary, threads hand
                their slot arrays to the next thread, so a steady fraction
                of frees are of blocks another thread allocated. This
                pattern punishes allocators that have no cross-thread free
                path. Metric: M ops/s (one op = free + malloc).
              - `bench_random`: each thread keeps a window of 256 live
                blocks with sizes drawn log-uniformly from 8 B to 32 KiB,
                replacing a random one per op. Metric: M ops/s.
              - `bench_rss`: allocate 256 MiB of log-uniform blocks, free a
                random 50 %, then allocate again with different sizes.
                Report VmRSS / live bytes after each phase (1.0 = no
                overhead).
              - `bench_realloc`: 64 buffers, each grown in 1-64 B steps to
                1 MiB, in round-robin order so that a buffer cannot always
                grow in place. Metric: ns per realloc.
Why:          The four programs test four separate parts of an allocator:
              throughput at scale (larson), size-class breadth (random),
              memory efficiency (rss) and the growth path (realloc). Each
              backs a claim the README makes.
Trade-off:    The loads are synthetic. The Phase 3 sqlite3/python3 timings
              remain the "real program" check. None of these simulate a
              long-running server's fragmentation over hours.
Interview Q:  "What does larson measure that a simple malloc/free loop
              doesn't?" - Remote frees: blocks allocated on one thread and
              freed on another, which must find their way back without
              locking the whole heap.

## D-32: Plots and published numbers            (Phase 5)
Context:      The graphs must be generated from the benchmark data, and must
              not mislead.
Choice:       `tools/plot_bench.py` (matplotlib, Agg backend) reads
              `bench/results/*.csv` and writes PNGs to `docs/img/`. It draws
              throughput vs threads as lines with min-max bands, with one
              fixed colour per allocator across all charts and a linear
              y axis starting at zero. RSS / live and realloc are grouped
              bars with their values printed on them. The README quotes
              medians taken from the CSVs and states the losses as plainly
              as the wins.
Why:          The plots and the prose come from the same files, so they
              cannot drift apart.
Trade-off:    PNGs in git are binary diffs (tens of KiB each). This is
              accepted so the README renders on GitHub with no build step.

## D-33: Table lookup for size classes up to 1 KiB            (Phase 5)
Context:      The first larson run had MemLoc behind glibc at 1 thread (about
              70 vs 83 M ops/s). Probing showed that every single size was fast
              (100-125 M ops/s), and so was a 16-128 B mix. Only mixes that
              crossed 128 B were slow, even with 20 live blocks. The
              arithmetic `size_to_class` (D-1) branches on `n <= 128`, and
              uniform 16-256 B requests take that branch in either direction
              half of the time: about one misprediction per malloc.
Options:      A) a branch-free arithmetic formula; B) a lookup table indexed
              by 16-byte granule for small requests, as tcmalloc does
Choice:       B. A 65-entry `uint8_t` table covers 0-1024 B. Above that, the
              arithmetic form is kept (one predictable branch on
              `n <= 1024`). A `static_assert` proves that the table and
              the formula agree for every size up to 1 KiB.
Why:          Measured with the same probe on mixed 16-256 B requests: 71 → 110
              M ops/s. Single-size loads are unchanged.
Trade-off:    65 bytes of read-only data, which is always in cache while
              allocating.
Interview Q:  "How did you find it?" - Bisect the workload, not the code:
              single sizes fast, a mix below 128 fast, a mix across 128 slow
              regardless of working-set size. That points to control flow,
              not memory, and the only data-dependent branch on the path
              was the class computation.

## D-34: realloc resizes medium spans in place            (Phase 5)
Context:      The realloc benchmark grows 64 buffers through the medium tier.
              realloc (D-11) moved a medium block whenever its unit count
              changed, even when the next units in the chunk were free.
Options:      A) always allocate + copy + free; B) resize the span in place
              when its neighbour is free, falling back to A.
Choice:       B. `PageHeap::resize_span` grows a span into the free units right
              after it, or gives back its tail, under the page-heap lock.
              realloc tries it before moving a block that stays medium.
Why:          It avoids an allocate + copy of up to 1 MiB per step whenever
              the neighbour is free. It is also what glibc does for its
              own chunks.
Trade-off:    Measured effect on the benchmark: the median over five full
              runs went from 61 ns per call (63d0792) to 53 ns (3832b0c),
              against 65 ns for mimalloc and 75 ns for glibc. It did *not*
              lower peak RSS. `MEMLOC_STATS=1` showed why: 28 chunks stay
              mapped with 95 MiB unused at exit, because freed medium spans
              are never returned to the OS (a README non-goal), and the final
              step of each buffer crosses 1 MiB into the huge tier.
Interview Q:  "Your realloc is faster, so why is its peak RSS the highest?"
              Speed comes from avoiding copies. Peak RSS comes from what
              happens to freed spans: glibc trims and mremaps, MemLoc keeps
              every chunk it ever mapped for reuse. Two separate knobs.

## D-35: Group reports by a stack signature            (Phase 6)
Context:      One bug can produce many identical reports (every run, every
              thread, every leak of the same site). Triage should cost one
              LLM call per distinct bug, not per report.
Options:      A) hash the raw PCs; B) hash module + offset; C) hash the
              report type plus the function names of the innermost frames
              outside libc, the loader and MemLoc, per stack.
Choice:       C. The signature is `type` + up to 3 user-code functions from
              each of `stack`, `alloc_stack` and `free_stack`, hashed to 12
              hex digits. Frames without a function name fall back to
              module basename + offset. `leak-summary` records are dropped
              (they repeat the per-site leak records).
Why:          PCs change with ASLR and offsets change with every rebuild,
              while function names survive both. Keeping the three stacks
              apart separates "same crash site, different allocation site",
              which are usually different bugs.
Trade-off:    Two different bugs in the same function with the same callers
              merge into one group. Line numbers would split them but would
              also split one bug after an unrelated edit.
Interview Q:  "How is this different from crash bucketing at scale?" - It is
              the same idea as Breakpad/Crashpad signatures, minus the
              skip-lists for inlined and generic frames that a large
              codebase needs.

## D-36: Structured output via a JSON schema            (Phase 6)
Context:      The tool must turn a free-text model answer into fields the
              eval can score and a script can consume.
Options:      A) ask for JSON in the prompt and parse it; B) a forced tool
              call; C) `output_config.format` with a JSON schema.
Choice:       C, with `additionalProperties: false` and every field required:
              `bug_type`, `culprit_function`, `root_cause`, `fix`,
              `confidence` (low/medium/high).
Why:          The API guarantees schema-valid JSON, so there is no retry-on-
              parse-error path. Forced tool choice is being removed on newer
              models, and structured output is the documented replacement.
Trade-off:    `refusal` and `max_tokens` stop reasons can still end a
              response early, so the code checks `stop_reason` before
              parsing. The client also re-checks the fields, which costs a
              few lines.
Interview Q:  "Why also validate after a guaranteed schema?" - The guarantee
              covers the model's output, not the tool's own fakes and mocks.
              The same validator guards the mock and the real path.

## D-37: One Claude call per group, with source context            (Phase 6)
Context:      A stack of function names is often not enough to find the
              root cause. Knowing that `release_request` double-freed does
              not say which earlier path freed the pointer.
Choice:       One `messages.create` per group on `claude-opus-5` with
              adaptive thinking. The prompt holds the report (type, detail,
              and the three stacks as function file:line) and ±12 lines of
              source around each user frame, resolved against the repository
              if the recorded path does not exist. Server-side fallbacks
              (`fallbacks: "default"`) are on. The API key comes only from
              `ANTHROPIC_API_KEY`, which is never written anywhere.
Why:          Source context turns "where" into "why". The model is the
              skill's default, and fallbacks re-run a declined request
              instead of returning an empty answer.
Trade-off:    Prompts are a few KB per group. For 11 bug programs that is
              negligible, and for a large crash corpus the groups (D-35)
              bound the cost. The system prompt is shorter than the minimum
              cacheable prefix, so there is no prompt caching.
Interview Q:  "What stops the model from inventing a fix?" - Nothing in the
              tool does, and the eval (D-38) measures how often it happens.
              `confidence` lets a human sort the queue.

## D-38: Evaluate against tests/bugs ground truth            (Phase 6)
Context:      tests/bugs/expected.json records, for each of the 11 bugs,
              the report type, the function to blame, a root cause and a fix.
Options:      A) exact match only; B) string similarity; C) exact match for
              the categorical fields, and an LLM judge for free text.
Choice:       C. `type` and `culprit_function` must match exactly. With the
              real model, a second call judges `root_cause` and `fix`
              against the ground truth and returns two booleans with a reason.
              Offline, the judge is a keyword-overlap heuristic.
Why:          Root causes are paraphrases, so exact or fuzzy string matches
              would score correct answers as wrong. The categorical fields are
              cheap to check exactly and catch most failures.
Trade-off:    An LLM judge can be lenient toward an LLM answer. Its reasons
              are logged in the trace so a human can audit every verdict.
              With 11 cases, one miss is 9 points, so the score is a smoke
              test, not a benchmark.
Interview Q:  "How would you trust the judge?" - Spot-check its verdicts
              against human labels. Then measure agreement, and swap the
              judge model to see whether scores move.

## D-39: Mocked model and trace log            (Phase 6)
Context:      CI and contributors have no API key, but the tool still needs
              tests. Real runs need an audit trail.
Choice:       `--llm auto` uses Claude when `ANTHROPIC_API_KEY` is set and
              the rule-based mock otherwise. The mock picks the culprit from
              the stack that the report type points at (alloc for overflows
              and leaks, free for use-after-free, the crash stack otherwise).
              That makes it a real baseline, not a stub. Every model call
              appends one JSON line to a trace file: time, model, signature,
              prompt, raw response, stop reason, token usage and latency.
              Unit tests use the mock plus a fake client object that stands in
              for the SDK.
Why:          Tests run offline in milliseconds and cover the request shape,
              refusal handling and parsing. The baseline shows what the LLM
              adds beyond "blame the innermost frame". Traces make each eval
              reproducible and debuggable.
Trade-off:    The fake client can drift from the real SDK. One real
              `make triage-eval` run is the check for that.
Interview Q:  "What does the LLM beat the baseline on?" - Root cause and
              fix. The baseline already names the function, but it cannot
              explain an ownership bug that spans two functions.
