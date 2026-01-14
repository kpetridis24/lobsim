# lobsim — Limit Order Book Simulator

[![CI](https://github.com/kpetridis24/lobsim/actions/workflows/ci.yml/badge.svg)](https://github.com/kpetridis24/lobsim/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

`lobsim` is a fast, deterministic **L3 limit order book replay + paper execution simulator** for market microstructure research and strategy prototyping.

It consumes an event stream (historical and/or strategy-injected), maintains a **per-order (L3)** book state, and emits facts (fills + diagnostics) via a pluggable sink interface.
The core is written in **C++20** for performance, with **Python bindings** for research workflows.

## What you get
- **L3 engine**: applies `ADD`, `DELETE`, `SUBTRACT`, `MATCH`, `SET` via `NormalizedLobEvent`.
- **Paper execution**: strategy events (`UpdateSource::STRATEGY`) can cross and/or rest and generate fills.
- **Structured output**: maker/taker fills (`FillRecord`) + event application stream (`EventApplyRecord`) + diagnostics (`DiagnosticRecord`).
- **Book observation APIs**: best price, depth-at-price, L2 top-N ladders.
- **Replay utilities**: `ReplaySession` to step events and enforce monotonic time assumptions.
- **Parity tooling**: script to compare Python vs C++ fill streams on the same sample data.

## Installation / prerequisites (developer build)
This repo is built with CMake and ships Python bindings via `pybind11`.

**Dependencies**
- C++: CMake, Ninja, a C++20 compiler
- Arrow/Parquet (for the provided parquet sources/examples)
- Python: 3.11+ (bindings), `pybind11` (CMake package)

**Sample data is stored in Git LFS**
```bash
git lfs install
git lfs pull
```

## Quickstart
### C++ build + tests
```bash
./scripts/make_cpp.sh
```

### Build Python bindings + install editable package
```bash
./scripts/make_all.sh
python -m pip install -e python
python -c "import lobsim; from lobsim.engine import PaperTradingSimulator; print('import ok')"
```

## Examples
### CoinAPI BTC/USDT parquet replay (C++)
```bash
./scripts/run_cpp_example.sh sample_data/coinapi_coinbase_btcusdt_sample.parquet
```

### LOBSTER AMZN message+orderbook replay (Python)
```bash
PYTHONPATH=python python examples/lobsim_lobster_py.py \
  sample_data/AMZN_2012-06-21_34200000_57600000_message_10.csv \
  sample_data/AMZN_2012-06-21_34200000_57600000_orderbook_10.csv
```

### Compare fills: C++ vs Python (same dataset)
Runs both implementations and diffs the full fill stream (all fields).
```bash
./scripts/compare_cpp_python_fills.sh sample_data/coinapi_coinbase_btcusdt_sample.parquet
```

## Benchmarks
Two benchmark entry points are provided:
- `examples/lobsim_bench_cpp.cpp` (C++)
- `examples/lobsim_bench_py.py` (Python)

Run both:
```bash
./scripts/run_benchmarks.sh sample_data/coinapi_coinbase_btcusdt_sample.parquet
```

Notes:
- Python defaults to **no sink attached** (so it doesn’t store millions of fills in RAM). Use `--with-sink` only for small runs.
- C++ uses a lightweight counting sink by default (counts fills/events/diagnostics without storing them).

## Data notes (LOBSTER)
LOBSTER provides:
- a **message** file (event tape) and
- an **orderbook** file (L2 snapshots after each message row),
aligned row-by-row.

Important limitation: you cannot always reconstruct a perfect full L3 order list from an arbitrary window, because the book at the start of the window may contain orders created before the window.
In those cases, you may see a small drift vs the provided L2 snapshots when the message file references order IDs that were never submitted within the window.

## Repository layout
- `cpp/include/lobsim/` — C++ public headers
- `cpp/src/` — C++ implementation
- `cpp/tests/` — C++ unit tests (Catch2)
- `python/lobsim/` — Python package wrapper + stubs for autocomplete
- `examples/` — end-to-end examples and benchmarks
- `scripts/` — developer workflows

## License
Apache 2.0. See `LICENSE`.
