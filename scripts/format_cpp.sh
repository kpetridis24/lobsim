#!/usr/bin/env bash
set -euo pipefail

files=()
paths=(cpp/include cpp/src cpp/tests)
if command -v rg >/dev/null 2>&1; then
  while IFS= read -r file; do
    files+=("${file}")
  done < <(rg --files -g '*.h' -g '*.hpp' -g '*.c' -g '*.cc' -g '*.cpp' -g '*.ipp' "${paths[@]}")
else
  while IFS= read -r -d '' file; do
    files+=("${file}")
  done < <(find "${paths[@]}" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.ipp' \) -print0)
fi
if [[ ${#files[@]} -eq 0 ]]; then
  echo "No C/C++ source files found."
  exit 0
fi

clang-format -i "${files[@]}"
