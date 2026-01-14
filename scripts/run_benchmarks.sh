#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root_dir}/build"
data_path="${1:-${root_dir}/sample_data/coinapi_coinbase_btcusdt_sample.parquet}"
max_events="${2:-0}"

cmake -S "${root_dir}" -B "${build_dir}" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "${build_dir}" --target lobsim_bench_cpp

cpp_args=(--path "${data_path}")
if [[ "${max_events}" != "0" ]]; then
  cpp_args+=(--max-events "${max_events}")
fi

echo "== C++ benchmark =="
"${build_dir}/lobsim_bench_cpp" "${cpp_args[@]}"

PYTHON_BIN="${PYTHON:-python}"
if ! command -v "${PYTHON_BIN}" >/dev/null 2>&1; then
  PYTHON_BIN="python3"
fi

export PYTHONPATH="${root_dir}/python${PYTHONPATH:+:${PYTHONPATH}}"

py_args=(--path "${data_path}")
if [[ "${max_events}" != "0" ]]; then
  py_args+=(--max-events "${max_events}")
fi

echo "== Python benchmark =="
"${PYTHON_BIN}" "${root_dir}/examples/lobsim_bench_py.py" "${py_args[@]}"
