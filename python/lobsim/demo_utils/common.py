from __future__ import annotations

import datetime as dt
import hashlib
from dataclasses import dataclass
from decimal import ROUND_HALF_UP, Decimal
from pathlib import Path
from typing import Iterable

import pyarrow.parquet as pq

from lobsim.lob_event import NormalizedLobEvent
from lobsim.types import (
    Side,
    UnknownAggressorIdSentinel,
    UnknownTraderIdSentinel,
    UpdateSource,
    UpdateType,
)


def parse_update_type(value: str) -> UpdateType:
    s = value.strip().upper()
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
    if s == "AGGRESSIVE_TRADE":
        return UpdateType.AGGRESSIVE_TRADE
    raise ValueError(f"unknown update_type: {value}")


def parse_time_us(v) -> int:
    if isinstance(v, int):
        return v
    if isinstance(v, float):
        return int(v)
    if isinstance(v, dt.time):
        return (v.hour * 3600 + v.minute * 60 + v.second) * 1_000_000 + v.microsecond
    if isinstance(v, str):
        if ":" in v:
            hh, mm, rest = v.split(":")
            ss, frac = (rest.split(".") + ["0"])[:2]
            frac = (frac + "000000")[:6]
            return (
                int(hh) * 3600_000_000
                + int(mm) * 60_000_000
                + int(ss) * 1_000_000
                + int(frac)
            )
        return int(float(v))
    raise ValueError(f"unsupported time value: {type(v)}")


def to_ticks(x: float, step: float, *, strict: bool) -> int:
    dx = Decimal(str(x))
    ds = Decimal(str(step))
    y = dx / ds
    r = y.to_integral_value(rounding=ROUND_HALF_UP)
    if strict and y != r:
        raise ValueError(f"value {x} not on grid {step}")
    return int(r)


def stable_int64(value: str) -> int:
    h = hashlib.blake2b(value.encode("utf-8"), digest_size=8).digest()
    v = int.from_bytes(h, "little", signed=False)
    if v >= 2**63:
        v -= 2**64
    return v


@dataclass
class RawEvent:
    ts_exchange_us: int
    ts_received_us: int
    update_type: str
    is_buy: int
    entry_px: float
    entry_sx: float
    order_id: str


class ParquetStream:
    def __init__(self, path: str | Path, batch_size: int = 100):
        self._pq = pq.ParquetFile(str(path))
        self._batch_size = batch_size
        schema_names = self._pq.schema_arrow.names
        recv_col = next(
            (n for n in schema_names if n in ("time_received", "time_feed")), None
        )
        if recv_col is None:
            candidates = [
                n for n in schema_names if "time" in n and "exchange" not in n
            ]
            if not candidates:
                raise ValueError("No received time column found")
            recv_col = candidates[0]
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
        self._batch_iter = self._pq.iter_batches(
            columns=self._cols, batch_size=self._batch_size
        )
        self._batch = None
        self._row = 0

    def __iter__(self) -> Iterable[RawEvent]:
        return self

    def __next__(self) -> RawEvent:
        if self._batch is None or self._row >= self._batch.num_rows:
            self._batch = next(self._batch_iter)
            self._row = 0

        cols = {name: self._batch.column(i) for i, name in enumerate(self._cols)}
        i = self._row
        self._row += 1

        return RawEvent(
            ts_exchange_us=parse_time_us(cols["time_exchange"][i].as_py()),
            ts_received_us=parse_time_us(cols[self._recv_col][i].as_py()),
            update_type=str(cols["update_type"][i].as_py()),
            is_buy=int(cols["is_buy"][i].as_py()),
            entry_px=float(cols["entry_px"][i].as_py()),
            entry_sx=float(cols["entry_sx"][i].as_py()),
            order_id=str(cols["order_id"][i].as_py()),
        )


class Adapter:
    def __init__(self, *, tick_size: float, lot_size: float, symbol_id: str):
        self.tick_size = tick_size
        self.lot_size = lot_size
        self.symbol_id = symbol_id

    def normalize(self, raw: RawEvent) -> NormalizedLobEvent:
        if raw.ts_exchange_us < 0 or raw.ts_received_us < 0:
            raise ValueError("negative timestamp")
        if raw.entry_px < 0 or raw.entry_sx < 0:
            raise ValueError("negative price/size")
        if not raw.order_id:
            raise ValueError("empty order_id")

        update_type = parse_update_type(raw.update_type)
        side = Side.BUY if raw.is_buy else Side.SELL
        price_ticks = to_ticks(raw.entry_px, self.tick_size, strict=True)
        qty_lots = to_ticks(raw.entry_sx, self.lot_size, strict=True)
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
            symbolId=self.symbol_id,
            updateSource=UpdateSource.HISTORICAL,
        )


def find_snapshot_path(data_path: Path) -> Path | None:
    if data_path.name.endswith("_snap.parquet") and data_path.exists():
        return data_path
    candidate = data_path.with_name(f"{data_path.stem}_snap{data_path.suffix}")
    if candidate.exists():
        return candidate
    return None


def load_l3_snapshot(
    path: Path, *, tick_size: float, lot_size: float, batch_size: int = 4096
):
    cols = ["update_type", "is_buy", "entry_px", "entry_sx", "order_id"]
    pf = pq.ParquetFile(str(path))
    missing = [name for name in cols if name not in pf.schema.names]
    if missing:
        raise ValueError(f"snapshot parquet missing columns: {missing}")

    sides: list[Side] = []
    prices: list[int] = []
    quantities: list[int] = []
    order_ids: list[int] = []
    trader_ids: list[int] = []
    duplicate = 0
    seen_orders: set[int] = set()

    for batch in pf.iter_batches(columns=cols, batch_size=batch_size):
        bcols = {name: batch.column(i) for i, name in enumerate(cols)}
        for i in range(batch.num_rows):
            side = Side.BUY if int(bcols["is_buy"][i].as_py()) else Side.SELL
            price = to_ticks(
                float(bcols["entry_px"][i].as_py()), tick_size, strict=True
            )
            qty = to_ticks(float(bcols["entry_sx"][i].as_py()), lot_size, strict=True)
            order_id_str = str(bcols["order_id"][i].as_py())
            order_id = stable_int64(order_id_str)
            if order_id in seen_orders:
                duplicate += 1
                continue
            seen_orders.add(order_id)
            sides.append(side)
            prices.append(price)
            quantities.append(qty)
            order_ids.append(order_id)
            trader_ids.append(UnknownTraderIdSentinel)
    return sides, prices, quantities, order_ids, trader_ids, duplicate
