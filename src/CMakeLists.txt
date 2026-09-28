# The allocator is compiled once and packaged twice:
#   memloc_core  static library exposing only memloc_* (tests and benchmarks
#                link it directly, so sanitizers see our code rather than an
#                interposed malloc)
#   memloc       libmemloc.so, which adds the standard malloc family for
#                LD_PRELOAD (D-18)
add_library(memloc_objects OBJECT
  central.cpp
  fork.cpp
  huge.cpp
  malloc.cpp
  os.cpp
  page_heap.cpp
  panic.cpp
  thread_cache.cpp
  version.cpp
)
target_include_directories(memloc_objects
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(memloc_objects PRIVATE memloc_warnings)
# The allocator must never throw or depend on RTTI. Only the memloc_* API
# (marked MEMLOC_API) is visible outside libmemloc.so.
target_compile_options(memloc_objects PRIVATE -fno-exceptions -fno-rtti -fvisibility=hidden)

add_library(memloc_core STATIC $<TARGET_OBJECTS:memloc_objects>)
target_include_directories(memloc_core PUBLIC ${PROJECT_SOURCE_DIR}/include)

