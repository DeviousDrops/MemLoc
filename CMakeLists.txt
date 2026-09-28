cmake_minimum_required(VERSION 3.20)
project(MemLoc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(MEMLOC_BUILD_TESTS "Build unit tests" ON)
option(MEMLOC_BUILD_BENCH "Build benchmarks" ON)
set(MEMLOC_SANITIZER "" CACHE STRING "Sanitizer for tests: thread | address | undefined | empty")

# Warnings are errors everywhere in this project.
add_library(memloc_warnings INTERFACE)
target_compile_options(memloc_warnings INTERFACE -Wall -Wextra -Werror)

if(MEMLOC_SANITIZER)
  add_compile_options(-fsanitize=${MEMLOC_SANITIZER} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${MEMLOC_SANITIZER})
endif()

add_subdirectory(src)

if(MEMLOC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MEMLOC_BUILD_BENCH)
  add_subdirectory(bench)
endif()
