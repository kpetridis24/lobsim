from __future__ import annotations

import argparse
import datetime as dt
import time
from dataclasses import dataclass
from decimal import ROUND_HALF_UP, Decimal
from pathlib import Path
from typing import Iterable

import resource
import sys

try:
    import pyarrow.parquet as pq
except ImportError as exc:
    raise SystemExit(
        "pyarrow is required for the BTCUSDT parquet benchmark. Install it in your environment."
    ) from exc

from lobsim.engine import PaperTradingSimulatorCore
from lobsim.lob_event import NormalizedLobEvent
from lobsim.replay import ReplayConfig, ReplaySession
from lobsim.sink import InMemoryLogSink
from lobsim.types import (
    Side,
    UnknownAggressorIdSentinel,
    UnknownTraderIdSentinel,
    UpdateSource,
    UpdateType,
)


@dataclass
class CoinapiRawEvent:
    ts_exchange_us: int
    ts_received_us: int
    update_type: str
    is_buy: int
    entry_px: float
    entry_sx: float
    order_id: str


class CoinapiCoinbaseBTCUSDTSource:
    def __init__(self, path: str | Path, batch_size: int = 8192):
        self._pq = pq.ParquetFile(str(path))
        self._batch_size = batch_size
        self._cols = [
            "time_exchange",
            "time_coinapi",
            "update_type",
            "is_buy",
            "entry_px",
            "entry_sx",
            "order_id",
        ]

    def __iter__(self) -> Iterable[CoinapiRawEvent]:
        for batch in self._pq.iter_batches(
            columns=self._cols, batch_size=self._batch_size
        ):
            cols = {name: batch.column(i) for i, name in enumerate(self._cols)}
            n = batch.num_rows
            for i in range(n):
                yield CoinapiRawEvent(
                    ts_exchange_us=parse_time_us(cols["time_exchange"][i].as_py()),
                    ts_received_us=parse_time_us(cols["time_coinapi"][i].as_py()),
                    update_type=str(cols["update_type"][i].as_py()),
                    is_buy=int(cols["is_buy"][i].as_py()),
                    entry_px=float(cols["entry_px"][i].as_py()),
                    entry_sx=float(cols["entry_sx"][i].as_py()),
                    order_id=str(cols["order_id"][i].as_py()),
                )


class CoinapiCoinbaseBTCUSDTAdapter:
    def __init__(
        self,
        *,
        tick_size: float,
        lot_size: float,
        symbol_id: str = "BTC-USDT",
    ):
        self.tick_size = tick_size
        self.lot_size = lot_size
        self.symbol_id = symbol_id

    def normalize(self, raw: CoinapiRawEvent) -> NormalizedLobEvent:
        if raw.ts_exchange_us < 0 or raw.ts_received_us < 0:
            raise ValueError("negative timestamp")
        if raw.entry_px < 0 or raw.entry_sx < 0:
            raise ValueError("negative price/size")
        if not raw.order_id:
            raise ValueError("empty order_id")

        update_type = parse_update_type(raw.update_type)
        side = Side.BUY if raw.is_buy else Side.SELL

        price_ticks = to_ticks(raw.entry_px, self.tick_size)
        qty_lots = to_ticks(raw.entry_sx, self.lot_size)
        order_id = stable_int64(raw.order_id)

        return NormalizedLobEvent(
            tsExchange=raw.ts_exchange_us,
            tsReceived=raw.ts_received_us,
            side=side,
            updateType=update_type,
            priceTicks=price_ticks,
            quantityLots=qty_lots,
            orderId=order_id,
            traderId=UnknownTraderIdSentinel,
            aggressorId=UnknownAggressorIdSentinel,
            updateSource=UpdateSource.HISTORICAL,
            symbolId=self.symbol_id,
        )


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
    if isinstance(v, int):
        return v
    if isinstance(v, dt.time):
        return (v.hour * 3600 + v.minute * 60 + v.second) * 1_000_000 + v.microsecond
    if isinstance(v, str):
        hh, mm, rest = v.split(":")
        ss, frac = (rest.split(".") + ["0"])[:2]
        frac = (frac + "000000")[:6]
        return (
            int(hh) * 3600_000_000
            + int(mm) * 60_000_000
            + int(ss) * 1_000_000
            + int(frac)
        )
    raise ValueError(f"unsupported time value: {type(v)}")


def to_ticks(x: float, step: float) -> int:
    dx = Decimal(str(x))
    ds = Decimal(str(step))
    y = dx / ds
    if y < 0:
        raise ValueError("negative ticks/lots")
    return int(y.to_integral_value(rounding=ROUND_HALF_UP))


def stable_int64(s: str) -> int:
    h = 14695981039346656037
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    if h >= 2**63:
        h -= 2**64
    return int(h)


def rss_bytes(ru_maxrss: int) -> int:
    if sys.platform == "darwin":
        return int(ru_maxrss)
    return int(ru_maxrss) * 1024


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--path",
        default="sample_data/coinapi_coinbase_btcusdt_sample.parquet",
    )
    parser.add_argument("--tick-size", type=float, default=0.01)
    parser.add_argument("--lot-size", type=float, default=1e-8)
    parser.add_argument("--symbol", default="BTC-USDT")
    parser.add_argument("--batch-size", type=int, default=8192)
    parser.add_argument("--max-events", type=int, default=0)
    parser.add_argument(
        "--with-sink",
        action="store_true",
        help="Use InMemoryLogSink (stores all fills).",
    )

    args = parser.parse_args()

    engine = PaperTradingSimulatorCore()
    sink = InMemoryLogSink() if args.with_sink else None
    if sink is not None:
        engine.set_log_sink(sink)

    adapter = CoinapiCoinbaseBTCUSDTAdapter(
        tick_size=args.tick_size, lot_size=args.lot_size, symbol_id=args.symbol
    )
    source = CoinapiCoinbaseBTCUSDTSource(args.path, batch_size=args.batch_size)
    replay = ReplaySession(engine, ReplayConfig(require_monotonic_ts_received=True))

    raw_events = 0
    normalized_events = 0
    engine_updates = 0
    adapter_failures = 0
    has_range = False
    first_ts = 0
    last_ts = 0

    usage_start = resource.getrusage(resource.RUSAGE_SELF)
    t0 = time.perf_counter()

    for raw in source:
        if args.max_events and raw_events >= args.max_events:
            break
        raw_events += 1
        try:
            ev = adapter.normalize(raw)
        except Exception:
            adapter_failures += 1
            continue

        normalized_events += 1
        if not has_range:
            has_range = True
            first_ts = ev.tsReceived
            last_ts = ev.tsReceived
        else:
            if ev.tsReceived < first_ts:
                first_ts = ev.tsReceived
            if ev.tsReceived > last_ts:
                last_ts = ev.tsReceived

        replay.step(ev)
        engine_updates += 1

    t1 = time.perf_counter()
    usage_end = resource.getrusage(resource.RUSAGE_SELF)

    wall_seconds = t1 - t0
    cpu_seconds = (usage_end.ru_utime - usage_start.ru_utime) + (
        usage_end.ru_stime - usage_start.ru_stime
    )
    events_per_sec = engine_updates / wall_seconds if wall_seconds else 0.0
    raw_per_sec = raw_events / wall_seconds if wall_seconds else 0.0

    print("benchmark=python")
    print(f"raw_events={raw_events}")
    print(f"normalized_events={normalized_events}")
    print(f"engine_updates={engine_updates}")
    print(f"adapter_failures={adapter_failures}")
    print(f"wall_seconds={wall_seconds:.3f}")
    print(f"cpu_seconds={cpu_seconds:.3f}")
    print(f"cpu_utilization={(cpu_seconds / wall_seconds) if wall_seconds else 0.0:.3f}")
    print(f"events_per_sec={events_per_sec:.3f}")
    print(f"raw_events_per_sec={raw_per_sec:.3f}")
    print(f"max_rss_bytes={rss_bytes(usage_end.ru_maxrss)}")

    if sink is not None:
        print(f"fill_count={len(sink.get_fills())}")
    else:
        print("fill_count=0")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
