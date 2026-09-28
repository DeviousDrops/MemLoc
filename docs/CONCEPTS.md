# Concepts the code depends on

These are short explanations of the OS, CPU and C++ ideas MemLoc depends on.
Each one says where it shows up in the code and which decision it drives.
None of it is MemLoc-specific.

## Operating system

**Virtual memory, `mmap` and RSS.**
- `mmap(MAP_PRIVATE | MAP_ANONYMOUS)` reserves address space. The kernel
  gives a page physical memory only on first touch, and that page is zeroed.
- So *mapped* bytes (`memloc_stats().mapped`) and *resident* bytes (RSS,
  `VmRSS` in `/proc/self/status`) are different numbers.
- MemLoc relies on this in three places:
  - The 8 MiB chunk registry costs almost nothing until it is used.
  - `calloc` of a huge block skips the `memset`.
  - A fresh chunk header needs no zeroing.
- The benchmarks report `VmHWM`, the peak RSS (D-30).
- Code: [os.cpp](../src/os.cpp), [page_heap.cpp](../src/page_heap.cpp).

**Alignment by over-mapping.** `mmap` only promises page alignment. To get
a 4 MiB-aligned 4 MiB region, map 8 MiB and `munmap` the pieces before and
after the aligned window (`os_map_aligned`). Aligned chunks are what make
`p & ~(4 MiB − 1)` find a block's metadata (D-2).

**`mremap`.** It moves or grows a mapping by editing page tables, so a
100 MiB `realloc` copies no data (D-11).

**Dynamic linking and symbol interposition.**
- The dynamic linker resolves each undefined symbol by searching loaded
  objects in order: the executable, then `LD_PRELOAD` libraries, then their
  dependencies such as `libc.so.6`.
- A preloaded library that defines `malloc` therefore wins, even for calls
  made inside libc itself. glibc documents this as a supported way to
  replace malloc, and lists the functions a replacement must provide (D-18).
- **Trap:** the compiler knows what `malloc` means and may turn code inside
  the replacement into calls to itself. `preload.cpp` is built with
  `-fno-builtin-*` for this reason.

**Bootstrapping.**
- The first `malloc` can happen before any C++ static constructor runs,
  inside the dynamic linker or libc start-up.
- Allocator state must therefore work with *no* initialisation code. The
  state is `constinit` globals, zeroed BSS, and pthread mutexes set up by
  `PTHREAD_MUTEX_INITIALIZER`.
- A lazy "am I initialised?" check, or `dlsym(RTLD_NEXT, "malloc")`, would
  itself allocate or recurse (D-19).

**`fork` in a multi-threaded process.**
- The child process has one thread, the one that called `fork`. But it has a
  copy of every mutex in whatever state the parent's threads left it.
- A lock held by some other parent thread stays locked forever in the child.
- `pthread_atfork` handlers fix this. They take every allocator lock before
  the fork and release them afterwards on both sides. The lock order must
  match the order used everywhere else, or the handler itself can deadlock
  (D-20).

**Signals and async-signal safety.**
- A signal handler can interrupt any code, including `malloc` holding a lock.
  Only async-signal-safe functions may be called from a handler.
- The SIGSEGV handler runs on an alternate stack (`sigaltstack`), because
  the fault may be a stack overflow. It formats into a fixed buffer.
- It still calls `backtrace` and `dladdr`, which are not on POSIX's safe
  list. A warm-up at start-up makes sure they neither allocate nor take
  MemLoc's locks by then. Most crash reporters make the same pragmatic
  choice, and the code says so (D-26).

## CPU and concurrency

**Caches, cache lines and false sharing.**
- CPUs move memory in 64-byte lines. Two threads writing *different*
  variables that share a line keep stealing the line from each other.
  That is false sharing.
- Each `CentralList` is `alignas(64)` so that locks for different size
  classes never share a line (D-13).

**Branch prediction.**
- A conditional branch that goes each way unpredictably costs a pipeline
  flush of about 15-20 cycles each time it is mispredicted.
- The arithmetic size-class function branched on `n <= 128`. A workload
  mixing sizes on both sides ran at 71 M ops/s, and a table lookup raised
  it to 110 (D-33).
- `[[likely]]` and `[[unlikely]]` only affect code layout. They don't
  change prediction.

**Thread-local storage models.**
- `thread_local` variables in a shared library default to the
  *general-dynamic* model. Each access calls `__tls_get_addr`, which can
  allocate on first use: fatal inside `malloc`.
- The *initial-exec* model (`tls_model("initial-exec")`) reserves space in
  the static TLS block at load time. Each access becomes a single load off
  the thread pointer (`fs` on x86-64, `tpidr_el0` on arm64).
- The catch is that the library must be loaded at start-up (as `LD_PRELOAD`
  is), not `dlopen`ed later (D-14).

**Data races and the C++ memory model.**
- Two unsynchronised accesses to one object, at least one a write, are
  undefined behaviour, even if the hardware "would be fine".
- Mutexes give ordering: an `unlock` *happens before* the next `lock` of
  the same mutex. MemLoc protects all shared mutable structures with
  mutexes.
- `free` reads two things without a lock: the chunk registry bit and the
  span descriptor. A relaxed load of the bit is enough because
  happens-before is transitive. The bit was set before the block existed,
  and the freeing thread got the block through some synchronised chain.
  The descriptor only changes while its span holds no live block (D-15).
- ThreadSanitizer checks this at run time. `make tsan` runs the unit tests
  under it.

**Contention and batching.** A lock taken once per 128 objects costs 128
times less than a lock per object. That is the whole idea of the thread
cache and transfer cache (D-5, D-17), and why their batch size scales
inversely with object size.

## Allocator design

**Size classes and internal fragmentation.**
- Rounding every request up to one of 40 sizes lets blocks of a class be
  interchangeable, so a free list needs no per-block headers.
- The cost is waste inside each block. The classes step by 16 bytes up to
  128, then 4 steps per power of two, which bounds the waste at 20 % (D-1).

**External fragmentation.**
- Free memory can be split into pieces too small or of the wrong kind to
  reuse.
- MemLoc's spans hold one class each, so free slots in a span can only be
  reused by that class. That is why its RSS after the refill benchmark is
  1.32× live bytes versus glibc's 1.02×.
- glibc's boundary-tag heap merges neighbouring free chunks of any size.

**Intrusive free lists.** A free block's first 8 bytes store the pointer to
the next free block (`next_of(p)`). Free memory holds its own bookkeeping,
so the free list costs no extra memory (D-8).

**Metadata lookup.**
- `free(p)` gets no size, so the allocator must find the block's metadata
  from the address alone.
- The options are a header before each block, as glibc does, a radix tree
  of pages (tcmalloc), or an aligned region with a header at its base
  (mimalloc, and MemLoc).
- MemLoc's lookup is two masks, a shift and three loads, and it has no
  per-block headers (D-2).

## C++20 features used

- **`constinit`**: checks at compile time that a global is statically
  initialised. Nothing runs before the first `malloc`.
- **`constexpr` tables with `static_assert`**: the size-class table and its
  invariants are computed and proven at compile time. These include no gaps
  and tail waste at most 1/16 (`classes_are_valid`).
- **`std::atomic_ref`**: atomic operations on a plain array. The registry
  can be zeroed BSS instead of an array of `std::atomic`, which avoids any
  constructor.
- **`std::bit_width`** and `__builtin` intrinsics for log2 and overflow
  checks (`__builtin_mul_overflow` in `calloc`).
- **`[[likely]]`/`[[unlikely]]`** and `__attribute__((noinline))` split fast
  and slow paths (D-22).
- **No exceptions**: `std::mutex::lock` can throw, so `Mutex` calls pthread
  directly. `libmemloc.so` depends only on libc (D-13).
