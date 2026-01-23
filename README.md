<p align="center">
  <img src="assets/lobsim-logo.png" width="300" alt="LOBSIM logo">
</p>

# LOBSIM — Limit Order Book Simulator

[![CI](https://github.com/kpetridis24/lobsim/actions/workflows/ci.yml/badge.svg)](https://github.com/kpetridis24/lobsim/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

## Table of contents
- [Why use it](#why-use-it)
- [Core concepts](#core-concepts)
- [Event stream requirements (L3) + handling missing / NaN order IDs](#event-stream-requirements-l3--handling-missing--nan-order-ids)
- [Installation / prerequisites (developer build)](#installation--prerequisites-developer-build)
- [Quickstart](#quickstart)
- [Examples](#examples)
- [Benchmarks](#benchmarks)
- [Repository layout](#repository-layout)
- [License](#license)

`lobsim` is a fast, deterministic **L3 limit order book replay + paper execution simulator** for market microstructure research and strategy prototyping.

It consumes an event stream (historical and/or strategy-injected), maintains a **strict per-order (L3)** book state (requires stable `order_id`s for historical events), and emits facts (fills + diagnostics) via a pluggable sink interface.
The core is written in **C++20** for performance, with **Python bindings** for research workflows.

## Why use it
- **Research faster**: replay event-by-event, inspect the book, inject your own strategy orders, and analyze fills in one loop.
- **Model-in-the-loop**: compute signals or run ML inference on live book views during replay (without breaking event ordering).
- **Stay reproducible**: deterministic engine + structured outputs make results debuggable and comparable across runs.
- **Works in Python and C++**: run research in notebooks and deploy the same mechanics in native code.
- **Measure execution**: fills include maker/taker metadata and timestamps, so you can evaluate queueing and fill quality.
- **Lightning fast**: the core is C++20; Python bindings keep research workflows responsive even on large event streams.
- **Built-in observability**: structured fills, event-application records, and diagnostics make it easy to debug, audit, and understand your event stream.

## Core concepts

### `PaperTradingSimulator` (single-book engine)
`PaperTradingSimulator` is the core state machine for **one** order book. You feed it `NormalizedLobEvent`s via `update(...)`, and query the book state at any time.

Minimal usage (Python):
```python
from lobsim.engine import PaperTradingSimulator
from lobsim.sink import InMemoryLogSink

engine = PaperTradingSimulator()
sink = InMemoryLogSink()
engine.set_log_sink(sink)  # enables fills + diagnostics + event-apply records
```

Query examples:
```python
from lobsim.types import Side

best_bid = engine.get_best_price_ticks(Side.BUY)
best_ask = engine.get_best_price_ticks(Side.SELL)
top10_bids = engine.l2_top_n(Side.BUY, 10)  # [(price_ticks, qty_lots), ...]
```

### `NormalizedLobEvent` (canonical event schema)
`NormalizedLobEvent` is the “input language” of the engine.

Important fields / conventions:
- `ts_received`, `ts_exchange`: integer timestamps in **microseconds** (you decide the epoch; just stay consistent).
- `price_ticks`, `quantity_lots`: integers on a fixed grid (defined by your `tick_size` / `lot_size` in the adapter).
- `order_id`: stable identifier for a **single** historical L3 order (or a synthetic per-level ID if you intentionally normalize an L2 feed).
- `symbol_id`: book identifier (used by `MultiBookSimulator`; for single-book it can be any string).
- `update_source`: whether the event is `HISTORICAL` (market feed) or `STRATEGY` (your paper/strategy actions).

Minimal construction (Python):
```python
from lobsim.lob_event import NormalizedLobEvent
from lobsim.types import Side, UpdateSource, UpdateType, UnknownAggressorIdSentinel, UnknownTraderIdSentinel

ev = NormalizedLobEvent(
    ts_exchange=0,
    ts_received=123_456,
    side=Side.BUY,
    update_type=UpdateType.ADD,
    price_ticks=9_355_000,
    quantity_lots=100_000,
    order_id=1,
    trader_id=UnknownTraderIdSentinel,
    aggressor_id=UnknownAggressorIdSentinel,
    update_source=UpdateSource.HISTORICAL,
    symbol_id="coinbase:BTC-USDT",
)
engine.update(ev)
```

### What each `UpdateType` means (and when to use it)
`lobsim` is fundamentally **L3** (per-order). The engine assumes that historical updates refer to an existing order object via `order_id` (unless you intentionally normalize an L2 feed into synthetic per-level “orders”).

#### `ADD`
Creates a new order with a unique `order_id` and initial `quantity_lots` at `price_ticks`.
- **Historical `ADD`**: represents a new resting market order.
- **Strategy `ADD`**: represents a paper limit order. If the price is marketable, it will generate immediate taker fills; any remaining quantity becomes a resting **paper** order (it does not alter historical liquidity).

#### `DELETE`
Cancels an existing order (`order_id`) by setting its remaining quantity to zero.
- Use this for full cancels. (`quantity_lots` is typically `0`.)

#### `SUBTRACT`
Reduces the remaining quantity of an existing order (`order_id`) by `quantity_lots`.
- Use for partial cancels / partial reductions.
- `quantity_lots < 0` is invalid.

#### `SET`
Overwrites the remaining quantity of an existing order (`order_id`) to `quantity_lots`.
- Use this if your feed explicitly encodes “set remaining size to X”.
- This is also the typical normalization target when you treat a missing-ID feed as L2 and maintain “total level size”.

#### `MATCH`
Represents a trade that removes liquidity from a **passive** existing order (`order_id`) by `quantity_lots`.
- Use this for feeds that emit passive-side trade events directly (instead of encoding trades as marketable `ADD`s).

#### `AGGRESSIVE_TRADE` (strategy-only)
A strategy “market-style” order that consumes the current opposite book immediately (no need to know the exact crossing price).
- It does **not** rest any remainder.
- The historical book is not mutated; fills are emitted as if you traded against that liquidity.
- In the current implementation, `price_ticks` is treated as metadata (the engine uses the current best levels).

### Observability: `ILogSink` / `InMemoryLogSink`
Sinks receive the facts emitted by the engine:
- `FillRecord`: executed trades (maker/taker, qty/price, timestamps, sources).
- `EventApplyRecord`: what the engine attempted to apply (useful for audit + debugging).
- `DiagnosticRecord`: structured warnings/errors with event context (e.g., “DELETE non-existing order_id”).

Attach a sink (Python):
```python
from lobsim.engine import PaperTradingSimulator
from lobsim.sink import InMemoryLogSink

engine = PaperTradingSimulator()
sink = InMemoryLogSink()
engine.set_log_sink(sink)

fills = sink.get_fills()
events = sink.get_events()
diagnostics = sink.get_diagnostics()
```

### Multi-book: `BookId`, `MultiBookSimulator`, `InMemoryMultiLogSink`
`MultiBookSimulator` composes many `PaperTradingSimulator` instances and:
- merges the next event across all registered streams by `ts_received` (no look-ahead),
- maintains a single “current time”,
- lets you inject strategy events per book with optional latency.

Minimal usage (Python):
```python
from lobsim import BookId
from lobsim.multibook import Config, MultiBookSimulator
from lobsim.sink import InMemoryMultiLogSink

cfg = Config(require_monotonic_ts_received=True, fail_fast=True)
sim = MultiBookSimulator(cfg)

book = BookId("coinbase", "BTC-USDT")
sim.add_book(book)

sink = InMemoryMultiLogSink()
sim.set_multi_log_sink(sink)
```

Register a stream:
- `add_stream(book_id, source, adapter)`: `source` is any Python iterator; `adapter.normalize(raw)` must return a `NormalizedLobEvent`.
- `add_stream(book_id, normalized_source, adapter=None)`: if the source already yields `NormalizedLobEvent`s.

Step and inject strategy events:
```python
while sim.step():
    pass

# Inject a market-style strategy order (example)
from lobsim.lob_event import NormalizedLobEvent
from lobsim.types import Side, UpdateType, UpdateSource, UnknownAggressorIdSentinel, UnknownTraderIdSentinel

sim.submit_strategy_event(
    book,
    NormalizedLobEvent(
        ts_exchange=0,
        ts_received=0,  # 0 means “relative to current time” in MultiBookSimulator
        side=Side.BUY,
        update_type=UpdateType.AGGRESSIVE_TRADE,
        price_ticks=0,
        quantity_lots=100_000,
        order_id=123,
        trader_id=UnknownTraderIdSentinel,
        aggressor_id=UnknownAggressorIdSentinel,
        update_source=UpdateSource.STRATEGY,
        symbol_id="",  # will be filled to the book_key
    ),
    latency=1_000,  # microseconds
)
```

## Event stream requirements (L3) + handling missing / NaN order IDs

`lobsim` is an **L3 (per-order)** simulator. That means the engine assumes that **every historical order update refers to a concrete order object** via a stable `order_id`.

### ✅ L3 assumption (default)
For historical events, the engine expects:

- `order_id` is present and stable for the lifetime of the order
- `ADD(order_id)` is unique (no duplicate `ADD` for the same live order)
- `SET / SUBTRACT / DELETE / MATCH` refer to an existing `order_id`
- FIFO queueing at a price level is preserved using order arrival order

If your feed satisfies this, you get full L3 behavior: queue priority, maker/taker attribution, and order lifecycle tracking.

---

### ⚠️ Missing / NaN order IDs (L2-style feeds)
Some event streams provide **no order IDs** (e.g., `order_id = NaN/null`), or provide them only partially. In these cases, the feed is effectively **L2 (price-level)**, and it is **not possible** to reconstruct true FIFO queue priority.

`lobsim` intentionally does **not** guess semantics inside the engine.
Instead, **the data source / adapter must normalize** such feeds into valid L3-like events.

#### Recommended normalization for missing order IDs (treat as L2)
If a **substantial majority** of events have missing IDs, treat the stream as **pure L2**:

1) **Ignore all order IDs from the feed** (treat them as missing)
2) Maintain a level state: `level_qty[(symbol, side, price_ticks)] -> qty_lots`
3) Emit a deterministic synthetic `order_id` per price level:

```python
levelId = f(symbol, side, price_ticks) # must be collision-safe
```


4) Convert incoming updates into `ADD/SET/DELETE` on the synthetic levelId:

- First time a level appears with qty > 0:
  - emit `ADD(levelId, price_ticks, qty_lots)`
- If the level already exists:
  - emit `SET(levelId, price_ticks, newQtyLots)`  
- If `newQtyLots == 0`:
  - emit `DELETE(levelId, price_ticks, 0)`

This produces stable and realistic **top-of-book + depth evolution** while keeping the engine API unchanged.

> Important: This mode preserves **L2 depth correctness**, but it does **not** represent true L3 queue position within a level (because the feed does not contain it).

---

### Mixed feeds (some real order IDs, some missing IDs)
If your feed contains both:
- real per-order updates (true L3 IDs), and
- occasional NaN/missing IDs at the same price levels,

you have two valid choices:

**(A) Strict L3 integrity (recommended for correctness):**  
Drop/diagnose NaN updates, and replay only true L3 events.

**(B) Hybrid (recommended for best book continuity):**  
Treat NaN updates as L2 and apply them to synthetic `levelId` orders, *separate from real L3 orders*.  
Make sure the synthetic ID namespace **cannot collide** with real order IDs.

---

### Why this is handled in the source (not the engine)
A missing `order_id` does not have a single universal meaning. Depending on the venue/data vendor, it can represent:
- L2 price-level deltas
- aggregated snapshots
- feed corruption / partial packet loss
- numeric precision loss (e.g., large IDs stored as floating point)

Because the correct interpretation depends on the feed, `lobsim` keeps the core engine **strict and deterministic**, and delegates normalization to adapters/sources.



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
### Streamlit demos (recommended)
After building the Python bindings (see Quickstart), install demo deps and run:
```bash
python -m pip install streamlit plotly pyarrow

lobsim demo replay   # replay explorer
lobsim demo trend    # trend-following baseline
lobsim demo arb      # multi-book arbitrage monitor
```

If your shell can’t find the `lobsim` command, run the same via:
```bash
python -m lobsim.cli demo replay
python -m lobsim.cli demo trend
python -m lobsim.cli demo arb
python -m lobsim.cli example cpp
python -m lobsim.cli bench
```

### Example runners
```bash
lobsim example cpp
lobsim example py
```

### LOBSTER AMZN message+orderbook replay (Python)
```bash
PYTHONPATH=python python examples/lobsim_lobster_py.py \
  sample_data/AMZN_2012-06-21_34200000_57600000_message_10.csv \
  sample_data/AMZN_2012-06-21_34200000_57600000_orderbook_10.csv
```

## Benchmarks
Two benchmark entry points are provided:
- `benchmark/lobsim_bench_cpp.cpp` (C++)
- `benchmark/lobsim_bench_py.py` (Python)

Run both:
```bash
lobsim bench
```

Notes:
- Python defaults to **no sink attached** (so it doesn’t store millions of fills in RAM). Use `--with-sink` only for small runs.
- C++ uses a lightweight counting sink by default (counts fills/events/diagnostics without storing them).

## Repository layout
- `cpp/include/lobsim/` — C++ public headers
- `cpp/src/` — C++ implementation
- `cpp/tests/` — C++ unit tests (Catch2)
- `python/lobsim/` — Python package wrapper + stubs for autocomplete
- `examples/` — end-to-end examples and benchmarks
- `scripts/` — developer workflows

## License
Apache 2.0. See `LICENSE`.
