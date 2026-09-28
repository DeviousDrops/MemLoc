# Benchmarks (D-30, D-31). They call plain malloc and are *not* linked
# against MemLoc: tools/bench.py picks the allocator with LD_PRELOAD, so the
# three allocators run identical machine code. -fno-builtin keeps the
# compiler from eliding or merging malloc/free pairs.
find_package(Threads REQUIRED)

foreach(name larson random rss realloc)
  add_executable(bench_${name} bench_${name}.cpp)
  target_link_libraries(bench_${name} PRIVATE memloc_warnings Threads::Threads)
  target_compile_options(bench_${name} PRIVATE -O2 -fno-builtin)
endforeach()
