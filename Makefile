# Convenience wrapper around CMake. All real build logic lives in CMakeLists.txt.
BUILD ?= $(HOME)/build/memloc
TSAN_BUILD ?= $(HOME)/build/memloc-tsan
PYTHON ?= python3
LLM ?= groq
GEN := $(shell command -v ninja >/dev/null && echo -G Ninja)

.PHONY: all build test bugs triage-eval bench plots tsan format format-fix tidy clean

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

# LLM triage of the bug reports, scored against tests/bugs/expected.json.
# Needs GROQ_API_KEY (free at console.groq.com); LLM=mock runs offline.
triage-eval: bugs
	$(PYTHON) tools/triage.py eval --reports $(BUILD)/bug-reports --llm $(LLM) \
	  --trace $(BUILD)/triage-trace.jsonl

# Full benchmark run (~2 min): glibc vs mimalloc vs MemLoc, then the graphs.
bench: build
	tools/bench.py --build $(BUILD)
	tools/plot_bench.py --table

plots:
	tools/plot_bench.py --table

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
