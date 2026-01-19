#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root_dir}/build"
data_path="${1:-${root_dir}/sample_data/coinbase_btcusdt_sample.parquet}"

cmake -S "${root_dir}" -B "${build_dir}" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "${build_dir}" --target lobsim_cpp_example

"${build_dir}/lobsim_cpp_example" "${data_path}"
