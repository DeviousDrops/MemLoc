# Convenience wrapper around CMake. All real build logic lives in CMakeLists.txt.
BUILD ?= $(HOME)/build/memloc
TSAN_BUILD ?= $(HOME)/build/memloc-tsan
GEN := $(shell command -v ninja >/dev/null && echo -G Ninja)

.PHONY: all build test bugs tsan format format-fix tidy clean

all: test

build:
	cmake -S . -B $(BUILD) $(GEN) -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build $(BUILD)

test: build
	ctest --test-dir $(BUILD) --output-on-failure -j8

# Debug-mode bug suite only, with symbolised reports kept for inspection.
bugs: build
	tools/run_bugs.py --lib $(BUILD)/src/libmemloc.so --bin-dir $(BUILD)/tests/bugs \
	  --keep $(BUILD)/bug-reports

tsan:
	cmake -S . -B $(TSAN_BUILD) $(GEN) -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DMEMLOC_SANITIZER=thread \
	  -DMEMLOC_BUILD_BENCH=OFF
	cmake --build $(TSAN_BUILD)
	ctest --test-dir $(TSAN_BUILD) --output-on-failure -j8 -L unit

format:
	tools/check_format.sh

format-fix:
	tools/check_format.sh --fix

tidy: build
	tools/run_tidy.sh $(BUILD)

clean:
	rm -rf $(BUILD) $(TSAN_BUILD)
