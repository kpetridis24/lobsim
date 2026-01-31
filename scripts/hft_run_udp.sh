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
ENGINE_DURATION_SEC=${ENGINE_DURATION_SEC:-}

if [ -z "${ENGINE_DURATION_SEC}" ]; then
  ENGINE_DURATION_SEC=$((DURATION + 2))
fi

ENGINE_BIN="${build_dir}/lobsim_udp_engine"
GEN_BIN="${build_dir}/lobsim_event_generator"

if [ ! -x "${ENGINE_BIN}" ] || [ ! -x "${GEN_BIN}" ]; then
  echo "Binaries not found. Run ./scripts/hft_build.sh first." >&2
  exit 1
fi

METRICS=${METRICS:-1}

echo "Running UDP stress test:"
echo "  rate=${RATE} duration=${DURATION} symbols=${SYMBOLS}"

ENGINE_DURATION_SEC="${ENGINE_DURATION_SEC}" METRICS="${METRICS}" "${ENGINE_BIN}" &
ENGINE_PID=$!
sleep 0.2

"${GEN_BIN}" \
  --host 127.0.0.1 \
  --port 1234 \
  --rate "${RATE}" \
  --duration "${DURATION}" \
  --symbols "${SYMBOLS}" \
  --cancel-pct "${CANCEL_PCT}" \
  --market-pct "${MARKET_PCT}" \
  --cross-pct "${CROSS_PCT}" \
  --seed "${SEED}"

wait ${ENGINE_PID} || true
