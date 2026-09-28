# Bug programs for debug mode (D-27). Each one contains a single deliberate
# memory bug; tools/run_bugs.py runs it under LD_PRELOAD=libmemloc.so with
# MEMLOC_DEBUG=1 and checks the report against expected.json.
#
# Built without the project's warning flags (several bugs are exactly what
# -Wall would complain about), unoptimised so the bugs survive, and with
# -rdynamic so dladdr() can name their functions.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

file(GLOB bug_sources CONFIGURE_DEPENDS *.c *.cpp)
foreach(src IN LISTS bug_sources)
  get_filename_component(name ${src} NAME_WE)
  add_executable(bug_${name} ${src})
  target_compile_options(bug_${name} PRIVATE -O0 -g -fno-builtin -fno-omit-frame-pointer -w)
  target_link_options(bug_${name} PRIVATE -rdynamic)
  target_link_libraries(bug_${name} PRIVATE Threads::Threads)
  add_dependencies(bug_${name} memloc)
  add_test(NAME bug_${name}
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/run_bugs.py
            --lib $<TARGET_FILE:memloc> --bin $<TARGET_FILE:bug_${name}> --name ${name})
  set_tests_properties(bug_${name} PROPERTIES LABELS bugs TIMEOUT 120)
endforeach()
