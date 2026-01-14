#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root_dir}/build"

PYTHON_BIN="${PYTHON:-python}"
if ! command -v "${PYTHON_BIN}" >/dev/null 2>&1; then
  PYTHON_BIN="python3"
fi

pybind11_dir="$(
"${PYTHON_BIN}" - <<'PY'
try:
    import pybind11
    print(pybind11.get_cmake_dir())
except Exception:
    pass
PY
)"

if [[ -z "${pybind11_dir}" ]]; then
  echo "pybind11 CMake config not found. Install pybind11 for your Python interpreter."
  echo "Example: python3 -m pip install pybind11"
  exit 1
fi

cmake -S "${root_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLOBSIM_BUILD_PYTHON=ON \
  -Dpybind11_DIR="${pybind11_dir}"
cmake --build "${build_dir}"
