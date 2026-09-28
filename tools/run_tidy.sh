#!/usr/bin/env bash
# Run clang-tidy over the allocator sources using a build's compile_commands.json.
# Usage: tools/run_tidy.sh <build-dir>
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD_DIR="${1:?usage: run_tidy.sh <build-dir>}"
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
mapfile -t files < <(find src -name '*.cpp' | sort)
"$CLANG_TIDY" -p "$BUILD_DIR" --quiet "${files[@]}"
echo "clang-tidy OK (${#files[@]} files, $("$CLANG_TIDY" --version | grep -m1 version))"
