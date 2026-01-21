from __future__ import annotations

import html
import sys
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple

import streamlit as st
from plotly import graph_objects as go

# Allow running from repo without reinstalling the package
REPO_PY = Path(__file__).resolve().parents[1] / "python"
if str(REPO_PY) not in sys.path:
    sys.path.append(str(REPO_PY))

from lobsim import BookId
from lobsim.demo_utils import Adapter, ParquetStream, find_snapshot_path, load_l3_snapshot, to_ticks
from lobsim.lob_event import NormalizedLobEvent
from lobsim.multibook import Config, MultiBookSimulator
from lobsim.sink import InMemoryMultiLogSink, PaperOrderLedgerStatus
from lobsim.types import (
    DiagnosticRecordCode,
    DiagnosticRecordSeverity,
    diagnostic_code_names,
    diagnostic_severity_names,
    Side,
    UnknownAggressorIdSentinel,
    UnknownTraderIdSentinel,
    UpdateSource,
    UpdateType,
)


class _BufferedRawStream:
    def __init__(self, first, source: ParquetStream):
        self._first = first
        self._source = source
        self._used_first = False

    def __iter__(self):
        return self

    def __next__(self):
        if not self._used_first:
            self._used_first = True
            return self._first
        return next(self._source)


class _TimeShiftAdapter:
    def __init__(self, adapter: Adapter, *, offset_received: int, offset_exchange: int):
        self._adapter = adapter
        self._offset_received = offset_received
        self._offset_exchange = offset_exchange

    def normalize(self, raw):
        ev = self._adapter.normalize(raw)
        ev.tsReceived = max(0, ev.tsReceived - self._offset_received)
        ev.tsExchange = max(0, ev.tsExchange - self._offset_exchange)
        return ev


@dataclass(frozen=True)
class BookSpec:
    label: str
    venue: str
    symbol: str
    data_path: str
    tick_size: float
    lot_size: float

    @property
    def book_id(self) -> BookId:
        return BookId(self.venue, self.symbol)

    @property
    def book_key(self) -> str:
        return f"{self.venue}:{self.symbol}" if self.venue else self.symbol


BOOK_SPECS = [
    BookSpec(
        label="Coinbase BTC-USDT",
        venue="coinbase",
        symbol="BTC-USDT",
        data_path="sample_data/coinbase_btcusdt_sample_big.parquet",
        tick_size=0.01,
        lot_size=1e-8,
    ),
    BookSpec(
        label="Binance BTC-USDT",
        venue="binance",
        symbol="BTC-USDT",
        data_path="sample_data/binance_btcusdt_sample_big.parquet",
        tick_size=0.01,
        lot_size=1e-8,
    ),
    BookSpec(
        label="Coinbase ETH-USDT",
        venue="coinbase",
        symbol="ETH-USDT",
        data_path="sample_data/coinbase_ethusdt_sample_big.parquet",
        tick_size=0.01,
        lot_size=1e-8,
    ),
]

BTC_BOOK_KEYS = [
    "coinbase:BTC-USDT",
    "binance:BTC-USDT",
]
ETH_BOOK_KEY = "coinbase:ETH-USDT"

DIAG_CODE_NAMES = diagnostic_code_names()
DIAG_SEVERITY_NAMES = diagnostic_severity_names()


def _safe_int(value: int) -> str | int:
    if value > (2**53 - 1) or value < -(2**53 - 1):
        return str(value)
    return value


def _price_from_ticks(book_key: str, ticks: int) -> float:
    spec = st.session_state.book_specs_by_key[book_key]
    return round(ticks * spec.tick_size, 2)


def _qty_from_lots(book_key: str, lots: int) -> float:
    spec = st.session_state.book_specs_by_key[book_key]
    return round(lots * spec.lot_size, 8)


def _render_table(rows: list[dict[str, object]], *, height_px: int) -> None:
    if not rows:
        st.info("No data yet.")
        return
    columns = list(rows[0].keys())
    header = "".join(f"<th>{html.escape(str(col))}</th>" for col in columns)
    body_rows = []
    for row in rows:
        cells = "".join(f"<td>{html.escape(str(row.get(col, '')))}</td>" for col in columns)
        body_rows.append(f"<tr>{cells}</tr>")
    table_html = f"""
    <div class="lobsim-table-wrap" style="max-height: {height_px}px;">
      <table class="lobsim-table">
        <thead><tr>{header}</tr></thead>
        <tbody>{''.join(body_rows)}</tbody>
      </table>
    </div>
    """
    st.markdown(table_html, unsafe_allow_html=True)


def _compute_imbalance(window: deque[float]) -> float:
    if not window:
        return 0.0
    total = sum(abs(x) for x in window)
    if total == 0:
        return 0.0
    return sum(window) / total


def _format_specs_table() -> list[dict[str, object]]:
    rows = []
    for spec in BOOK_SPECS:
        snap_path = find_snapshot_path(Path(spec.data_path))
        offset = st.session_state.time_offsets.get(spec.book_key)
        rows.append(
            {
                "book": spec.book_key,
                "label": spec.label,
                "tick_size": spec.tick_size,
                "lot_size": spec.lot_size,
                "data": spec.data_path,
                "snapshot": snap_path.name if snap_path else "missing",
                "time_offset_us": _safe_int(offset) if offset is not None else "-",
            }
        )
    return rows


def _format_event_rows(events: list[object]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for ev in events:
        rows.append(
            {
                "seq": ev.seq,
                "ts_exchange": _safe_int(ev.tsExchange),
                "ts_received": _safe_int(ev.tsReceived),
                "side": ev.side.name,
                "update_type": ev.updateType.name,
                "source": ev.source.name,
                "price": _price_from_ticks(ev.bookKey, ev.priceTicks),
                "qty": _qty_from_lots(ev.bookKey, ev.qtyLots),
                "price_ticks": ev.priceTicks,
                "qty_lots": ev.qtyLots,
                "order_id": str(ev.orderId),
                "book": ev.bookKey,
            }
        )
    return rows


def _format_fill_rows(fills: list[object]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for fill in fills:
        rows.append(
            {
                "seq": fill.seq,
                "ts_exchange": _safe_int(fill.tsExchange),
                "ts_received": _safe_int(fill.tsReceived),
                "price": _price_from_ticks(fill.bookKey, fill.priceTicks),
                "qty": _qty_from_lots(fill.bookKey, fill.qtyLots),
                "price_ticks": fill.priceTicks,
                "qty_lots": fill.qtyLots,
                "maker_side": fill.makerSide.name,
                "maker_source": fill.makerSource.name,
                "taker_side": fill.takerSide.name,
                "taker_source": fill.takerSource.name,
                "maker_order_id": str(fill.makerOrderId),
                "taker_order_id": str(fill.takerOrderId),
                "book": fill.bookKey,
            }
        )
    return rows


def _format_diag_rows(diags: list[object]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for diag in diags:
        try:
            code_id = int(diag.code)
        except Exception:
            code_id = -1
        try:
            severity_id = int(diag.severity)
        except Exception:
            severity_id = -1
        code_name = DIAG_CODE_NAMES.get(code_id) or f"CODE_{code_id}"
        severity_name = DIAG_SEVERITY_NAMES.get(severity_id) or f"SEVERITY_{severity_id}"
        rows.append(
            {
                "seq": diag.seq,
                "ts_exchange": _safe_int(diag.tsExchange),
                "ts_received": _safe_int(diag.tsReceived),
                "code": code_name,
                "severity": severity_name,
                "code_id": code_id,
                "severity_id": severity_id,
                "book": diag.bookKey,
            }
        )
    return rows


def ensure_session() -> None:
    if "sim" in st.session_state:
        return
    st.session_state.sim = None
    st.session_state.sink = None
    st.session_state.book_specs_by_key = {spec.book_key: spec for spec in BOOK_SPECS}
    st.session_state.book_ids = {spec.book_key: spec.book_id for spec in BOOK_SPECS}
    st.session_state.mid_history = {spec.book_key: [] for spec in BOOK_SPECS}
    st.session_state.spread_history = {spec.book_key: [] for spec in BOOK_SPECS}
    st.session_state.ofi_history = {spec.book_key: [] for spec in BOOK_SPECS}
    st.session_state.tfi_history = {spec.book_key: [] for spec in BOOK_SPECS}
    st.session_state.event_window = {spec.book_key: deque(maxlen=200) for spec in BOOK_SPECS}
    st.session_state.fill_window = {spec.book_key: deque(maxlen=200) for spec in BOOK_SPECS}
    st.session_state.book_counts = {spec.book_key: {"events": 0, "fills": 0} for spec in BOOK_SPECS}
    st.session_state.arb_history = []
    st.session_state.event_count = 0
    st.session_state.fill_count = 0
    st.session_state.diag_count = 0
    st.session_state.last_action = "-"
    st.session_state.last_trade_ts = 0
    st.session_state.last_decision_ts = 0
    st.session_state.global_steps = 0
    st.session_state.next_order_id = 2_000_000_000
    st.session_state.time_offsets = {}


def init_simulator(*, batch_size: int, require_monotonic: bool, fail_fast: bool) -> None:
    sim = MultiBookSimulator(Config(require_monotonic_ts_received=require_monotonic, fail_fast=fail_fast))
    sink = InMemoryMultiLogSink()
    sim.set_multi_log_sink(sink)
    st.session_state.time_offsets = {}

    for spec in BOOK_SPECS:
        book_id = spec.book_id
        sim.add_book(book_id)

        snap_path = find_snapshot_path(Path(spec.data_path))
        if snap_path is not None:
            sides, prices, quantities, order_ids, trader_ids, dupes = load_l3_snapshot(
                snap_path, tick_size=spec.tick_size, lot_size=spec.lot_size, batch_size=4096
            )
            if sides:
                sim.init_from_l3_snapshot(book_id, sides, prices, quantities, order_ids, trader_ids)
                msg = f"Loaded snapshot {snap_path.name} ({len(sides)} orders"
                if dupes:
                    msg += f", skipped {dupes} dupes"
                msg += ")"
                st.success(msg)
            else:
                st.warning(f"Snapshot {snap_path.name} had no usable levels.")
        else:
            st.warning(f"Snapshot not found for {spec.label}; starting empty.")

        source = ParquetStream(spec.data_path, batch_size=batch_size)
        adapter = Adapter(tick_size=spec.tick_size, lot_size=spec.lot_size, symbol_id=spec.book_key)

        try:
            first_raw = next(source)
        except StopIteration:
            st.warning(f"No events found in {spec.data_path}")
            continue

        offset_received = first_raw.ts_received_us
        offset_exchange = first_raw.ts_exchange_us
        st.session_state.time_offsets[spec.book_key] = offset_received

        buffered = _BufferedRawStream(first_raw, source)
        aligned_adapter = _TimeShiftAdapter(
            adapter, offset_received=offset_received, offset_exchange=offset_exchange
        )
        sim.add_stream(book_id, buffered, aligned_adapter)

    st.session_state.sim = sim
    st.session_state.sink = sink

    st.session_state.mid_history = {spec.book_key: [] for spec in BOOK_SPECS}
    st.session_state.spread_history = {spec.book_key: [] for spec in BOOK_SPECS}
    st.session_state.ofi_history = {spec.book_key: [] for spec in BOOK_SPECS}
    st.session_state.tfi_history = {spec.book_key: [] for spec in BOOK_SPECS}
    st.session_state.event_window = {spec.book_key: deque(maxlen=200) for spec in BOOK_SPECS}
    st.session_state.fill_window = {spec.book_key: deque(maxlen=200) for spec in BOOK_SPECS}
    st.session_state.book_counts = {spec.book_key: {"events": 0, "fills": 0} for spec in BOOK_SPECS}
    st.session_state.arb_history = []
    st.session_state.event_count = 0
    st.session_state.fill_count = 0
    st.session_state.diag_count = 0
    st.session_state.last_action = "-"
    st.session_state.last_trade_ts = 0
    st.session_state.last_decision_ts = 0
    st.session_state.global_steps = 0
    st.session_state.next_order_id = 2_000_000_000


def _update_history(ts: int) -> None:
    sim: MultiBookSimulator = st.session_state.sim
    for key, book_id in st.session_state.book_ids.items():
        bid = sim.get_best_price_ticks(book_id, Side.BUY)
        ask = sim.get_best_price_ticks(book_id, Side.SELL)
        if bid is None or ask is None:
            continue
        mid = (bid + ask) / 2.0
        spec = st.session_state.book_specs_by_key[key]
        mid_px = mid * spec.tick_size
        spread_px = (ask - bid) * spec.tick_size
        st.session_state.mid_history[key].append((ts, mid_px))
        st.session_state.spread_history[key].append((ts, spread_px))

        ofi = _compute_imbalance(st.session_state.event_window[key])
        tfi = _compute_imbalance(st.session_state.fill_window[key])
        st.session_state.ofi_history[key].append((ts, ofi))
        st.session_state.tfi_history[key].append((ts, tfi))

    cb_key, bn_key = BTC_BOOK_KEYS
    cb_bid = sim.get_best_price_ticks(st.session_state.book_ids[cb_key], Side.BUY)
    cb_ask = sim.get_best_price_ticks(st.session_state.book_ids[cb_key], Side.SELL)
    bn_bid = sim.get_best_price_ticks(st.session_state.book_ids[bn_key], Side.BUY)
    bn_ask = sim.get_best_price_ticks(st.session_state.book_ids[bn_key], Side.SELL)
    if cb_bid is not None and cb_ask is not None and bn_bid is not None and bn_ask is not None:
        cb_spec = st.session_state.book_specs_by_key[cb_key]
        bn_spec = st.session_state.book_specs_by_key[bn_key]
        edge_buy_cb_sell_bn = (bn_bid * bn_spec.tick_size) - (cb_ask * cb_spec.tick_size)
        edge_buy_bn_sell_cb = (cb_bid * cb_spec.tick_size) - (bn_ask * bn_spec.tick_size)
        st.session_state.arb_history.append((ts, edge_buy_cb_sell_bn, edge_buy_bn_sell_cb))


def _process_new_records() -> None:
    sink: InMemoryMultiLogSink = st.session_state.sink
    events = sink.events()
    fills = sink.fills()

    new_events = events[st.session_state.event_count :]
    new_fills = fills[st.session_state.fill_count :]
    st.session_state.event_count = len(events)
    st.session_state.fill_count = len(fills)

    for ev in new_events:
        key = ev.bookKey
        if key in st.session_state.book_counts:
            st.session_state.book_counts[key]["events"] += 1

        if ev.source != UpdateSource.HISTORICAL:
            continue
        if ev.updateType == UpdateType.SET:
            continue

        spec = st.session_state.book_specs_by_key.get(key)
        if spec is None:
            continue
        qty = ev.qtyLots * spec.lot_size
        side_sign = 1.0 if ev.side == Side.BUY else -1.0
        if ev.updateType == UpdateType.ADD:
            delta = side_sign * qty
        else:
            delta = -side_sign * qty
        st.session_state.event_window[key].append(delta)

    for fill in new_fills:
        key = fill.bookKey
        if key in st.session_state.book_counts:
            st.session_state.book_counts[key]["fills"] += 1

        if fill.takerSource != UpdateSource.HISTORICAL:
            continue
        spec = st.session_state.book_specs_by_key.get(key)
        if spec is None:
            continue
        qty = fill.qtyLots * spec.lot_size
        side_sign = 1.0 if fill.takerSide == Side.BUY else -1.0
        st.session_state.fill_window[key].append(side_sign * qty)

    if new_events:
        last_ts = new_events[-1].tsReceived
        _update_history(last_ts)


def _maybe_execute_arbitrage(
    *,
    threshold: float,
    qty: float,
    min_interval_ms: int,
    latency_cb_ms: int,
    latency_bn_ms: int,
    decision_mode: str,
    decision_value: int,
) -> None:
    sim: MultiBookSimulator = st.session_state.sim
    now = sim.current_time() or 0

    allow = False
    if decision_mode == "Every event":
        allow = True
    elif decision_mode == "Every N events":
        if decision_value > 0 and st.session_state.global_steps % decision_value == 0:
            allow = True
    elif decision_mode == "Every N ms":
        if decision_value > 0 and now - st.session_state.last_decision_ts >= decision_value * 1000:
            allow = True

    if not allow:
        return

    st.session_state.last_decision_ts = now

    cb_key, bn_key = BTC_BOOK_KEYS
    cb_id = st.session_state.book_ids[cb_key]
    bn_id = st.session_state.book_ids[bn_key]
    cb_bid = sim.get_best_price_ticks(cb_id, Side.BUY)
    cb_ask = sim.get_best_price_ticks(cb_id, Side.SELL)
    bn_bid = sim.get_best_price_ticks(bn_id, Side.BUY)
    bn_ask = sim.get_best_price_ticks(bn_id, Side.SELL)

    if cb_bid is None or cb_ask is None or bn_bid is None or bn_ask is None:
        st.session_state.last_action = "waiting_for_quotes"
        return

    cb_spec = st.session_state.book_specs_by_key[cb_key]
    bn_spec = st.session_state.book_specs_by_key[bn_key]
    edge_buy_cb_sell_bn = (bn_bid * bn_spec.tick_size) - (cb_ask * cb_spec.tick_size)
    edge_buy_bn_sell_cb = (cb_bid * cb_spec.tick_size) - (bn_ask * bn_spec.tick_size)

    if now - st.session_state.last_trade_ts < min_interval_ms * 1000:
        st.session_state.last_action = "throttled"
        return

    def submit(book_id: BookId, book_key: str, side: Side, price_ticks: int, latency_ms: int) -> bool:
        nonlocal now
        spec = st.session_state.book_specs_by_key[book_key]
        qty_lots = to_ticks(qty, spec.lot_size, strict=False)
        if qty_lots <= 0:
            return False
        ev = NormalizedLobEvent(
            tsExchange=now,
            tsReceived=now,
            side=side,
            updateType=UpdateType.AGGRESSIVE_TRADE,
            priceTicks=price_ticks,
            quantityLots=qty_lots,
            orderId=st.session_state.next_order_id,
            traderId=UnknownTraderIdSentinel,
            aggressorId=UnknownAggressorIdSentinel,
            symbolId=book_key,
            updateSource=UpdateSource.STRATEGY,
        )
        st.session_state.next_order_id += 1
        sim.submit_strategy_event(book_id, ev, latency=latency_ms * 1000)
        return True

    if edge_buy_cb_sell_bn >= threshold:
        ok_buy = submit(cb_id, cb_key, Side.BUY, cb_ask, latency_cb_ms)
        ok_sell = submit(bn_id, bn_key, Side.SELL, bn_bid, latency_bn_ms)
        if ok_buy and ok_sell:
            st.session_state.last_action = f"ARB: buy Coinbase @ {cb_ask} sell Binance @ {bn_bid}"
            st.session_state.last_trade_ts = now
        return

    if edge_buy_bn_sell_cb >= threshold:
        ok_buy = submit(bn_id, bn_key, Side.BUY, bn_ask, latency_bn_ms)
        ok_sell = submit(cb_id, cb_key, Side.SELL, cb_bid, latency_cb_ms)
        if ok_buy and ok_sell:
            st.session_state.last_action = f"ARB: buy Binance @ {bn_ask} sell Coinbase @ {cb_bid}"
            st.session_state.last_trade_ts = now
        return

    st.session_state.last_action = "no_arb"


def step_events(
    n: int,
    *,
    run_strategy: bool,
    strategy_params: dict,
    decision_mode: str,
    decision_value: int,
) -> int:
    sim: MultiBookSimulator = st.session_state.sim
    applied = 0
    for _ in range(n):
        if not sim.step():
            break
        applied += 1
        st.session_state.global_steps += 1
        _process_new_records()
        if run_strategy:
            _maybe_execute_arbitrage(
                threshold=strategy_params["threshold"],
                qty=strategy_params["qty"],
                min_interval_ms=strategy_params["min_interval_ms"],
                latency_cb_ms=strategy_params["latency_cb_ms"],
                latency_bn_ms=strategy_params["latency_bn_ms"],
                decision_mode=decision_mode,
                decision_value=decision_value,
            )
    return applied


def step_time(
    delta_ms: int,
    *,
    run_strategy: bool,
    strategy_params: dict,
    decision_mode: str,
    decision_value: int,
) -> int:
    sim: MultiBookSimulator = st.session_state.sim
    base = sim.current_time() or 0
    target = base + delta_ms * 1000
    applied = 0
    while True:
        if not sim.step():
            break
        applied += 1
        st.session_state.global_steps += 1
        _process_new_records()
        if run_strategy:
            _maybe_execute_arbitrage(
                threshold=strategy_params["threshold"],
                qty=strategy_params["qty"],
                min_interval_ms=strategy_params["min_interval_ms"],
                latency_cb_ms=strategy_params["latency_cb_ms"],
                latency_bn_ms=strategy_params["latency_bn_ms"],
                decision_mode=decision_mode,
                decision_value=decision_value,
            )
        if (sim.current_time() or 0) >= target:
            break
    return applied


def _latest_value(history: List[Tuple[int, float]]) -> float | None:
    return history[-1][1] if history else None


def _plot_btc_mid() -> None:
    cb = st.session_state.mid_history[BTC_BOOK_KEYS[0]][-2000:]
    bn = st.session_state.mid_history[BTC_BOOK_KEYS[1]][-2000:]
    if not cb and not bn:
        st.info("No BTC price history yet.")
        return
    fig = go.Figure()
    if cb:
        fig.add_trace(go.Scatter(x=[t for t, _ in cb], y=[v for _, v in cb], name="Coinbase BTC"))
    if bn:
        fig.add_trace(go.Scatter(x=[t for t, _ in bn], y=[v for _, v in bn], name="Binance BTC"))
    fig.update_layout(
        height=320,
        margin=dict(l=20, r=20, t=35, b=30),
        xaxis_title="tsReceived (us)",
        yaxis_title="mid (price)",
    )
    st.plotly_chart(fig, use_container_width=True)


def _plot_eth_mid() -> None:
    eth = st.session_state.mid_history[ETH_BOOK_KEY][-2000:]
    if not eth:
        st.info("No ETH price history yet.")
        return
    fig = go.Figure()
    fig.add_trace(go.Scatter(x=[t for t, _ in eth], y=[v for _, v in eth], name="ETH mid"))
    fig.update_layout(
        height=260,
        margin=dict(l=20, r=20, t=35, b=30),
        xaxis_title="tsReceived (us)",
        yaxis_title="mid (price)",
    )
    st.plotly_chart(fig, use_container_width=True)


def _plot_arb_edges() -> None:
    history = st.session_state.arb_history[-2000:]
    if not history:
        st.info("No arbitrage edge history yet.")
        return
    ts = [t for t, _, _ in history]
    edge1 = [e1 for _, e1, _ in history]
    edge2 = [e2 for _, _, e2 in history]
    fig = go.Figure()
    fig.add_trace(go.Scatter(x=ts, y=edge1, name="Buy Coinbase / Sell Binance"))
    fig.add_trace(go.Scatter(x=ts, y=edge2, name="Buy Binance / Sell Coinbase"))
    fig.update_layout(
        height=240,
        margin=dict(l=20, r=20, t=35, b=30),
        xaxis_title="tsReceived (us)",
        yaxis_title="edge (price)",
    )
    st.plotly_chart(fig, use_container_width=True)


def _strategy_fills_table() -> None:
    sink: InMemoryMultiLogSink = st.session_state.sink
    rows = []
    for fill in sink.fills():
        if fill.makerSource != UpdateSource.STRATEGY and fill.takerSource != UpdateSource.STRATEGY:
            continue
        rows.append(
            {
                "seq": fill.seq,
                "ts_received": _safe_int(fill.tsReceived),
                "book": fill.bookKey,
                "price": _price_from_ticks(fill.bookKey, fill.priceTicks),
                "qty": _qty_from_lots(fill.bookKey, fill.qtyLots),
                "maker_source": fill.makerSource.name,
                "taker_source": fill.takerSource.name,
                "maker_side": fill.makerSide.name,
                "taker_side": fill.takerSide.name,
            }
        )
    rows = rows[-100:]
    _render_table(rows[::-1], height_px=260)


def _strategy_orders_table(view: str) -> None:
    sink: InMemoryMultiLogSink = st.session_state.sink
    rows = []
    for book_key, ledger in sink.paper_ledger().items():
        for entry in ledger.values():
            state = entry.state
            is_active = state.status in (PaperOrderLedgerStatus.OPEN, PaperOrderLedgerStatus.PARTIALLY_FILLED)
            if view == "Active" and not is_active:
                continue
            if view == "Closed" and is_active:
                continue
            rows.append(
                {
                    "created_seq": state.createdSeq,
                    "order_id": str(state.orderId),
                    "side": state.side.name,
                    "status": state.status.name,
                    "price": _price_from_ticks(book_key, state.priceTicks),
                    "qty_init": _qty_from_lots(book_key, state.initialQty),
                    "qty_rem": _qty_from_lots(book_key, state.remainingQty),
                    "qty_filled": _qty_from_lots(book_key, state.filledQty),
                    "book": book_key,
                }
            )
    rows.sort(key=lambda r: r["created_seq"])
    for row in rows:
        row["created_seq"] = _safe_int(row["created_seq"])
    _render_table(rows, height_px=260)


def _render_recent_tables(book_filter: str) -> None:
    sink: InMemoryMultiLogSink = st.session_state.sink
    if book_filter == "All":
        events = sink.events()[-100:]
        fills = sink.fills()[-100:]
        diags = sink.diagnostics()[-100:]
    else:
        events = sink.events_for(book_filter)[-100:]
        fills = sink.fills_for(book_filter)[-100:]
        diags = sink.diagnostics_for(book_filter)[-100:]

    st.markdown("<div class='section-title'>Recent Events</div>", unsafe_allow_html=True)
    _render_table(_format_event_rows(list(reversed(events))), height_px=320)
    st.markdown("<div class='section-title'>Recent Fills</div>", unsafe_allow_html=True)
    _render_table(_format_fill_rows(list(reversed(fills))), height_px=320)
    st.markdown("<div class='section-title'>Recent Diagnostics</div>", unsafe_allow_html=True)
    _render_table(_format_diag_rows(list(reversed(diags))), height_px=320)


def main() -> None:
    ensure_session()
    st.set_page_config(page_title="LOBSIM Arbitrage Monitor", layout="wide")
    st.markdown(
        """
        <style>
        @import url('https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;600&family=IBM+Plex+Mono:wght@400;600&display=swap');
        :root {
            --bg: #f7f4ef;
            --ink: #1b1a17;
            --accent: #d6721e;
            --muted: #5f5c56;
            --panel: #fffaf3;
            --border: #e6ddd1;
        }
        .stApp {
            background: radial-gradient(circle at top, #fff 0%, #f7f4ef 50%, #efe7dd 100%);
            color: var(--ink);
        }
        h1, h2, h3, h4, h5, h6, p, label, span, div {
            color: var(--ink);
        }
        section[data-testid="stSidebar"] {
            background: linear-gradient(180deg, #f3eadf 0%, #efe4d6 100%);
        }
        section[data-testid="stSidebar"] * {
            color: var(--ink) !important;
        }
        section[data-testid="stSidebar"] button {
            background: var(--ink) !important;
            color: #ffffff !important;
            border: none !important;
        }
        section[data-testid="stSidebar"] button:hover {
            background: #2d2b27 !important;
            color: #ffffff !important;
        }
        section[data-testid="stSidebar"] button * {
            color: #ffffff !important;
        }
        section[data-testid="stSidebarHeader"] button,
        section[data-testid="stSidebarHeader"] button * {
            color: #ffffff !important;
        }
        section[data-testid="stSidebar"] div[data-baseweb="select"] > div,
        section[data-testid="stSidebar"] input,
        section[data-testid="stSidebar"] textarea,
        section[data-testid="stSidebar"] select,
        section[data-testid="stSidebar"] .stTextInput input,
        section[data-testid="stSidebar"] .stNumberInput input,
        section[data-testid="stSidebar"] .stTextArea textarea {
            background: #ffffff !important;
            color: var(--ink) !important;
            border: 1px solid var(--border) !important;
        }
        section[data-testid="stSidebar"] div[data-baseweb="select"] * {
            color: var(--ink) !important;
        }
        section[data-testid="stSidebar"] li[role="option"],
        div[role="listbox"] {
            background: #ffffff !important;
            color: var(--ink) !important;
            border: 1px solid var(--border) !important;
        }
        li[role="option"][aria-selected="true"],
        section[data-testid="stSidebar"] li[role="option"][aria-selected="true"] {
            background: #efe4d6 !important;
            color: var(--ink) !important;
        }
        div.stButton > button {
            background: var(--ink) !important;
            color: #ffffff !important;
            border: none !important;
            box-shadow: none !important;
        }
        div.stButton > button:hover {
            background: #2d2b27 !important;
            color: #ffffff !important;
        }
        div.stButton > button * {
            color: #ffffff !important;
        }
        div[data-testid="stNumberInput"] button {
            background: #ffffff !important;
            color: var(--ink) !important;
            border: 1px solid var(--border) !important;
        }
        div[data-testid="stNumberInput"] button * {
            color: var(--ink) !important;
        }
        .lobsim-table-wrap {
            overflow-y: auto;
            border: 1px solid var(--border);
            border-radius: 12px;
            background: var(--panel);
        }
        table.lobsim-table {
            width: 100%;
            border-collapse: collapse;
            font-size: 0.85rem;
            color: var(--ink);
        }
        table.lobsim-table th,
        table.lobsim-table td {
            padding: 6px 8px;
            border-bottom: 1px solid var(--border);
        }
        table.lobsim-table th {
            background: #f1e7da;
            position: sticky;
            top: 0;
            z-index: 1;
        }
        table.lobsim-table tr:nth-child(even) td {
            background: #f9f2e8;
        }
        .section-title {
            font-family: "Space Grotesk", sans-serif;
            font-size: 1.1rem;
            font-weight: 600;
            margin: 0.4rem 0 0.2rem 0;
        }
        </style>
        """,
        unsafe_allow_html=True,
    )

    st.title("Arbitrage dashboard with LOBSIM")
    st.caption(
        "Fixed books: Coinbase BTC-USDT, Binance BTC-USDT, Coinbase ETH-USDT. "
        "Replay is deterministic and synchronized across books." 
    )

    with st.sidebar:
        st.header("Runner")
        batch_size = st.slider("Batch size", min_value=50, max_value=2000, value=200, step=50)
        require_monotonic = st.checkbox("Require monotonic tsReceived", value=True)
        fail_fast = st.checkbox("Fail fast", value=False)
        init_btn = st.button("Initialize / Reset")

        st.header("Step")
        step_mode = st.radio("Step mode", ["Events", "Time (ms)"], horizontal=True)
        if step_mode == "Events":
            step_n = st.number_input("Step size (events)", value=100, min_value=1, step=50, format="%d")
            col_step = st.columns(2)
            with col_step[0]:
                step_once = st.button("Step 1")
            with col_step[1]:
                step_n_btn = st.button(f"Step {step_n}")
            step_ms = 0
            step_time_btn = False
        else:
            step_ms = st.number_input("Step size (ms)", value=1000, min_value=1, step=100, format="%d")
            step_once = False
            step_n_btn = False
            step_time_btn = st.button(f"Step {step_ms} ms")

        st.header("Arbitrage Strategy")
        enable_strategy = st.checkbox("Enable strategy", value=False)
        arb_threshold = st.number_input(
            "Threshold (price units)", value=0.5, min_value=0.0, step=0.1, format="%.4f"
        )
        arb_qty = st.number_input("Quantity (BTC)", value=0.001, min_value=1e-6, format="%.6f")
        min_interval_ms = st.number_input("Min interval (ms)", value=250, min_value=0, step=50, format="%d")
        latency_cb_ms = st.number_input("Coinbase latency (ms)", value=0, min_value=0, step=1, format="%d")
        latency_bn_ms = st.number_input("Binance latency (ms)", value=0, min_value=0, step=1, format="%d")
        decision_mode = st.radio(
            "Decision frequency",
            ["Every event", "Every N events", "Every N ms"],
            horizontal=True,
        )
        if decision_mode == "Every N events":
            decision_value = st.number_input("Decision every N events", value=10, min_value=1, step=1, format="%d")
        elif decision_mode == "Every N ms":
            decision_value = st.number_input("Decision every N ms", value=1000, min_value=1, step=100, format="%d")
        else:
            decision_value = 1

    if init_btn:
        init_simulator(batch_size=batch_size, require_monotonic=require_monotonic, fail_fast=fail_fast)

    if st.session_state.sim is None:
        st.info("Initialize from the sidebar to start replaying.")
        st.markdown("<div class='section-title'>Book specs</div>", unsafe_allow_html=True)
        _render_table(_format_specs_table(), height_px=180)
        return

    strategy_params = {
        "threshold": arb_threshold,
        "qty": arb_qty,
        "min_interval_ms": int(min_interval_ms),
        "latency_cb_ms": int(latency_cb_ms),
        "latency_bn_ms": int(latency_bn_ms),
    }

    if step_once and step_mode == "Events":
        step_events(
            1,
            run_strategy=bool(enable_strategy),
            strategy_params=strategy_params,
            decision_mode=decision_mode,
            decision_value=int(decision_value),
        )
    if step_n_btn and step_mode == "Events":
        step_events(
            int(step_n),
            run_strategy=bool(enable_strategy),
            strategy_params=strategy_params,
            decision_mode=decision_mode,
            decision_value=int(decision_value),
        )
    if step_time_btn and step_mode == "Time (ms)":
        step_time(
            int(step_ms),
            run_strategy=bool(enable_strategy),
            strategy_params=strategy_params,
            decision_mode=decision_mode,
            decision_value=int(decision_value),
        )

    sim: MultiBookSimulator = st.session_state.sim

    st.markdown("<div class='section-title'>Book specs</div>", unsafe_allow_html=True)
    _render_table(_format_specs_table(), height_px=180)

    st.markdown("<div class='section-title'>Live book metrics</div>", unsafe_allow_html=True)
    cols = st.columns(3)
    for idx, spec in enumerate(BOOK_SPECS):
        key = spec.book_key
        bid = sim.get_best_price_ticks(st.session_state.book_ids[key], Side.BUY)
        ask = sim.get_best_price_ticks(st.session_state.book_ids[key], Side.SELL)
        mid = _latest_value(st.session_state.mid_history[key])
        spread = _latest_value(st.session_state.spread_history[key])
        ofi = _latest_value(st.session_state.ofi_history[key])
        tfi = _latest_value(st.session_state.tfi_history[key])
        events_count = st.session_state.book_counts[key]["events"]
        fills_count = st.session_state.book_counts[key]["fills"]

        with cols[idx]:
            st.subheader(spec.label)
            st.metric("Best bid", "-" if bid is None else f"{bid * spec.tick_size:.2f}")
            st.metric("Best ask", "-" if ask is None else f"{ask * spec.tick_size:.2f}")
            st.metric("Spread", "-" if spread is None else f"{spread:.4f}")
            st.metric("Mid", "-" if mid is None else f"{mid:.2f}")
            st.metric("Order flow imbalance", f"{(ofi or 0.0):.2f}")
            st.metric("Trade imbalance", f"{(tfi or 0.0):.2f}")
            st.caption(f"Events: {events_count} | Fills: {fills_count}")

    st.markdown("<div class='section-title'>BTC cross-venue opportunity</div>", unsafe_allow_html=True)
    cb_key, bn_key = BTC_BOOK_KEYS
    cb_bid = sim.get_best_price_ticks(st.session_state.book_ids[cb_key], Side.BUY)
    cb_ask = sim.get_best_price_ticks(st.session_state.book_ids[cb_key], Side.SELL)
    bn_bid = sim.get_best_price_ticks(st.session_state.book_ids[bn_key], Side.BUY)
    bn_ask = sim.get_best_price_ticks(st.session_state.book_ids[bn_key], Side.SELL)
    edge_buy_cb_sell_bn = None
    edge_buy_bn_sell_cb = None
    if cb_bid is not None and cb_ask is not None and bn_bid is not None and bn_ask is not None:
        cb_spec = st.session_state.book_specs_by_key[cb_key]
        bn_spec = st.session_state.book_specs_by_key[bn_key]
        edge_buy_cb_sell_bn = (bn_bid * bn_spec.tick_size) - (cb_ask * cb_spec.tick_size)
        edge_buy_bn_sell_cb = (cb_bid * cb_spec.tick_size) - (bn_ask * bn_spec.tick_size)

    col_edge = st.columns(3)
    col_edge[0].metric("Buy Coinbase / Sell Binance", "-" if edge_buy_cb_sell_bn is None else f"{edge_buy_cb_sell_bn:.4f}")
    col_edge[1].metric("Buy Binance / Sell Coinbase", "-" if edge_buy_bn_sell_cb is None else f"{edge_buy_bn_sell_cb:.4f}")
    col_edge[2].metric("Last strategy action", st.session_state.last_action)

    st.markdown("<div class='section-title'>BTC mid comparison</div>", unsafe_allow_html=True)
    _plot_btc_mid()

    st.markdown("<div class='section-title'>ETH mid</div>", unsafe_allow_html=True)
    _plot_eth_mid()

    st.markdown("<div class='section-title'>BTC arbitrage edges</div>", unsafe_allow_html=True)
    _plot_arb_edges()

    st.markdown("<div class='section-title'>Strategy fills</div>", unsafe_allow_html=True)
    _strategy_fills_table()

    st.markdown("<div class='section-title'>Strategy orders</div>", unsafe_allow_html=True)
    order_view = st.radio("Strategy Orders", ["Active", "Closed", "All"], horizontal=True, label_visibility="collapsed")
    _strategy_orders_table(order_view)

    st.markdown("<div class='section-title'>Observability</div>", unsafe_allow_html=True)
    book_filter = st.selectbox(
        "Filter by book",
        ["All"] + [spec.book_key for spec in BOOK_SPECS],
        index=0,
    )
    _render_recent_tables(book_filter)


if __name__ == "__main__":
    main()
