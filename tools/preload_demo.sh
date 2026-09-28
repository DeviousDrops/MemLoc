#!/usr/bin/env bash
# Runs real programs on MemLoc via LD_PRELOAD (D-21):
#   1. asks the dynamic linker (LD_DEBUG=bindings) whether every `malloc`
#      binding went to libmemloc.so and none to libc.so.6 or another library.
#      A binding to the program itself is fine: an executable that takes
#      malloc's address gets a "canonical PLT" stub that every library binds
#      to, and that stub in turn binds to libmemloc.so (python3 does this),
#   2. runs an allocation-heavy workload with glibc and then with MemLoc,
#   3. requires byte-identical output and prints both wall times.
#
# usage: tools/preload_demo.sh [path/to/libmemloc.so]
# Exits 77 (ctest "skipped") if neither sqlite3 nor python3 is installed.
set -euo pipefail

lib=$(realpath "${1:-$HOME/build/memloc/src/libmemloc.so}")
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
[ -f "$lib" ] || { echo "no such library: $lib" >&2; exit 1; }

now_ms() { local us=${EPOCHREALTIME/./}; echo $((us / 1000)); }

# check_bindings <name> <command...>: fail unless malloc binds only to MemLoc
# (or to the program's canonical PLT stub, which is listed without a path).
check_bindings() {
  local name=$1; shift
  LD_DEBUG=bindings LD_PRELOAD="$lib" "$@" >/dev/null 2>"$out/$name.bindings" || true
  local ours theirs
  ours=$(grep -c "libmemloc.so.*normal symbol \`malloc'" "$out/$name.bindings" || true)
  theirs=$(grep "to /[^ ]* \[[0-9]*\]: normal symbol \`malloc'" "$out/$name.bindings" |
    grep -vc libmemloc.so || true)
  if [ "$ours" -eq 0 ] || [ "$theirs" -ne 0 ]; then
    echo "FAIL $name: malloc bindings to libmemloc.so: $ours, elsewhere: $theirs" >&2
    exit 1
  fi
  echo "  bindings: $ours malloc binding(s), all to libmemloc.so"
}

# compare <name> <command...>: run with glibc and with MemLoc, diff the output.
compare() {
  local name=$1; shift
  local t0 t1 t2
  t0=$(now_ms)
  "$@" >"$out/$name.glibc"
  t1=$(now_ms)
  LD_PRELOAD="$lib" "$@" >"$out/$name.memloc"
  t2=$(now_ms)
  if ! diff -u "$out/$name.glibc" "$out/$name.memloc"; then
    echo "FAIL $name: output differs under MemLoc" >&2
    exit 1
  fi
  echo "  output: identical ($(wc -l <"$out/$name.memloc") lines)"
  echo "  wall time: glibc $((t1 - t0)) ms, memloc $((t2 - t1)) ms"
}

ran=0
if command -v sqlite3 >/dev/null; then
  echo "sqlite3 $(sqlite3 --version | cut -d' ' -f1)"
  check_bindings sqlite3 sqlite3 :memory: "select 1;"
  compare sqlite3 sqlite3 :memory: -init /dev/null ".read $here/demo/workload.sql"
  ran=$((ran + 1))
else
  echo "sqlite3 not installed: skipped"
fi

if command -v python3 >/dev/null; then
  echo "$(python3 --version)"
  check_bindings python3 python3 -c "pass"
  compare python3 python3 "$here/demo/workload.py"
  # Python serves objects <= 512 B from its own arenas (pymalloc). This run
  # sends every object allocation through malloc, and so through MemLoc.
  echo "python3 with PYTHONMALLOC=malloc"
  compare python3-raw env PYTHONMALLOC=malloc python3 "$here/demo/workload.py"
  ran=$((ran + 1))
else
  echo "python3 not installed: skipped"
fi

[ "$ran" -gt 0 ] || exit 77
echo "OK: $ran program(s) ran on MemLoc with identical output"
