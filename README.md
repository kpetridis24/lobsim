# LOBSIM - Limit Order Book Simulator 
LOBSIM is a fast, deterministic **L3 limit order book replay + paper execution 
engine** designed for market microstructure research and strategy prototyping.

It consumes an event stream (historical or synthetic/strategy), maintains a 
full **per-order (L3)** book state, and emits facts (fills and other execution signals) 
via a pluggable logging interface. The core is written in **C++20** for performance and 
determinism, with **Python bindings** for research workflows.

## What it does
- **Replay L3 market data** event-by-event (or in batches) into a consistent order book state.
- Hadle common L3 updates: `ADD`, `DELETE`, `SUBTRACT`, `MATCH`, `SET`.
- Simulate **crossing/walking** when the feed provides marketable orders.
- **Emit structured fills** (maker/taker metadata, timestamps, price/qty) through a sink interface.
- Provide **book observation APIs** (best prices, depth at price, top-of-book ladders, bounded L3 views) for signal extraction and ML feature building.
- Support **both historical** and **strategy** event sources (`UpdateSource::HISTORICAL` / `UpdateSource::STRATEGY`) so researchers can inject orders and analyze fills.

## Typical usage
- Replay historical events into the engine and inspect book state.
- Inject strategy orders (as events tagged `UpdateSource::STRATEGY`) from Python/C++
- Analyze fills and execution quality from the emitted logs
