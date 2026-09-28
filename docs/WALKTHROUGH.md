# Walkthrough: one `malloc` and one `free` through the code

This follows real calls through the source, in the order the code runs. The
file names are clickable, and each step names the function that runs it. The
reasons behind each step are in [DECISIONS.md](DECISIONS.md), cited as D-n.

```
malloc(24)                                         free(p)
  preload.cpp      malloc ──► memloc_malloc          preload.cpp    free ──► memloc_free
  malloc.cpp       size_to_class(24) = 1             chunk.h        in_chunk(p)? span_of(p)
  thread_cache.h   tc_pop(1)  ── hit ──► return      thread_cache.h tc_push(cls, p) ──► return
                     │ miss                                           │ list > 2×batch
  malloc.cpp       malloc_slow ──► alloc              thread_cache.cpp tc_release_batch
  thread_cache.cpp tc_refill(1)                      central.cpp    release_batch
  central.cpp      CentralList::fetch                                 (transfer cache or spans)
                     │ no span with room
  page_heap.cpp    PageHeap::alloc_span ──► grow ──► os.cpp os_map_aligned (mmap 4 MiB)
```

## 1. The call reaches MemLoc

A program started with `LD_PRELOAD=libmemloc.so` calls `malloc(24)`. The
dynamic linker searches the preloaded library before `libc.so.6`, so the
symbol resolves to the `malloc` in [src/preload.cpp](../src/preload.cpp)
(D-18). That wrapper just calls `memloc_malloc`. No initialisation code ran
before this call, and none needs to (D-19). Every global the allocator
touches is `constinit`: zeroed BSS, or a pthread mutex set up by a static
initializer. A thread cache that has never been used is all zeros.

## 2. The fast path: `memloc_malloc` in [src/malloc.cpp](../src/malloc.cpp)

```cpp
if (size <= kMaxSmall) [[likely]] {
  if (void* p = tc_pop(size_to_class(size))) return p;
}
return malloc_slow(size);
```

- `size_to_class(24)` ([src/size_class.h](../src/size_class.h)) reads a
  65-entry table at index `(24 + 15) >> 4 = 2` and gets class 1 (32 bytes).
  A table rather than arithmetic, because the arithmetic version branches
  on `n <= 128`, and mixed sizes mispredict that branch (D-33).
- `tc_pop(1)` ([src/thread_cache.h](../src/thread_cache.h)) reads
  `t_cache.lists[1].head`. `t_cache` is `thread_local` with the
  `initial-exec` TLS model, so the access is one `fs:`-relative load with no
  call to `__tls_get_addr` (D-14). If the list is non-empty, it unlinks the
  head through the object's first word (D-8) and returns it. That is the
  whole fast path: no locks, no atomics, and no check for debug mode (D-22).

On this thread's first call the list is empty, so we fall into
`malloc_slow`. It is `noinline`, which keeps the fast path small enough for
the compiler to inline into callers.

## 3. Refilling the thread cache: `tc_refill` in [src/thread_cache.cpp](../src/thread_cache.cpp)

- `malloc_slow` checks `debug_enabled()` (false), then calls `alloc`,
  `small_alloc`, and then `tc_refill(1)`.
- The cache is `kUninit`, so `activate` runs. It marks the cache active, then
  registers a pthread key whose destructor flushes the cache at thread exit
  (D-14), and installs the `fork` handlers once per process (D-20).
- It asks the central list for one batch: `batch_size(1) = 65536 / 32`,
  clamped to 128 objects (D-5, D-17). It keeps all but the first object in
  `lists[1]` and returns the first one.

## 4. The central list: `CentralList::fetch` in [src/central.cpp](../src/central.cpp)

There is one `CentralList` per class, each `alignas(64)` with its own mutex
(D-13), so threads working on different sizes never contend. `fetch`:

1. It takes the class mutex.
2. If a whole batch sits in the transfer cache (batches released earlier by
   other threads), it hands that over in O(1) (D-17).
3. Otherwise it drains spans that have room. For each span, it first pops
   the recycled `free_list`, then carves never-used objects from `carve`
   up to `limit` (D-8). A span with nothing left is unlinked.
4. If no span has room, it asks the page heap for a new one.

## 5. The page heap: `PageHeap::alloc_span` in [src/page_heap.cpp](../src/page_heap.cpp)

Memory comes from the OS in **chunks**: 4 MiB regions aligned to 4 MiB,
split into 64 units of 64 KiB ([src/chunk.h](../src/chunk.h), D-2, D-3).

- Unit 0 of each chunk is its header. It holds the 64-bit occupancy bitmap
  `used`, the `owner[64]` array and the 64 `Span` descriptors.
- `alloc_span(1, kSmall)` takes the page-heap mutex and looks through the
  chunks for a run of free bits (`find_free_run` on `~used`).
- There are no chunks yet, so `grow` calls `os_map_aligned(4 MiB, 4 MiB)`
  ([src/os.cpp](../src/os.cpp)). That maps 8 MiB and unmaps the misaligned
  head and tail, leaving an aligned 4 MiB window.
- It then sets the chunk's bit in the global **registry**, a bitmap with
  one bit per 4 MiB of the 48-bit address space, 8 MiB of lazily-faulted
  BSS.
- Back in `alloc_span`, the span's bits go into `used`, and `owner[u]` is
  set to the span's first unit for every unit it covers. The descriptor
  gets its kind. The central list then fills in the size class and the
  carve range.

Now the objects travel back up: page heap → central list → a batch of 128
in the thread cache → one object to the program. The next 127 calls to
`malloc(17..32)` on this thread are served by step 2 alone.

## 6. `free(p)`: `memloc_free`

```cpp
if (in_chunk(ptr)) [[likely]] {
  const Span* s = span_of(ptr);
  if (s->kind == SpanKind::kSmall && tc_push(s->size_class, ptr)) return;
}
free_slow(ptr);
```

- `in_chunk(p)` tests the registry bit for `p >> 22` with a relaxed atomic
  load. Relaxed is enough because the bit was set before any block in that
  chunk existed (D-15).
- `span_of(p)` takes three dependent loads and no search. It computes
  `chunk = p & ~(4 MiB − 1)`, `unit = (p − chunk) >> 16`, then
  `span = &chunk->spans[chunk->owner[unit]]`. The span says the block is
  small and belongs to class 1.
- `tc_push` links the block onto `lists[1]`. That is the fast path again, no
  lock.

When a list grows past twice its batch size, `tc_release_batch` detaches
one batch and hands it to `CentralList::release_batch` (D-16). That call
either parks the batch in the transfer cache, or `release_to_spans` returns
each object to its span's `free_list`. A small span whose `live` count drops
to 0 goes back to the page heap, unless it is the class's only span. Keeping
the last one avoids thrashing at the boundary (D-12).

**Cross-thread frees need no special path.** A block allocated by thread A
and freed by thread B simply joins B's cache. Blocks of one class are
interchangeable, so nobody tracks who owns them. The batches moving through
the central list carry memory back to the threads that allocate (D-5).

## 7. The other two tiers

- **Medium, 32 KiB < n ≤ 1 MiB.** `medium_alloc` asks the page heap for
  `ceil(n / 64 KiB)` units as one span of kind `kMedium` and returns its
  base, which is always 64 KiB-aligned. `free` finds the span the same way
  and calls `free_span`, which clears the bits (D-4).
- **Huge, n > 1 MiB.** `huge_alloc` ([src/huge.cpp](../src/huge.cpp)) makes
  its own `mmap`, with a 64-byte `HugeHeader` just before the user pointer.
  `free` sees no registry bit, checks the header magic and calls `munmap`.
  Huge memory goes back to the OS immediately.
- **realloc** (still `malloc.cpp`) tries not to copy.
  - A small block that stays in its class is returned as is.
  - A medium span grows into free units right after it, or gives back its
    tail (`PageHeap::resize_span`, D-34).
  - A huge block is moved by `mremap`, so the kernel remaps the pages
    instead of copying (D-11).
  - Otherwise it allocates, copies and frees.

## 8. Threads, `fork` and exit

- **Thread exit.** The pthread key destructor `flush_on_exit` returns every
  cached object to the central lists. It then marks the cache `kDead`, so
  any malloc from a later TSD destructor bypasses the cache (D-14).
- **fork.** `before_fork` ([src/fork.cpp](../src/fork.cpp)) takes every
  allocator lock in a fixed order: debug, then central lists, then page
  heap. `after_fork` releases them in reverse order, in both parent and
  child. The child therefore never inherits a lock held by a thread that no
  longer exists (D-20).

## 9. The same calls with `MEMLOC_DEBUG=1`

A constructor in [src/debug.cpp](../src/debug.cpp) reads the variable,
flushes the thread cache with `tc_disable` and sets `g_debug`.

- **Why the fast paths are skipped.** Disabled caches are empty, and
  `tc_push` refuses when the cache isn't active. So every call falls through
  to the slow paths, and those are the only places that check the flag
  (D-22).
- **Allocation.** `debug_alloc` wraps each block in a 192-byte header, then
  16 guard bytes on each side, and fills the user bytes with `0xBE`. The
  header holds the magic, the size, both stacks and a link in the live list
  (D-23). It gets the memory from `uncached_alloc`.
- **Free.** `debug_free` checks the magic (double or invalid free) and both
  guards (overflow or underflow). It then fills the block with `0xDD` and
  queues it in a quarantine of 4096 blocks or 64 MiB. A block is really
  freed only when it leaves the quarantine, and the fill is checked at that
  point. A changed fill means a use-after-free write.
- **Reports.** Each finding becomes one JSON line: the stacks are module +
  offset from `backtrace` and `dladdr1` (D-24, D-25). The program then
  aborts.
- **At exit.** Blocks still on the live list are reported as leaks, grouped
  by allocation stack.
- **After the report.** `tools/symbolize.py` adds file:line, and
  `tools/triage.py` groups the reports and asks a model for a diagnosis
  (D-35 to D-39).
