#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root_dir}/build_hft"

if [ -f "${build_dir}/CMakeCache.txt" ]; then
  if grep -q "CMAKE_GENERATOR:INTERNAL=Unix Makefiles" "${build_dir}/CMakeCache.txt"; then
    echo "Cleaning build_hft (generator mismatch: Unix Makefiles -> Ninja)"
    rm -rf "${build_dir}"
  fi
fi

cmake -S "${root_dir}" -B "${build_dir}" -G Ninja \
  -DLOBSIM_BUILD_TESTS=OFF \
  -DLOBSIM_BUILD_PYTHON=OFF

cmake --build "${build_dir}" --target lobsim_udp_engine --target lobsim_event_generator --target lobsim_memory_replay

echo "Built:"
echo "  ${build_dir}/lobsim_udp_engine"
echo "  ${build_dir}/lobsim_event_generator"
echo "  ${build_dir}/lobsim_memory_replay"
