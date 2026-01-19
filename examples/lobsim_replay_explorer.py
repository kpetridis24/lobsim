from __future__ import annotations

import datetime as dt
import html
import hashlib
from dataclasses import dataclass
from decimal import ROUND_HALF_UP, Decimal
from pathlib import Path
from typing import Iterable

import pyarrow.parquet as pq
import streamlit as st
from lobsim.lob_event import NormalizedLobEvent
from lobsim import BookId
from lobsim.multibook import Config, MultiBookSimulator
from lobsim.sink import InMemoryMultiLogSink
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
from plotly import graph_objects as go


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
        recv_col = next((n for n in schema_names if n in ("time_received", "time_feed")), None)
        if recv_col is None:
            candidates = [n for n in schema_names if "time" in n and "exchange" not in n]
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
        self._batch_iter = self._pq.iter_batches(columns=self._cols, batch_size=self._batch_size)
        self._batch = None
        self._row = 0

    def __iter__(self) -> Iterable[RawEvent]:
        return self

    def __next__(self) -> RawEvent:
        while True:
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
        )


def find_snapshot_path(data_path: Path) -> Path | None:
    if data_path.name.endswith("_snap.parquet") and data_path.exists():
        return data_path
    candidate = data_path.with_name(f"{data_path.stem}_snap{data_path.suffix}")
    if candidate.exists():
        return candidate
    return None


def load_l3_snapshot(path: Path, *, tick_size: float, lot_size: float, batch_size: int = 4096):
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
    seen: set[int] = set()
    duplicate_count = 0
    for batch in pf.iter_batches(columns=cols, batch_size=batch_size):
        data = {name: batch.column(i) for i, name in enumerate(cols)}
        n = batch.num_rows
        for i in range(n):
            update = str(data["update_type"][i].as_py()).strip().upper()
            if update != "SNAPSHOT":
                continue
            side = Side.BUY if int(data["is_buy"][i].as_py()) else Side.SELL
            price_ticks = to_ticks(float(data["entry_px"][i].as_py()), tick_size, strict=True)
            qty_lots = to_ticks(float(data["entry_sx"][i].as_py()), lot_size, strict=True)
            if qty_lots <= 0:
                continue
            raw_order_id = str(data["order_id"][i].as_py()).strip()
            if not raw_order_id:
                continue
            order_id = stable_int64(raw_order_id)
            if order_id in seen:
                duplicate_count += 1
                continue
            seen.add(order_id)
            sides.append(side)
            prices.append(price_ticks)
            quantities.append(qty_lots)
            order_ids.append(order_id)
            trader_ids.append(UnknownTraderIdSentinel)

    return sides, prices, quantities, order_ids, trader_ids, duplicate_count


def ensure_session() -> None:
    if "sim" not in st.session_state:
        st.session_state.sim = None
        st.session_state.sink = None
        st.session_state.book_id = None
        st.session_state.mid_history = []
        st.session_state.event_count = 0
        st.session_state.fill_count = 0
        st.session_state.diag_count = 0


def _enum_label(value) -> str:
    return value.name if hasattr(value, "name") else str(value)


DIAG_CODE_NAMES = diagnostic_code_names()
DIAG_SEVERITY_NAMES = diagnostic_severity_names()


def _safe_int(value: int) -> str | int:
    if value > (2**53 - 1) or value < -(2**53 - 1):
        return str(value)
    return value


def _price_from_ticks(ticks: int) -> float:
    adapter = st.session_state.adapter
    value = ticks * adapter.tick_size if adapter is not None else float(ticks)
    return round(value, 2)


def _qty_from_lots(lots: int) -> float:
    adapter = st.session_state.adapter
    value = lots * adapter.lot_size if adapter is not None else float(lots)
    return round(value, 8)


def _format_levels(levels: list[tuple[int, int]]) -> list[dict[str, object]]:
    return [
        {
            "price": _price_from_ticks(price_ticks),
            "qty": _qty_from_lots(qty_lots),
            "price_ticks": price_ticks,
            "qty_lots": qty_lots,
        }
        for price_ticks, qty_lots in levels
    ]


def _format_event_rows(events: list[object]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for ev in events:
        rows.append(
            {
                "seq": ev.seq,
                "ts_exchange": _safe_int(ev.tsExchange),
                "ts_received": _safe_int(ev.tsReceived),
                "side": _enum_label(ev.side),
                "update_type": _enum_label(ev.updateType),
                "source": _enum_label(ev.source),
                "price": _price_from_ticks(ev.priceTicks),
                "qty": _qty_from_lots(ev.qtyLots),
                "price_ticks": ev.priceTicks,
                "qty_lots": ev.qtyLots,
                "order_id": str(ev.orderId),
                "trader_id": str(ev.traderId),
                "aggressor_id": str(ev.aggressorId),
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
                "price": _price_from_ticks(fill.priceTicks),
                "qty": _qty_from_lots(fill.qtyLots),
                "price_ticks": fill.priceTicks,
                "qty_lots": fill.qtyLots,
                "maker_side": _enum_label(fill.makerSide),
                "maker_order_id": str(fill.makerOrderId),
                "maker_trader_id": str(fill.makerTraderId),
                "maker_source": _enum_label(fill.makerSource),
                "taker_side": _enum_label(fill.takerSide),
                "taker_order_id": str(fill.takerOrderId),
                "taker_trader_id": str(fill.takerTraderId),
                "taker_source": _enum_label(fill.takerSource),
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
            code_id = ""
        try:
            severity_id = int(diag.severity)
        except Exception:
            severity_id = ""
        code_name = DIAG_CODE_NAMES.get(code_id) or f"CODE_{code_id}"
        severity_name = DIAG_SEVERITY_NAMES.get(severity_id) or f"SEVERITY_{severity_id}"
        code_name = code_name.encode("ascii", "backslashreplace").decode("ascii")
        severity_name = severity_name.encode("ascii", "backslashreplace").decode("ascii")
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


def _format_strategy_order_rows(
    ledger: dict[int, object], *, view: str, book_key: str
) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for _, entry in ledger.items():
        state = entry.state
        status_label = _enum_label(state.status)
        is_active = status_label in ("OPEN", "PARTIALLY_FILLED")
        if view == "Active" and not is_active:
            continue
        if view == "Closed" and is_active:
            continue
        rows.append(
            {
                "_created_seq": state.createdSeq,
                "created_seq": _safe_int(state.createdSeq),
                "last_update_seq": _safe_int(state.lastUpdateSeq),
                "order_id": str(state.orderId),
                "side": _enum_label(state.side),
                "price": _price_from_ticks(state.priceTicks),
                "qty_initial": _qty_from_lots(state.initialQty),
                "qty_remaining": _qty_from_lots(state.remainingQty),
                "qty_filled": _qty_from_lots(state.filledQty),
                "status": status_label,
                "num_fills": len(entry.fills),
                "book": book_key,
            }
        )
    rows.sort(key=lambda row: row["_created_seq"])
    for row in rows:
        row.pop("_created_seq", None)
    return rows


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


def init_simulator(
    path: Path,
    *,
    venue: str,
    symbol: str,
    tick_size: float,
    lot_size: float,
    batch_size: int,
    require_monotonic: bool,
    fail_fast: bool,
) -> None:
    sim = MultiBookSimulator(Config(require_monotonic_ts_received=require_monotonic, fail_fast=fail_fast))
    sink = InMemoryMultiLogSink()
    sim.set_multi_log_sink(sink)

    book_id = BookId(venue=venue, symbol=symbol)
    sim.add_book(book_id)

    snap_path = find_snapshot_path(path)
    if snap_path is not None:
        try:
            sides, prices, quantities, order_ids, trader_ids, dupes = load_l3_snapshot(
                snap_path, tick_size=tick_size, lot_size=lot_size, batch_size=batch_size
            )
            if sides:
                sim.init_from_l3_snapshot(book_id, sides, prices, quantities, order_ids, trader_ids)
                message = f"Loaded snapshot: {snap_path.name} ({len(sides)} orders)"
                if dupes:
                    message += f", skipped {dupes} duplicate order_ids"
                st.success(message)
            else:
                st.warning(f"Snapshot {snap_path.name} had no usable levels.")
        except Exception as exc:
            st.warning(f"Failed to load snapshot {snap_path.name}: {exc}")
    else:
        st.warning("Snapshot file not found. Starting from an empty book.")

    source = ParquetStream(path, batch_size=batch_size)
    adapter = Adapter(tick_size=tick_size, lot_size=lot_size, symbol_id=book_id.book_key)
    sim.add_stream(book_id, source, adapter)

    st.session_state.sim = sim
    st.session_state.sink = sink
    st.session_state.book_id = book_id
    st.session_state.source = source
    st.session_state.adapter = adapter
    st.session_state.mid_history = []
    st.session_state.event_count = 0
    st.session_state.fill_count = 0
    st.session_state.diag_count = 0


def update_mid_history() -> None:
    sim = st.session_state.sim
    book_id = st.session_state.book_id
    adapter = st.session_state.adapter
    if sim is None or book_id is None:
        return
    if adapter is None:
        return

    best_bid = sim.get_best_price_ticks(book_id, Side.BUY)
    best_ask = sim.get_best_price_ticks(book_id, Side.SELL)
    if best_bid is None or best_ask is None:
        return

    ts = sim.current_time()
    if ts is None:
        return

    bid_px = best_bid * adapter.tick_size
    ask_px = best_ask * adapter.tick_size
    mid = (bid_px + ask_px) / 2.0
    st.session_state.mid_history.append(
        {"ts": ts, "mid": mid, "bid": bid_px, "ask": ask_px, "bid_ticks": best_bid, "ask_ticks": best_ask}
    )
    if len(st.session_state.mid_history) > 2000:
        st.session_state.mid_history = st.session_state.mid_history[-2000:]


def step_n(n: int) -> int:
    sim = st.session_state.sim
    sink = st.session_state.sink
    if sim is None or sink is None:
        return 0

    applied = 0
    for _ in range(n):
        if not sim.step():
            break
        applied += 1
        update_mid_history()

    st.session_state.event_count = len(sink.events())
    st.session_state.fill_count = len(sink.fills())
    st.session_state.diag_count = len(sink.diagnostics())
    return applied


def step_for(delta_us: int) -> int:
    sim = st.session_state.sim
    sink = st.session_state.sink
    if sim is None or sink is None:
        return 0

    if delta_us <= 0:
        return 0

    base = sim.current_time() or 0
    target = base + delta_us
    applied = 0
    while sim.step():
        applied += 1
        update_mid_history()
        current = sim.current_time()
        if current is not None and current >= target:
            break

    st.session_state.event_count = len(sink.events())
    st.session_state.fill_count = len(sink.fills())
    st.session_state.diag_count = len(sink.diagnostics())
    return applied


def render_header() -> None:
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
            --table-ink: #3b352f;
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
        section[data-testid="stSidebar"] input,
        section[data-testid="stSidebar"] textarea,
        section[data-testid="stSidebar"] select {
            background: #ffffff !important;
            color: var(--ink) !important;
            border: 1px solid var(--border) !important;
        }
        section[data-testid="stSidebar"] .stTextInput input,
        section[data-testid="stSidebar"] .stNumberInput input,
        section[data-testid="stSidebar"] .stTextArea textarea {
            background: #ffffff !important;
            color: var(--ink) !important;
            border: 1px solid var(--border) !important;
        }
        section[data-testid="stSidebar"] div[data-baseweb="select"] > div {
            background: #ffffff !important;
            color: var(--ink) !important;
            border: 1px solid var(--border) !important;
        }
        section[data-testid="stSidebar"] div[data-baseweb="select"] * {
            color: var(--ink) !important;
        }
        section[data-testid="stSidebar"] div[data-baseweb="popover"] {
            background: #ffffff !important;
            color: var(--ink) !important;
            border: 1px solid var(--border) !important;
        }
        section[data-testid="stSidebar"] div[data-baseweb="popover"] * {
            color: var(--ink) !important;
        }
        section[data-testid="stSidebar"] li[role="option"] {
            background: #ffffff !important;
            color: var(--ink) !important;
        }
        section[data-testid="stSidebar"] li[role="option"][aria-selected="true"] {
            background: #efe4d6 !important;
            color: var(--ink) !important;
        }
        div[role="listbox"] {
            background: #ffffff !important;
            color: var(--ink) !important;
            border: 1px solid var(--border) !important;
        }
        div[role="listbox"] * {
            color: var(--ink) !important;
        }
        li[role="option"] {
            background: #ffffff !important;
            color: var(--ink) !important;
        }
        li[role="option"][aria-selected="true"] {
            background: #efe4d6 !important;
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
        section[data-testid="stSidebar"] div[data-testid="stNumberInput"] button {
            background: #ffffff !important;
            color: var(--ink) !important;
            border: 1px solid var(--border) !important;
        }
        section[data-testid="stSidebar"] div[data-testid="stNumberInput"] button * {
            color: var(--ink) !important;
        }
        .title { font-family: "Space Grotesk", sans-serif; font-size: 2.6rem; font-weight: 600; }
        .subtitle { font-family: "Space Grotesk", sans-serif; font-size: 1.1rem; color: var(--muted); }
        .section-title { font-family: "Space Grotesk", sans-serif; font-size: 1.25rem; font-weight: 600; margin-top: 0.5rem; }
        .metric-label { font-family: "IBM Plex Mono", monospace; font-size: 0.8rem; color: var(--muted); text-transform: uppercase; letter-spacing: 0.08em; }
        .metric-value { font-family: "Space Grotesk", sans-serif; font-size: 1.4rem; font-weight: 600; }
        div[data-testid="stDataFrame"], div[data-testid="stTable"] {
            background: var(--panel);
            border: 1px solid var(--border);
            border-radius: 12px;
        }
        div[data-testid="stDataFrame"] * , div[data-testid="stTable"] * {
            color: var(--table-ink) !important;
        }
        div[data-testid="stDataFrame"] .ag-theme-streamlit,
        div[data-testid="stDataFrame"] .ag-theme-alpine,
        div[data-testid="stDataFrame"] .ag-theme-alpine-dark {
            --ag-background-color: #fffaf3;
            --ag-foreground-color: #3b352f;
            --ag-header-background-color: #f6efe6;
            --ag-header-foreground-color: #3b352f;
            --ag-odd-row-background-color: #fbf7f1;
            --ag-even-row-background-color: #fffaf3;
            --ag-border-color: #e6ddd1;
            --ag-row-hover-color: #efe4d6;
            --ag-input-background-color: #ffffff;
        }
        div[data-testid="stDataFrame"] .ag-root,
        div[data-testid="stDataFrame"] .ag-body-viewport,
        div[data-testid="stDataFrame"] .ag-center-cols-container {
            background: #fffaf3 !important;
        }
        div[data-testid="stTable"] table {
            background: #fffaf3 !important;
        }
        div[data-testid="stTable"] th,
        div[data-testid="stTable"] td {
            color: var(--table-ink) !important;
        }
        div[data-testid="stDataFrame"] .ag-root-wrapper,
        div[data-testid="stDataFrame"] .ag-root,
        div[data-testid="stDataFrame"] .ag-body-viewport,
        div[data-testid="stDataFrame"] .ag-header,
        div[data-testid="stDataFrame"] .ag-row,
        div[data-testid="stDataFrame"] .ag-cell,
        div[data-testid="stDataFrame"] .ag-cell-value,
        div[data-testid="stDataFrame"] .ag-header-cell-text,
        div[data-testid="stDataFrame"] .ag-floating-filter-input,
        div[data-testid="stDataFrame"] .ag-input-field-input,
        div[data-testid="stDataFrame"] .ag-header-icon,
        div[data-testid="stDataFrame"] .ag-icon {
            color: var(--table-ink) !important;
            background: var(--panel) !important;
        }
        div[data-testid="stDataFrame"] .ag-header {
            background: #f6efe6 !important;
        }
        div[data-testid="stDataFrame"] .ag-row:nth-child(even) {
            background: #fbf7f1 !important;
        }
        .lobsim-table-wrap {
            overflow-y: auto;
            overflow-x: auto;
            border: 1px solid var(--border);
            border-radius: 12px;
            background: var(--panel);
        }
        table.lobsim-table {
            width: max-content;
            min-width: 100%;
            border-collapse: collapse;
            font-family: "IBM Plex Mono", monospace;
            font-size: 0.85rem;
            color: var(--table-ink);
        }
        table.lobsim-table th,
        table.lobsim-table td {
            border-bottom: 1px solid var(--border);
            padding: 6px 10px;
            text-align: left;
            white-space: nowrap;
            word-break: normal;
        }
        table.lobsim-table th {
            background: #f6efe6;
            position: sticky;
            top: 0;
            z-index: 1;
        }
        table.lobsim-table tr:nth-child(even) td {
            background: #fbf7f1;
        }
        div[data-testid="stDataFrame"] [data-testid="stToolbar"] {
            background: var(--panel) !important;
            color: var(--table-ink) !important;
            border: 1px solid var(--border) !important;
            border-radius: 8px !important;
        }
        div[data-testid="stDataFrame"] [data-testid="stToolbar"] button {
            color: var(--table-ink) !important;
        }
        div[data-testid="stDataFrame"] [data-testid="stToolbar"] svg {
            fill: var(--table-ink) !important;
        }
        div[role="menu"] {
            background: var(--panel) !important;
            color: var(--table-ink) !important;
            border: 1px solid var(--border) !important;
        }
        div[role="menu"] * {
            color: var(--table-ink) !important;
        }
        div[data-testid="stAlert"] {
            background: #fff3e6;
            border: 1px solid var(--border);
        }
        div[data-testid="stAlert"] * { color: var(--ink) !important; }
        </style>
        """,
        unsafe_allow_html=True,
    )
    st.markdown('<div class="title">Replay Explorer</div>', unsafe_allow_html=True)
    st.markdown(
        '<div class="subtitle">Deterministic event replay with step-wise inspection and strategy injection.</div>',
        unsafe_allow_html=True,
    )


def main() -> None:
    st.set_page_config(page_title="lobsim Replay Explorer", layout="wide")
    render_header()
    ensure_session()

    with st.sidebar:
        st.header("Session")
        data_path = Path(
            st.text_input("Parquet path", value="sample_data/coinbase_btcusdt_sample_big.parquet")
        )
        venue = st.text_input("Venue", value="coinbase")
        symbol = st.text_input("Symbol", value="BTC-USDT")
        tick_size = st.number_input("Tick size", value=0.01, format="%.8f")
        lot_size = st.number_input("Lot size", value=1e-8, format="%.10f")
        batch_size = st.number_input("Batch size", value=100, step=200)
        require_monotonic = st.checkbox("Require monotonic tsReceived", value=True)
        fail_fast = st.checkbox("Fail fast", value=False)

        if st.button("Initialize / Reset"):
            init_simulator(
                data_path,
                venue=venue,
                symbol=symbol,
                tick_size=tick_size,
                lot_size=lot_size,
                batch_size=int(batch_size),
                require_monotonic=require_monotonic,
                fail_fast=fail_fast,
            )
            st.success("Simulator initialized.")

        st.divider()
        st.header("Replay Controls")
        step_events = st.number_input("Step N events", value=100, step=50)
        step_window = st.number_input("Step window (microseconds)", value=50_000, step=10_000)
        if st.button("Step N"):
            applied = step_n(int(step_events))
            st.info(f"Applied {applied} events.")
        if st.button("Step Window"):
            applied = step_for(int(step_window))
            st.info(f"Applied {applied} events.")

        st.divider()
        st.header("Strategy Injection")
        if st.session_state.book_id is not None:
            update_type = st.selectbox(
                "Update type",
                [
                    UpdateType.ADD,
                    UpdateType.DELETE,
                    UpdateType.SUBTRACT,
                    UpdateType.MATCH,
                    UpdateType.SET,
                ],
            )
            side = st.selectbox("Side", [Side.BUY, Side.SELL])
            price_ticks = st.number_input("Price ticks", value=0, step=1)
            qty_lots = st.number_input("Quantity lots", value=1, step=1)
            order_id = st.number_input("Order ID", value=123456, step=1)
            latency = st.number_input("Latency (us)", value=1, step=1)
            if st.button("Submit strategy event"):
                ev = NormalizedLobEvent(
                    tsExchange=0,
                    tsReceived=0,
                    side=side,
                    updateType=update_type,
                    priceTicks=int(price_ticks),
                    quantityLots=int(qty_lots),
                    orderId=int(order_id),
                    traderId=int(order_id),
                    aggressorId=UnknownAggressorIdSentinel,
                    updateSource=UpdateSource.STRATEGY,
                    symbolId="",
                )
                st.session_state.sim.submit_strategy_event(
                    st.session_state.book_id, ev, int(latency)
                )
                st.success(f"Strategy {update_type.name} queued.")

    sim = st.session_state.sim
    sink = st.session_state.sink

    if sim is None or sink is None:
        st.warning("Initialize the simulator from the sidebar to begin.")
        return

    adapter = st.session_state.adapter
    col_a, col_b, col_c, col_d = st.columns(4)
    with col_a:
        st.markdown('<div class="metric-label">Current time (us)</div>', unsafe_allow_html=True)
        st.markdown(
            f'<div class="metric-value">{sim.current_time() or "—"}</div>',
            unsafe_allow_html=True,
        )
    with col_b:
        st.markdown('<div class="metric-label">Events / Fills / Diagnostics</div>', unsafe_allow_html=True)
        st.markdown(
            f'<div class="metric-value">{st.session_state.event_count} / '
            f'{st.session_state.fill_count} / {st.session_state.diag_count}</div>',
            unsafe_allow_html=True,
        )
    with col_c:
        st.markdown('<div class="metric-label">Book</div>', unsafe_allow_html=True)
        st.markdown(
            f'<div class="metric-value">{st.session_state.book_id.book_key if st.session_state.book_id else "—"}</div>',
            unsafe_allow_html=True,
        )
    with col_d:
        st.markdown('<div class="metric-label">Spread</div>', unsafe_allow_html=True)
        spread_value = "—"
        if st.session_state.book_id is not None:
            best_bid = sim.get_best_price_ticks(st.session_state.book_id, Side.BUY)
            best_ask = sim.get_best_price_ticks(st.session_state.book_id, Side.SELL)
            if best_bid is not None and best_ask is not None:
                spread_ticks = best_ask - best_bid
                if adapter is not None:
                    spread_value = f"{spread_ticks} ticks / {spread_ticks * adapter.tick_size:.2f}"
                else:
                    spread_value = f"{spread_ticks} ticks"
        st.markdown(f'<div class="metric-value">{spread_value}</div>', unsafe_allow_html=True)

    left, right = st.columns([2, 1])

    with left:
        st.markdown('<div class="section-title">Mid Price (last 2,000 points)</div>', unsafe_allow_html=True)
        if st.session_state.mid_history:
            data = st.session_state.mid_history
            fig = go.Figure()
            fig.add_trace(
                go.Scatter(
                    x=[d["ts"] for d in data],
                    y=[d["mid"] for d in data],
                    mode="lines",
                    line=dict(color="#d6721e", width=2),
                    name="mid",
                )
            )
            fig.update_layout(
                margin=dict(l=10, r=10, t=20, b=20),
                paper_bgcolor="rgba(0,0,0,0)",
                plot_bgcolor="rgba(255,250,243,0.8)",
                font=dict(color="#1b1a17"),
                xaxis_title="tsReceived (us)",
                yaxis_title="mid (price)",
                xaxis=dict(
                    showgrid=True,
                    gridcolor="#e6ddd1",
                    tickfont=dict(color="#1b1a17"),
                    title_font=dict(color="#1b1a17"),
                ),
                yaxis=dict(
                    showgrid=True,
                    gridcolor="#e6ddd1",
                    tickfont=dict(color="#1b1a17"),
                    title_font=dict(color="#1b1a17"),
                ),
            )
            st.plotly_chart(fig, use_container_width=True)
        else:
            st.info("No price history yet. Step the replay to populate the chart.")

        st.markdown('<div class="section-title">Strategy Fills</div>', unsafe_allow_html=True)
        strategy_fills = [
            fill
            for fill in sink.fills()
            if (fill.makerSource == UpdateSource.STRATEGY or fill.takerSource == UpdateSource.STRATEGY)
        ]
        _render_table(
            _format_fill_rows(list(reversed(strategy_fills[-100:]))),
            height_px=370,
        )

        st.markdown('<div class="section-title">Strategy Orders</div>', unsafe_allow_html=True)
        orders_view = st.radio(
            "Show",
            ["Active", "Closed", "All"],
            horizontal=True,
            key="strategy_orders_view",
        )
        book_id = st.session_state.book_id
        if book_id is None:
            order_rows = []
        else:
            ledger = sink.paper_ledger_for(book_id.book_key)
            order_rows = _format_strategy_order_rows(
                ledger, view=orders_view, book_key=book_id.book_key
            )
        _render_table(order_rows[-100:], height_px=360)

    with right:
        st.markdown('<div class="section-title">Order Book (Top 10)</div>', unsafe_allow_html=True)
        book_id = st.session_state.book_id
        bids = _format_levels(sim.l2_top_n(book_id, Side.BUY, 10))
        asks = _format_levels(sim.l2_top_n(book_id, Side.SELL, 10))
        st.markdown("**Asks**")
        _render_table(asks, height_px=400)
        st.text(" ")
        st.markdown("**Bids**")
        _render_table(bids, height_px=400)

    st.markdown('<div class="section-title">Recent Events</div>', unsafe_allow_html=True)
    _render_table(
        _format_event_rows(list(reversed(sink.events()[-100:]))),
        height_px=360,
    )

    st.markdown('<div class="section-title">Recent Fills</div>', unsafe_allow_html=True)
    _render_table(
        _format_fill_rows(list(reversed(sink.fills()[-100:]))),
        height_px=320,
    )

    st.markdown('<div class="section-title">Diagnostics</div>', unsafe_allow_html=True)
    _render_table(
        _format_diag_rows(list(reversed(sink.diagnostics()[-100:]))),
        height_px=320,
    )


if __name__ == "__main__":
    main()
