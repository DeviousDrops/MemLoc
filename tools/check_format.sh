#!/usr/bin/env bash
# Verify (or with --fix, apply) clang-format on every tracked C/C++ file.
set -euo pipefail
cd "$(dirname "$0")/.."
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
mapfile -t files < <(find src include tests bench -name '*.c' -o -name '*.cpp' -o -name '*.h' | sort)
[[ ${#files[@]} -gt 0 ]] || { echo 'no sources found' >&2; exit 1; }
if [[ "${1:-}" == "--fix" ]]; then
  "$CLANG_FORMAT" -i "${files[@]}"
else
  "$CLANG_FORMAT" --dry-run -Werror "${files[@]}"
  echo "format OK (${#files[@]} files, $("$CLANG_FORMAT" --version))"
fi
