#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root_dir}/build_hft"

RATE=${RATE:-10000}
DURATION=${DURATION:-5}
SYMBOLS=${SYMBOLS:-10}
SEED=${SEED:-1}
CANCEL_PCT=${CANCEL_PCT:-0.30}
MARKET_PCT=${MARKET_PCT:-0.05}
CROSS_PCT=${CROSS_PCT:-0.02}
MAX_EVENTS=${MAX_EVENTS:-10000000}
MAX_MEM_MB=${MAX_MEM_MB:-2048}
HEADER=${HEADER:-0}
METRICS=${METRICS:-1}

BIN="${build_dir}/lobsim_memory_replay"
if [ ! -x "${BIN}" ]; then
  echo "Binary not found. Run ./scripts/hft_build.sh first." >&2
  exit 1
fi

echo "Running in-memory stress test:"
echo "  rate=${RATE} duration=${DURATION} symbols=${SYMBOLS} max_events=${MAX_EVENTS} max_mem_mb=${MAX_MEM_MB}"

args=(
  --rate "${RATE}"
  --duration "${DURATION}"
  --symbols "${SYMBOLS}"
  --cancel-pct "${CANCEL_PCT}"
  --market-pct "${MARKET_PCT}"
  --cross-pct "${CROSS_PCT}"
  --seed "${SEED}"
  --max-events "${MAX_EVENTS}"
  --max-mem-mb "${MAX_MEM_MB}"
)

if [ "${HEADER}" != "0" ]; then
  args+=(--header)
fi

METRICS="${METRICS}" "${BIN}" "${args[@]}"
