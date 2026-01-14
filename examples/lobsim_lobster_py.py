from __future__ import annotations

import csv
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Sequence

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
class LobsterMessageEvent:
    ts_exchange_us: int
    event_type: int
    order_id: int
    size: int
    price_ticks: int
    direction: int


class LobsterMessageSource:
    def __init__(self, path: str | Path, *, skip_rows: int = 0):
        self._path = Path(path)
        self._skip_rows = skip_rows

    def __iter__(self) -> Iterator[LobsterMessageEvent]:
        with self._path.open() as f:
            reader = csv.reader(f)
            for _ in range(self._skip_rows):
                next(reader, None)
            for row in reader:
                if not row:
                    continue
                if len(row) < 6:
                    raise ValueError(f"Bad message row with {len(row)} columns")
                ts_exchange_us = int(round(float(row[0]) * 1_000_000))
                event_type = int(row[1])
                order_id = int(row[2])
                size = int(row[3])
                price_ticks = int(row[4])
                direction = int(row[5])
                yield LobsterMessageEvent(
                    ts_exchange_us=ts_exchange_us,
                    event_type=event_type,
                    order_id=order_id,
                    size=size,
                    price_ticks=price_ticks,
                    direction=direction,
                )


class LobsterOrderbookSource:
    def __init__(self, path: str | Path, levels: int, *, skip_rows: int = 0):
        self._path = Path(path)
        self._levels = levels
        self._skip_rows = skip_rows

    def __iter__(self) -> Iterator[list[int]]:
        with self._path.open() as f:
            reader = csv.reader(f)
            for _ in range(self._skip_rows):
                next(reader, None)
            for row in reader:
                if not row:
                    continue
                expected = self._levels * 4
                if len(row) < expected:
                    raise ValueError(f"Bad orderbook row with {len(row)} columns (expected {expected})")
                yield [int(x) for x in row[:expected]]


class LobsterAdapter:
    def __init__(self, *, symbol_id: str):
        self.symbol_id = symbol_id

    def normalize(self, raw: LobsterMessageEvent) -> NormalizedLobEvent:
        ev = self.try_normalize(raw)
        if ev is None:
            raise ValueError("LOBSTER hidden execution (type 5) not applied to visible book")
        return ev

    def try_normalize(self, raw: LobsterMessageEvent) -> NormalizedLobEvent | None:
        if raw.event_type == 5:
            return None

        if raw.event_type == 1:
            update_type = UpdateType.ADD
        elif raw.event_type == 2:
            update_type = UpdateType.SUBTRACT
        elif raw.event_type == 3:
            update_type = UpdateType.DELETE
        elif raw.event_type == 4:
            update_type = UpdateType.MATCH
        else:
            raise ValueError(f"Unsupported LOBSTER event type: {raw.event_type}")

        if raw.direction not in (1, -1):
            raise ValueError(f"Bad LOBSTER direction: {raw.direction}")

        side = Side.BUY if raw.direction == 1 else Side.SELL

        # LOBSTER prices are already scaled by 1e-4 dollars, so we use them directly as ticks.
        return NormalizedLobEvent(
            tsExchange=raw.ts_exchange_us,
            tsReceived=raw.ts_exchange_us,
            side=side,
            updateType=update_type,
            priceTicks=raw.price_ticks,
            quantityLots=raw.size,
            orderId=raw.order_id,
            traderId=UnknownTraderIdSentinel,
            aggressorId=UnknownAggressorIdSentinel,
            updateSource=UpdateSource.HISTORICAL,
            symbolId=self.symbol_id,
        )


def init_from_orderbook_row(
    engine: PaperTradingSimulatorCore, row: Sequence[int], levels: int
) -> None:
    sides: list[Side] = []
    prices: list[int] = []
    quantities: list[int] = []
    for level in range(levels):
        ask_px = row[4 * level]
        ask_sz = row[4 * level + 1]
        bid_px = row[4 * level + 2]
        bid_sz = row[4 * level + 3]

        if ask_px > 0 and ask_sz > 0:
            sides.append(Side.SELL)
            prices.append(ask_px)
            quantities.append(ask_sz)
        if bid_px > 0 and bid_sz > 0:
            sides.append(Side.BUY)
            prices.append(bid_px)
            quantities.append(bid_sz)

    engine.init_from_l2_snapshot(sides, prices, quantities)


def main() -> int:
    base = Path("sample_data")
    message_path = base / "AMZN_2012-06-21_34200000_57600000_message_10.csv"
    orderbook_path = base / "AMZN_2012-06-21_34200000_57600000_orderbook_10.csv"
    if len(sys.argv) > 1:
        message_path = Path(sys.argv[1])
    if len(sys.argv) > 2:
        orderbook_path = Path(sys.argv[2])

    levels = 10

    engine = PaperTradingSimulatorCore()
    sink = InMemoryLogSink()
    engine.set_log_sink(sink)

    first_book_row = next(iter(LobsterOrderbookSource(orderbook_path, levels)))
    init_from_orderbook_row(engine, first_book_row, levels)

    # Row i of the orderbook corresponds to the state after message row i.
    # We initialize from row 0 and skip message row 0 to keep alignment.
    message_source = LobsterMessageSource(message_path, skip_rows=1)
    book_source = LobsterOrderbookSource(orderbook_path, levels, skip_rows=1)
    adapter = LobsterAdapter(symbol_id="AMZN")

    replay = ReplaySession(engine, ReplayConfig(require_monotonic_ts_received=True, fail_fast=True))

    processed = 0
    applied = 0
    skipped_hidden = 0
    mismatches = 0

    for raw, book_row in zip(message_source, book_source):
        processed += 1
        ev = adapter.try_normalize(raw)
        if ev is None:
            skipped_hidden += 1
        else:
            replay.step(ev)
            applied += 1

        if processed % 10000 == 0:
            top_bid = engine.l2_top_n(Side.BUY, 1)
            top_ask = engine.l2_top_n(Side.SELL, 1)
            bid_px, bid_sz = book_row[2], book_row[3]
            ask_px, ask_sz = book_row[0], book_row[1]

            bid_ok = (not top_bid and bid_px == 0) or (
                top_bid and top_bid[0] == (bid_px, bid_sz)
            )
            ask_ok = (not top_ask and ask_px == 0) or (
                top_ask and top_ask[0] == (ask_px, ask_sz)
            )
            if not (bid_ok and ask_ok):
                mismatches += 1

            print(
                f"row={processed} applied={applied} skipped_hidden={skipped_hidden} mismatches={mismatches}"
            )

    print(
        "done",
        f"rows={processed}",
        f"applied={applied}",
        f"skipped_hidden={skipped_hidden}",
        f"mismatches={mismatches}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
