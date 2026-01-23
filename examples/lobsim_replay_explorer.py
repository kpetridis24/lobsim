from __future__ import annotations

import sys
from pathlib import Path

import html

import streamlit as st

# Allow running from repo without reinstalling the package
REPO_PY = Path(__file__).resolve().parents[1] / "python"
if str(REPO_PY) not in sys.path:
    sys.path.append(str(REPO_PY))
from lobsim import BookId
from lobsim.demo_utils import (
    Adapter,
    ParquetStream,
    find_snapshot_path,
    load_l3_snapshot,
    parse_update_type,
    to_ticks,
)
from lobsim.lob_event import NormalizedLobEvent
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
                "ts_exchange": _safe_int(ev.ts_exchange),
                "ts_received": _safe_int(ev.ts_received),
                "side": _enum_label(ev.side),
                "update_type": _enum_label(ev.update_type),
                "source": _enum_label(ev.source),
                "price": _price_from_ticks(ev.price_ticks),
                "qty": _qty_from_lots(ev.qty_lots),
                "price_ticks": ev.price_ticks,
                "qty_lots": ev.qty_lots,
                "order_id": str(ev.order_id),
                "trader_id": str(ev.trader_id),
                "aggressor_id": str(ev.aggressor_id),
                "book": ev.book_key,
            }
        )
    return rows


def _format_fill_rows(fills: list[object]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for fill in fills:
        rows.append(
            {
                "seq": fill.seq,
                "ts_exchange": _safe_int(fill.ts_exchange),
                "ts_received": _safe_int(fill.ts_received),
                "price": _price_from_ticks(fill.price_ticks),
                "qty": _qty_from_lots(fill.qty_lots),
                "price_ticks": fill.price_ticks,
                "qty_lots": fill.qty_lots,
                "maker_side": _enum_label(fill.maker_side),
                "maker_order_id": str(fill.maker_order_id),
                "maker_trader_id": str(fill.maker_trader_id),
                "maker_source": _enum_label(fill.maker_source),
                "taker_side": _enum_label(fill.taker_side),
                "taker_order_id": str(fill.taker_order_id),
                "taker_trader_id": str(fill.taker_trader_id),
                "taker_source": _enum_label(fill.taker_source),
                "book": fill.book_key,
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
                "ts_exchange": _safe_int(diag.ts_exchange),
                "ts_received": _safe_int(diag.ts_received),
                "code": code_name,
                "severity": severity_name,
                "code_id": code_id,
                "severity_id": severity_id,
                "book": diag.book_key,
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
                "_created_seq": state.created_seq,
                "created_seq": _safe_int(state.created_seq),
                "last_update_seq": _safe_int(state.last_update_seq),
                "order_id": str(state.order_id),
                "side": _enum_label(state.side),
                "price": _price_from_ticks(state.price_ticks),
                "qty_initial": _qty_from_lots(state.initial_qty),
                "qty_remaining": _qty_from_lots(state.remaining_qty),
                "qty_filled": _qty_from_lots(state.filled_qty),
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
                snap_path,
                tick_size=tick_size,
                lot_size=lot_size,
                batch_size=batch_size,
                symbol_id=book_id.book_key,
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
        require_monotonic = st.checkbox("Require monotonic ts_received", value=True)
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
                    UpdateType.AGGRESSIVE_TRADE,
                ],
            )
            side = st.selectbox("Side", [Side.BUY, Side.SELL])
            price_ticks = st.number_input(
                "Price ticks (ignored for AGGRESSIVE_TRADE)",
                value=0,
                step=1,
                disabled=update_type == UpdateType.AGGRESSIVE_TRADE,
            )
            qty_lots = st.number_input("Quantity lots", value=1, step=1)
            order_id = st.number_input("Order ID", value=123456, step=1)
            latency = st.number_input("Latency (us)", value=1, step=1)
            if st.button("Submit strategy event"):
                ev = NormalizedLobEvent(
                    ts_exchange=0,
                    ts_received=0,
                    side=side,
                    update_type=update_type,
                    price_ticks=int(price_ticks),
                    quantity_lots=int(qty_lots),
                    order_id=int(order_id),
                    trader_id=int(order_id),
                    aggressor_id=UnknownAggressorIdSentinel,
                    update_source=UpdateSource.STRATEGY,
                    symbol_id="",
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
                xaxis_title="ts_received (us)",
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
            if (fill.maker_source == UpdateSource.STRATEGY or fill.taker_source == UpdateSource.STRATEGY)
        ]
        _render_table(
            _format_fill_rows(list(reversed(strategy_fills[-100:]))),
            height_px=370,
        )

        st.markdown('<div class="section-title">Strategy Orders</div>', unsafe_allow_html=True)
        orders_view = st.radio(
            "",
            ["Active", "Closed", "All"],
            horizontal=True,
            key="strategy_orders_view",
            label_visibility="collapsed",
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
