#!/usr/bin/env bash
set -euo pipefail

# Generate a "top CPU functions" report with gprof for lobsim_udp_engine.
# Notes:
# - Uses -pg instrumentation, which changes performance. Use for hotspots only.
# - Multi-threaded gprof is approximate.
#
# Usage:
#   RATE=200000 DURATION=10 SYMBOLS=25 SEED=1 ./scripts/profile_hft_gprof.sh
#
# Output:
#   benchmark/profiles/gprof_<timestamp>.txt

RATE=${RATE:-200000}
DURATION=${DURATION:-10}
SYMBOLS=${SYMBOLS:-25}
CANCEL_PCT=${CANCEL_PCT:-0.30}
MARKET_PCT=${MARKET_PCT:-0.05}
CROSS_PCT=${CROSS_PCT:-0.02}
SEED=${SEED:-1}
ENGINE_DURATION_SEC=${ENGINE_DURATION_SEC:-}

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
profiles_dir="${root_dir}/benchmark/profiles"

mkdir -p "${profiles_dir}"

if [ -z "${ENGINE_DURATION_SEC}" ]; then
  ENGINE_DURATION_SEC=$(python3 - <<PY
import math
print(int(math.ceil(float("${DURATION}") + 2.0)))
PY
)
fi

image_name="lobsim-hft"
if ! docker image inspect "${image_name}" >/dev/null 2>&1; then
  docker build -t "${image_name}" "${root_dir}"
fi

docker run --rm \
  -e RATE="${RATE}" \
  -e DURATION="${DURATION}" \
  -e SYMBOLS="${SYMBOLS}" \
  -e CANCEL_PCT="${CANCEL_PCT}" \
  -e MARKET_PCT="${MARKET_PCT}" \
  -e CROSS_PCT="${CROSS_PCT}" \
  -e SEED="${SEED}" \
  -e ENGINE_DURATION_SEC="${ENGINE_DURATION_SEC}" \
  -v "${root_dir}:/workspace" \
  -w /workspace \
  --entrypoint /bin/bash \
  "${image_name}" -lc '
    set -euo pipefail

    build_dir=/tmp/lobsim_gprof_build
    run_dir=/tmp/gprof_run

    cmake -S /workspace -B "${build_dir}" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-O2 -g -pg" \
      -DCMAKE_EXE_LINKER_FLAGS="-pg" \
      -DLOBSIM_BUILD_TESTS=OFF \
      -DLOBSIM_BUILD_PYTHON=OFF
    cmake --build "${build_dir}" --target lobsim_udp_engine --target lobsim_event_generator

    rm -rf "${run_dir}"
    mkdir -p "${run_dir}"

    (cd "${run_dir}" && METRICS=0 ENGINE_DURATION_SEC="${ENGINE_DURATION_SEC}" \
      "${build_dir}/lobsim_udp_engine" > /dev/null 2>/tmp/engine_err.txt) &
    ENGINE_PID=$!
    sleep 0.2

    "${build_dir}/lobsim_event_generator" \
      --host 127.0.0.1 \
      --port 1234 \
      --rate "${RATE}" \
      --duration "${DURATION}" \
      --symbols "${SYMBOLS}" \
      --cancel-pct "${CANCEL_PCT}" \
      --market-pct "${MARKET_PCT}" \
      --cross-pct "${CROSS_PCT}" \
      --seed "${SEED}" \
      --stats-out /tmp/gen_stats.json >/dev/null

    wait ${ENGINE_PID} >/dev/null 2>&1 || true

    ts=$(date +%Y%m%d_%H%M%S)
    out="/workspace/benchmark/profiles/gprof_${ts}.txt"
    gmon="${run_dir}/gmon.out"
    if [ ! -f "${gmon}" ]; then
      echo "gmon.out not found at ${gmon}. Engine may not have exited cleanly." >&2
      exit 1
    fi

    gprof "${build_dir}/lobsim_udp_engine" "${gmon}" > "${out}"
    echo "${out}"
  '
