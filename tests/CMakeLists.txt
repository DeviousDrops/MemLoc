include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

add_executable(memloc_tests
  api_test.cpp
  size_class_test.cpp
  smoke_test.cpp
  stress_test.cpp
  thread_test.cpp
)
target_link_libraries(memloc_tests PRIVATE memloc_core memloc_warnings GTest::gtest_main)
target_include_directories(memloc_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
gtest_discover_tests(memloc_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit)

# LD_PRELOAD drop-in checks (D-21). Sanitizer runtimes interpose malloc
# themselves, so these only exist in plain builds (where libmemloc.so does).
if(NOT MEMLOC_SANITIZER)
  add_executable(memloc_preload_tests preload/preload_test.cpp)
  # Not linked against memloc_core: allocations must arrive via LD_PRELOAD.
  target_link_libraries(memloc_preload_tests PRIVATE memloc_warnings GTest::gtest_main)
  target_compile_options(memloc_preload_tests PRIVATE -fno-builtin)
  add_dependencies(memloc_preload_tests memloc)
  gtest_discover_tests(memloc_preload_tests DISCOVERY_TIMEOUT 60
    PROPERTIES LABELS preload TIMEOUT 120
               ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:memloc>")

  add_test(NAME preload_demo
    COMMAND ${PROJECT_SOURCE_DIR}/tools/preload_demo.sh $<TARGET_FILE:memloc>)
  set_tests_properties(preload_demo PROPERTIES LABELS preload TIMEOUT 300 SKIP_RETURN_CODE 77)
endif()
