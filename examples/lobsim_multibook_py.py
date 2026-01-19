"""
Minimal multibook example in Python mirroring examples/lobsim_multibook_cpp.cpp.
Reads Coinbase BTC-USDT Parquet sample twice (spot/perp), replays in a MultiBookSimulator,
queries best prices/topN, and injects simple strategy orders on one book.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from decimal import ROUND_HALF_UP, Decimal
from pathlib import Path
from typing import Iterable, List, Optional

import pyarrow.parquet as pq

from lobsim import BookId
from lobsim.multibook import Config, MultiBookSimulator
from lobsim.sink import InMemoryMultiLogSink
from lobsim.types import (
    NoAggressorNeededSentinel,
    Side,
    UnknownAggressorIdSentinel,
    UnknownTraderIdSentinel,
    UpdateSource,
    UpdateType,
)
from lobsim.lob_event import NormalizedLobEvent


def parse_update_type(s: str) -> UpdateType:
    s = s.strip().upper()
    if s == "SNAPSHOT":
        return UpdateType.SET
    if s == "ADD":
        return UpdateType.ADD
    if s == "DELETE":
        return UpdateType.DELETE
    if s == "MATCH":
        return UpdateType.MATCH
    if s == "SUBTRACT":
        return UpdateType.SUBTRACT
    if s == "SET":
        return UpdateType.SET
    raise ValueError(f"unknown update_type: {s}")


def parse_time_us(v) -> int:
    # Feed times are already in microseconds since midnight (int64)
    if isinstance(v, int):
        return v
    if isinstance(v, str):
        v = v.strip()
        if ":" in v:
            hh, mm, rest = v.split(":")
            if "." in rest:
                ss, frac = rest.split(".")
            else:
                ss, frac = rest, "0"
            frac = (frac + "000000")[:6]
            return (
                int(hh) * 3600_000_000
                + int(mm) * 60_000_000
                + int(ss) * 1_000_000
                + int(frac)
            )
        try:
            return int(v)
        except ValueError:
            return int(float(v))
    if isinstance(v, float):
        return int(v)
    raise ValueError(f"unsupported time value: {type(v)}")


def to_ticks(x: float, step: float, *, strict: bool) -> int:
    dx = Decimal(str(x))
    ds = Decimal(str(step))
    y = dx / ds
    r = y.to_integral_value(rounding=ROUND_HALF_UP)
    if strict and y != r:
        raise ValueError(f"value {x} not on grid {step}")
    return int(r)


def stable_int64(s: str) -> int:
    h = hashlib.blake2b(s.encode("utf-8"), digest_size=8).digest()
    v = int.from_bytes(h, "little", signed=False)
    if v >= 2**63:
        v -= 2**64
    return v


@dataclass
class CoinbaseRaw:
    ts_exchange_us: int
    ts_received_us: int
    update_type: str
    is_buy: int
    entry_px: float
    entry_sx: float
    order_id: str


class CoinbaseParquetSource:
    def __init__(self, path: str | Path, batch_size: int = 512):
        self._pq = pq.ParquetFile(str(path))
        self._batch_size = batch_size
        schema_names = self._pq.schema_arrow.names
        recv_col = next((n for n in schema_names if n in ("time_received", "time_feed")), None)
        if recv_col is None:
            recv_candidates = [n for n in schema_names if "time" in n and "exchange" not in n]
            if not recv_candidates:
                raise ValueError("No received time column found")
            recv_col = recv_candidates[0]
        self._recv_col = recv_col
        self._cols = [
            "time_exchange",
            recv_col,
            "update_type",
            "is_buy",
            "entry_px",
            "entry_sx",
            "order_id",
        ]

    def iter_raw(self) -> Iterable[CoinbaseRaw]:
        for batch in self._pq.iter_batches(columns=self._cols, batch_size=self._batch_size):
            cols = {name: batch.column(i) for i, name in enumerate(self._cols)}
            n = batch.num_rows
            for i in range(n):
                yield CoinbaseRaw(
                    ts_exchange_us=parse_time_us(cols["time_exchange"][i].as_py()),
                    ts_received_us=parse_time_us(cols[self._recv_col][i].as_py()),
                    update_type=str(cols["update_type"][i].as_py()),
                    is_buy=int(cols["is_buy"][i].as_py()),
                    entry_px=float(cols["entry_px"][i].as_py()),
                    entry_sx=float(cols["entry_sx"][i].as_py()),
                    order_id=str(cols["order_id"][i].as_py()),
                )


def normalize_raw(raw: CoinbaseRaw, *, tick_size: float, lot_size: float, symbol: str) -> NormalizedLobEvent:
    ut = parse_update_type(raw.update_type)
    side = Side.BUY if raw.is_buy else Side.SELL
    price_ticks = to_ticks(raw.entry_px, tick_size, strict=True)
    qty_lots = to_ticks(raw.entry_sx, lot_size, strict=True)
    oid = stable_int64(raw.order_id)
    return NormalizedLobEvent(
        tsExchange=raw.ts_exchange_us,
        tsReceived=raw.ts_received_us,
        side=side,
        updateType=ut,
        priceTicks=price_ticks,
        quantityLots=qty_lots,
        orderId=oid,
        traderId=UnknownTraderIdSentinel,
        aggressorId=UnknownAggressorIdSentinel,
        updateSource=UpdateSource.HISTORICAL,
        symbolId=symbol,
    )


def load_normalized(path: str | Path, *, max_rows: Optional[int] = None) -> List[NormalizedLobEvent]:
    src = CoinbaseParquetSource(path)
    out: List[NormalizedLobEvent] = []
    tick = 0.01
    lot = 1e-8
    for i, raw in enumerate(src.iter_raw()):
        if max_rows is not None and i >= max_rows:
            break
        try:
            out.append(normalize_raw(raw, tick_size=tick, lot_size=lot, symbol="BTC-USDT"))
        except ValueError:
            # skip invalid rows in this demo
            continue
    return out


def print_levels(levels):
    return "[" + ", ".join(f"({px}, {qty})" for px, qty in levels) + "]"


def main():
    data_path = Path("sample_data/coinbase_btcusdt_sample.parquet")
    events_spot = load_normalized(data_path, max_rows=3000)
    events_perp = load_normalized(data_path, max_rows=3000)

    sim = MultiBookSimulator(Config(require_monotonic_ts_received=True, fail_fast=False))
    sink = InMemoryMultiLogSink()
    sim.set_multi_log_sink(sink)

    spot = BookId("coinbase", "BTC-USDT-spot")
    perp = BookId("coinbase", "BTC-USDT-perp")
    sim.add_book(spot)
    sim.add_book(perp)
    sim.add_normalized_stream(spot, events_spot)
    sim.add_normalized_stream(perp, events_perp)

    steps = 0
    max_steps = 2000
    while steps < max_steps and sim.step():
        steps += 1
        if steps % 500 == 0:
            t = sim.current_time()
            print(f"Step {steps} @ t={t}")
            bid_spot = sim.get_best_price_ticks(spot, Side.BUY)
            ask_spot = sim.get_best_price_ticks(spot, Side.SELL)
            bid_perp = sim.get_best_price_ticks(perp, Side.BUY)
            ask_perp = sim.get_best_price_ticks(perp, Side.SELL)
            print(f"  Spot best: bid={bid_spot} ask={ask_spot}")
            print(f"  Perp best: bid={bid_perp} ask={ask_perp}")
            top2b = sim.l2_top_n(spot, Side.BUY, 2)
            top2a = sim.l2_top_n(perp, Side.SELL, 2)
            print(f"  Spot top2 bids: {print_levels(top2b)} | Perp top2 asks: {print_levels(top2a)}")

            # Inject a simple strategy order on spot
            ts_recv = t if t is not None else 0
            strat = NormalizedLobEvent(
                tsExchange=ts_recv,
                tsReceived=ts_recv + 1,
                side=Side.BUY,
                updateType=UpdateType.ADD,
                priceTicks=ask_spot or 0,
                quantityLots=2,
                orderId=123456000 + steps,
                traderId=42,
                aggressorId=NoAggressorNeededSentinel,
                updateSource=UpdateSource.STRATEGY,
                symbolId="BTC-USDT",
            )
            sim.submit_strategy_event(spot, strat)

    print(f"\nReplay finished after {steps} steps")
    fills = sink.fills()
    print(f"Total fills: {len(fills)}")
    for f in fills:
        print(
            f"  [{f.bookKey}] seq={f.seq} px={f.priceTicks} qty={f.qtyLots} "
            f"makerSide={int(f.makerSide)} maker={f.makerOrderId} taker={f.takerOrderId} "
            f"src m/t={int(f.makerSource)}/{int(f.takerSource)}"
        )


if __name__ == "__main__":
    main()
