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
)
target_link_libraries(memloc_tests PRIVATE memloc_core memloc_warnings GTest::gtest_main)
target_include_directories(memloc_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
gtest_discover_tests(memloc_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit)
