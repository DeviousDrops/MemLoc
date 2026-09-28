# MemLoc

A small, fast, debuggable `malloc` replacement in C++20 for Linux (x86-64 and arm64).

MemLoc is a thread-caching allocator in the tcmalloc family. It is small enough
to read in an afternoon (about 2k lines of core code), and every design choice
is written down with its trade-offs in [docs/DECISIONS.md](docs/DECISIONS.md).
It builds as:

- `libmemloc_core.a`: the allocator, exposing `memloc_malloc`, `memloc_free`, … (used by tests)
- `libmemloc.so`: a drop-in replacement that exports the whole malloc family, for `LD_PRELOAD`

> Status: under construction. See [docs/PROGRESS.md](docs/PROGRESS.md).

## Architecture

```
malloc(n)
  ├─ n ≤ 32 KiB ──► size class (40 classes) ──► per-thread cache    (no lock, no atomics)
  │                                                 │ miss / over limit: move a batch
  │                                                 ▼
  │                                  central list per class      (mutex, alignas(64))
  │                                                 │ no span with free objects
  │                                                 ▼
  ├─ 32 KiB < n ≤ 1 MiB ─────────────► page heap: 64 KiB units in 4 MiB aligned chunks
  │                                    (64-bit occupancy bitmap per chunk)  ── mmap 4 MiB
  └─ n > 1 MiB ──────────────────────► direct mmap / mremap / munmap (64-byte header)

free(p)
  registry bit for (p >> 22) set? ── yes ─► chunk = p & ~(4 MiB-1)
                                            span  = chunk->spans[chunk->owner[unit(p)]]
                                            small → thread cache, medium → page heap
                                   └─ no ──► huge header at p-64 → munmap
```

## Deliberate non-goals

These were left out on purpose, to keep the design small enough to finish and defend:

- NUMA awareness
- huge-page (THP / hugetlbfs) tuning
- Windows or macOS support
- a lock-free central heap (central lists use a mutex per size class)
- returning small- or medium-object memory to the OS (huge blocks are unmapped on free)
- a custom `operator new`/`delete` (libstdc++'s already forward to `malloc`)

## Building

Requires Linux, CMake ≥ 3.20, and GCC ≥ 13 or Clang ≥ 17.

```sh
make test          # configure + build into ~/build/memloc and run ctest
make tsan          # ThreadSanitizer build of the unit tests
make format tidy   # clang-format check and clang-tidy
```

## Design documents

- [docs/DECISIONS.md](docs/DECISIONS.md): every design decision, the alternatives considered, and the trade-offs
- [docs/WALKTHROUGH.md](docs/WALKTHROUGH.md): the path of `malloc` and `free` through the code
- [docs/CONCEPTS.md](docs/CONCEPTS.md): the OS and C++ concepts the code depends on
- [docs/PROGRESS.md](docs/PROGRESS.md): a phase-by-phase build log with measured results
