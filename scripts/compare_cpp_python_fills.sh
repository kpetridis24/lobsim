#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root_dir}/build"
out_dir="${build_dir}/fill_compare"
data_path="${1:-${root_dir}/sample_data/coinbase_btcusdt_sample.parquet}"

if [ ! -f "${data_path}" ]; then
  echo "Skipping fill comparison: sample data not found at ${data_path}" >&2
  exit 0
fi

if head -n 1 "${data_path}" | grep -q "git-lfs.github.com/spec"; then
  echo "Skipping fill comparison: ${data_path} is a Git LFS pointer (data not available in CI)." >&2
  exit 0
fi

mkdir -p "${out_dir}"
cpp_out="${out_dir}/fills_cpp.csv"
py_out="${out_dir}/fills_py.csv"
diff_out="${out_dir}/fills_diff.txt"

cmake -S "${root_dir}" -B "${build_dir}" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "${build_dir}" --target lobsim_cpp_example

"${build_dir}/lobsim_cpp_example" "${data_path}" --dump-fills "${cpp_out}"

export PYTHONPATH="${root_dir}/python${PYTHONPATH:+:${PYTHONPATH}}"
DATA_PATH="${data_path}" OUT_PATH="${py_out}" python3 - <<'PY'
import csv
import datetime as dt
import os
from dataclasses import dataclass
from decimal import ROUND_HALF_UP, Decimal
from pathlib import Path
from typing import Iterable

try:
    import pyarrow.parquet as pq
except ImportError as exc:
    raise SystemExit(
        "pyarrow is required to read the parquet sample. "
        "Install it in your environment and re-run."
    ) from exc

from lobsim.engine import PaperTradingSimulator
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


def fnv1a_64_signed(value: str) -> int:
    h = 14695981039346656037
    for b in value.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    if h >= 2**63:
        h -= 2**64
    return h


@dataclass
class CoinbaseRawEvent:
    ts_exchange_us: int
    ts_received_us: int
    update_type: str
    is_buy: int
    entry_px: float
    entry_sx: float
    order_id: str


class CoinbaseBTCUSDTSource:
    def __init__(self, path: str | Path, batch_size: int = 128):
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

    def __iter__(self) -> Iterable[CoinbaseRawEvent]:
        for batch in self._pq.iter_batches(
            columns=self._cols, batch_size=self._batch_size
        ):
            cols = {name: batch.column(i) for i, name in enumerate(self._cols)}
            n = batch.num_rows
            for i in range(n):
                yield CoinbaseRawEvent(
                    ts_exchange_us=parse_time_us(cols["time_exchange"][i].as_py()),
                    ts_received_us=parse_time_us(cols[self._recv_col][i].as_py()),
                    update_type=str(cols["update_type"][i].as_py()),
                    is_buy=int(cols["is_buy"][i].as_py()),
                    entry_px=float(cols["entry_px"][i].as_py()),
                    entry_sx=float(cols["entry_sx"][i].as_py()),
                    order_id=str(cols["order_id"][i].as_py()),
                )


class CoinbaseBTCUSDTAdapter:
    def __init__(self, *, tick_size: float, lot_size: float, symbol_id: str = "BTC-USDT"):
        self.tick_size = tick_size
        self.lot_size = lot_size
        self.symbol_id = symbol_id

    def normalize(self, raw: CoinbaseRawEvent) -> NormalizedLobEvent:
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
        order_id = fnv1a_64_signed(raw.order_id)

        return NormalizedLobEvent(
            ts_exchange=raw.ts_exchange_us,
            ts_received=raw.ts_received_us,
            side=side,
            update_type=update_type,
            price_ticks=price_ticks,
            quantity_lots=qty_lots,
            order_id=order_id,
            trader_id=UnknownTraderIdSentinel,
            aggressor_id=UnknownAggressorIdSentinel,
            update_source=UpdateSource.HISTORICAL,
            symbol_id=self.symbol_id,
        )


def main() -> None:
    data_path = os.environ["DATA_PATH"]
    out_path = os.environ["OUT_PATH"]

    engine = PaperTradingSimulator()
    sink = InMemoryLogSink()
    engine.set_log_sink(sink)

    adapter = CoinbaseBTCUSDTAdapter(tick_size=0.01, lot_size=1e-8)
    source = CoinbaseBTCUSDTSource(data_path)

    cfg = ReplayConfig(require_monotonic_ts_received=True, fail_fast=True)
    replay = ReplaySession(engine, cfg)
    replay.run_raw(source=source, adapter=adapter, config=cfg)

    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(
            [
                "seq",
                "ts_exchange",
                "ts_received",
                "price_ticks",
                "qty_lots",
                "maker_side",
                "maker_order_id",
                "maker_trader_id",
                "maker_source",
                "taker_side",
                "taker_order_id",
                "taker_trader_id",
                "taker_source",
            ]
        )
        for r in sink.get_fills():
            writer.writerow(
                [
                    r.seq,
                    r.ts_exchange,
                    r.ts_received,
                    r.price_ticks,
                    r.qty_lots,
                    int(r.maker_side),
                    r.maker_order_id,
                    r.maker_trader_id,
                    int(r.maker_source),
                    int(r.taker_side),
                    r.taker_order_id,
                    r.taker_trader_id,
                    int(r.taker_source),
                ]
            )


if __name__ == "__main__":
    main()
PY

cpp_count=$(tail -n +2 "${cpp_out}" | wc -l | tr -d ' ')
py_count=$(tail -n +2 "${py_out}" | wc -l | tr -d ' ')

if [[ "${cpp_count}" != "${py_count}" ]]; then
    echo "Fill count mismatch: cpp=${cpp_count} python=${py_count}"
    exit 1
fi

if diff -u "${cpp_out}" "${py_out}" > "${diff_out}"; then
    echo "Fills match: ${cpp_count} records"
    echo "Diff output: ${diff_out}"
else
    echo "Fills differ. Unified diff saved to: ${diff_out}"
    exit 1
fi
