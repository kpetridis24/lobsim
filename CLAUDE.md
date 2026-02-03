# CLAUDE.md - LOBSIM Project Context

This file provides context for AI assistants working on the LOBSIM codebase.

## Project Overview

LOBSIM is a high-performance **L3 (per-order) limit order book simulator** for market microstructure research and strategy backtesting. It's written in C++20 with Python bindings via pybind11.

**Key capabilities:**
- Deterministic event-by-event replay of historical order book data
- Paper trading simulation with queue position tracking
- Multi-book orchestration with event merging by timestamp
- Fill generation with maker/taker attribution

## Architecture

### Core Components

```
NormalizedLobEvent → PaperTradingSimulator → ILogSink
                            ↓
                    [Historical Book] + [Paper Book]
                            ↓
                    FillRecord, EventApplyRecord, DiagnosticRecord
```

**PaperTradingSimulator** (`cpp/include/lobsim/paper_trading_simulator.hpp`)
- Single-book matching engine
- Maintains separate historical and paper order books
- Uses `NodePool<T>` for memory-efficient order management
- Tracks paper order queue position via Fenwick trees

**MultiBookSimulator** (`cpp/include/lobsim/multibook_simulator.hpp`)
- Composes multiple `PaperTradingSimulator` instances
- Merges events by `ts_received` using a min-heap
- Supports strategy event injection with latency simulation

### Data Structures

| Structure | Location | Purpose |
|-----------|----------|---------|
| `NodePool<T>` | `paper_trading_simulator.hpp` | Object pooling with free list |
| `FlatHashMap` | `flat_hash_map.hpp` | ska::flat_hash_map wrapper for O(1) lookups |
| `OrderPriorityQueue` | `paper_trading_simulator.hpp` | boost::intrusive_list for FIFO order queue |
| `FenwickTree` | `paper_trading_simulator.hpp` | Binary indexed tree for queue position |

### Event Types

| UpdateType | Meaning | Mutates Historical? | Mutates Paper? |
|------------|---------|---------------------|----------------|
| `ADD` | New order | Yes (rests or matches) | Yes |
| `DELETE` | Full cancel | Yes | Yes |
| `SUBTRACT` | Partial reduce | Yes | Yes |
| `SET` | Overwrite qty | Yes | Yes |
| `MATCH` | Passive fill | Yes | N/A |
| `AGGRESSIVE_TRADE` | Market order (strategy only) | Read-only | Yes |

### Paper vs Historical Orders

- **Historical orders** mutate the book and can fill paper orders
- **Paper (strategy) orders** match against historical liquidity read-only, then rest
- Queue position matters: Fenwick trees track position for accurate fill attribution
- A paper order is filled when historical orders ahead of it are consumed

## Code Conventions

### C++ Style
- **LLVM-based** with modifications (see `.clang-format`)
- **4-space indentation**, 120-character line limit
- **Naming**: `snake_case` for variables/functions, `PascalCase` for types
- **Braces**: same line for functions, next line for classes/namespaces

### Performance Patterns
- Use `LOBSIM_FORCEINLINE` for hot-path functions
- Prefer `FlatHashMap` over `std::unordered_map`
- Use `NodePool<T>` for frequently allocated/deallocated objects
- Prefer `boost::intrusive::list` over `std::list` for order queues
- Avoid allocations in the event processing hot path

### Template Patterns
- Heavy use of CRTP for static polymorphism
- Concepts for compile-time interface checking (C++20)
- `constexpr` where possible for compile-time evaluation

### Python Bindings
- Wrappers in `python/lobsim/` around `_core` module
- Python 3.11+ type hints required
- Use `ruff` for formatting

## Code Review Focus

### Priority Order
1. **Correctness** - Logic errors, edge cases, off-by-one errors
2. **Safety** - Buffer overflows, integer overflow, null pointer dereference
3. **Performance** - Hot path allocations, cache misses, unnecessary copies
4. **Maintainability** - Clear naming, appropriate comments, testability
5. **Style** - Consistency with existing code

### Common Issues to Watch For

**Memory Management:**
- Pool exhaustion (check `HISTORICAL_ORDER_POOL_EXHAUSTED` / `PAPER_ORDER_POOL_EXHAUSTED`)
- Use-after-release of pooled nodes
- Intrusive list nodes still linked when released

**Order Book Logic:**
- Side confusion (BUY vs SELL)
- Price comparison direction (bids descending, asks ascending)
- Queue position calculation errors
- Missing fills when historical order is consumed

**Event Processing:**
- Non-monotonic `ts_received` handling
- Missing diagnostic emission on error paths
- Incorrect sequence number advancement

**Python Bindings:**
- Type conversion errors (int64 overflow, string encoding)
- GIL management in callbacks
- Memory ownership across language boundary

## Testing

### C++ Tests
```bash
./scripts/make_cpp.sh  # Build with tests
ctest --test-dir build --output-on-failure
```

### Python Tests
```bash
python -m unittest discover -s python/tests -v
```

### Cross-Language Validation
```bash
./scripts/compare_cpp_python_fills.sh  # Ensures C++ and Python produce identical fills
```

## File Structure

```
cpp/
├── include/lobsim/     # Public headers (API)
│   ├── paper_trading_simulator.hpp  # Core engine
│   ├── multibook_simulator.hpp      # Multi-book orchestration
│   ├── lob_event.hpp                # Event schema
│   ├── log_sink.hpp                 # Sink interface
│   └── ...
├── src/                # Implementation
│   ├── paper_trading_simulator.cpp  # ~1300 lines, main logic
│   ├── in_memory_sink.cpp           # Sink implementation
│   └── python_bindings.cpp          # pybind11 bindings
└── tests/              # Catch2 unit tests

python/
├── lobsim/             # Python package
│   ├── engine.py       # PaperTradingSimulator wrapper
│   ├── sink.py         # Sink wrappers
│   ├── types.py        # Enum re-exports
│   └── _core.pyi       # Type stubs
└── tests/              # Python unit tests
```

## Build Commands

```bash
# C++ only
./scripts/make_cpp.sh

# C++ + Python bindings
./scripts/make_all.sh
pip install -e python

# Run benchmarks
lobsim bench
```

## Key Invariants

1. `seq` monotonically increases with each event processed
2. `order_arrival_seq` monotonically increases with each order added
3. An order exists in exactly one of: historical book, paper book, or neither (deleted)
4. Pool size equals active nodes plus free list size
5. Sum of Fenwick tree equals sum of individual quantities at that level
