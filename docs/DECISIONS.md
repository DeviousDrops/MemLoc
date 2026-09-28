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
