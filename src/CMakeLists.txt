# memloc_core: the allocator as a static library exposing only memloc_* symbols.
# Tests link this directly, so sanitizers see our code rather than an
# interposed malloc.
add_library(memloc_core STATIC
  central.cpp
  huge.cpp
  malloc.cpp
  os.cpp
  page_heap.cpp
  version.cpp
)
target_include_directories(memloc_core
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(memloc_core PRIVATE memloc_warnings)
# The allocator must never throw or depend on RTTI.
target_compile_options(memloc_core PRIVATE -fno-exceptions -fno-rtti)
