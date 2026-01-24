from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Tuple

import streamlit as st
from plotly import graph_objects as go

# Allow running from repo without reinstalling the package
REPO_PY = Path(__file__).resolve().parents[1] / "python"
if str(REPO_PY) not in sys.path:
    sys.path.append(str(REPO_PY))

from lobsim.demo_utils import (
    Adapter,
    ParquetStream,
    find_snapshot_path,
    load_l3_snapshot,
    to_ticks,
)
from lobsim.engine import PaperTradingSimulator
from lobsim.lob_event import NormalizedLobEvent
from lobsim.sink import InMemoryLogSink, PaperOrderLedgerStatus
from lobsim.types import (
    Side,
    UnknownAggressorIdSentinel,
    UnknownTraderIdSentinel,
    UpdateSource,
    UpdateType,
)


# --------------------------
# Helpers
# --------------------------
def mid_from_engine(engine: PaperTradingSimulator, tick_size: float) -> Tuple[int | None, float | None]:
    bid = engine.get_best_price_ticks(Side.BUY)
    ask = engine.get_best_price_ticks(Side.SELL)
    if bid is None or ask is None:
        return None, None
    mid_ticks = (bid + ask) / 2.0
    return int(mid_ticks), mid_ticks * tick_size


def _iter_strategy_trades(sink: InMemoryLogSink):
    """
    Yield (side, price_ticks, qty_lots) for any fill where STRATEGY participated.
    This captures both resting fills and aggressive trades.
    """
    for r in sink.get_fills():
        if r.maker_source == UpdateSource.STRATEGY:
            yield r.maker_side, r.price_ticks, r.qty_lots
        if r.taker_source == UpdateSource.STRATEGY:
            yield r.taker_side, r.price_ticks, r.qty_lots


def _render_table(rows: list[dict[str, object]], *, height_px: int) -> None:
    if not rows:
        st.info("No data yet.")
        return
    columns = list(rows[0].keys())
    header = "".join(f"<th>{col}</th>" for col in columns)
    body_rows = []
    for row in rows:
        cells = "".join(f"<td>{row.get(col, '')}</td>" for col in columns)
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


def _format_levels(levels: list[tuple[int, int]], *, tick_size: float, lot_size: float) -> list[dict[str, object]]:
    return [
        {
            "price": round(price_ticks * tick_size, 2),
            "qty": round(qty_lots * lot_size, 8),
            "price_ticks": price_ticks,
            "qty_lots": qty_lots,
        }
        for price_ticks, qty_lots in levels
    ]


# --------------------------
# Risk / Account model (beginner-friendly)
# --------------------------
@dataclass
class RiskConfig:
    starting_cash: float = 10_000.0  # USDT
    allow_short: bool = False
    max_position: float = 0.003  # BTC (abs cap)
    max_open_orders_per_side: int = 1
    min_order_interval_ms: int = 250
    max_loss: float = 0.0  # kill switch in USDT (0 = off)


@dataclass
class RiskState:
    cfg: RiskConfig
    last_order_ts: int = 0  # ts_received (us)
    enabled: bool = True
    disabled_reason: str = ""


def _open_orders_summary(
    sink: InMemoryLogSink, tick_size: float, lot_size: float
) -> dict:
    """
    Looks at the paper ledger and computes:
    - open orders count per side
    - reserved cash for BUY orders
    - reserved inventory for SELL orders (qty)
    """
    ledger = sink.get_paper_ledger()

    open_buy = 0
    open_sell = 0
    reserved_cash = 0.0
    reserved_sell_qty = 0.0

    for entry in ledger.values():
        state = entry.state
        if state.status not in (PaperOrderLedgerStatus.OPEN, PaperOrderLedgerStatus.PARTIALLY_FILLED):
            continue

        qty_units = state.remaining_qty * lot_size
        px = state.price_ticks * tick_size

        if state.side == Side.BUY:
            open_buy += 1
            reserved_cash += px * qty_units
        else:
            open_sell += 1
            reserved_sell_qty += qty_units

    return {
        "open_buy": open_buy,
        "open_sell": open_sell,
        "reserved_cash": reserved_cash,
        "reserved_sell_qty": reserved_sell_qty,
    }


def compute_account_metrics(
    sink: InMemoryLogSink,
    *,
    tick_size: float,
    lot_size: float,
    mark_px: float | None,
    fee_bps: float,
    starting_cash: float,
) -> dict:
    """
    This is the important part:
    - we track cash + position from fills
    - equity = cash + position * mark
    - total_pnl = equity - starting_cash

    This avoids “realized goes down while unrealized goes up” confusion.
    """
    fee_rate = fee_bps / 10_000.0

    cash = starting_cash
    position = 0.0
    fees_paid = 0.0

    # For a useful display, we keep an average entry cost for the CURRENT open position.
    avg_cost = 0.0
    fills_count = 0

    pnl_changes: list[float] = []  # cashflow per fill (includes fees)

    trades = list(_iter_strategy_trades(sink))
    if not trades:
        equity = cash
        return {
            "cash": cash,
            "position": 0.0,
            "avg_cost": 0.0,
            "equity": equity,
            "realized": 0.0,
            "unrealized": 0.0,
            "total_pnl": equity - starting_cash,
            "fees": 0.0,
            "fills": 0,
            "sharpe": None,
        }

    for side, price_ticks, qty_lots in trades:
        fills_count += 1
        qty = qty_lots * lot_size
        price = price_ticks * tick_size
        fee = price * qty * fee_rate
        fees_paid += fee

        if side == Side.BUY:
            # cash outflow
            cash -= price * qty + fee
            pnl_changes.append(-(price * qty + fee))

            # inventory update with avg_cost tracking
            if position >= 0:
                # add to long / open new long
                avg_cost = (avg_cost * position + price * qty) / (position + qty)
                position += qty
            else:
                # covering short
                min(qty, -position)
                position += qty
                # if flipped to long, remaining portion has avg_cost=price
                if position > 0:
                    avg_cost = price

        else:  # SELL
            # cash inflow
            cash += price * qty - fee
            pnl_changes.append(price * qty - fee)

            if position <= 0:
                # add to short / open new short
                # avg_cost for a short is the average sale price
                avg_cost = (avg_cost * (-position) + price * qty) / (-position + qty)
                position -= qty
            else:
                # reduce long
                position -= qty
                if position < 0:
                    # flipped to short
                    avg_cost = price

    unrealized = 0.0
    equity = cash
    if mark_px is not None:
        equity = cash + position * mark_px
        if position != 0.0:
            unrealized = (mark_px - avg_cost) * position

    total_pnl = equity - starting_cash
    realized = total_pnl - unrealized  # includes fees automatically

    sharpe = None
    if len(pnl_changes) >= 2:
        mean = sum(pnl_changes) / len(pnl_changes)
        var = sum((x - mean) ** 2 for x in pnl_changes) / (len(pnl_changes) - 1)
        if var > 0:
            sharpe = mean / (var**0.5) * (len(pnl_changes) ** 0.5)

    return {
        "cash": cash,
        "position": position,
        "avg_cost": avg_cost,
        "equity": equity,
        "realized": realized,
        "unrealized": unrealized,
        "total_pnl": total_pnl,
        "fees": fees_paid,
        "fills": fills_count,
        "sharpe": sharpe,
    }


def risk_check_killswitch(risk: RiskState, total_pnl: float):
    if risk.cfg.max_loss > 0 and total_pnl <= -abs(risk.cfg.max_loss):
        risk.enabled = False
        risk.disabled_reason = f"Kill-switch triggered (PnL {total_pnl:.2f} <= -{abs(risk.cfg.max_loss):.2f})."


def risk_can_submit(
    risk: RiskState,
    sink: InMemoryLogSink,
    *,
    tick_size: float,
    lot_size: float,
    mark_px: float | None,
    fee_bps: float,
    current_ts: int,
    side: Side,
    requested_qty_units: float,
    effective_price_units: float,
) -> tuple[bool, float, str]:
    """
    Returns: (ok, clipped_qty_units, reason)

    effective_price_units should be:
      - aggressive BUY -> best_ask
      - aggressive SELL -> best_bid
      - passive -> your limit price estimate
    """
    if not risk.enabled:
        return False, 0.0, f"disabled: {risk.disabled_reason}"

    # Rate limit (order spam guard)
    min_us = int(risk.cfg.min_order_interval_ms * 1000)
    if min_us > 0 and (current_ts - (risk.last_order_ts or 0)) < min_us:
        return False, 0.0, "blocked: rate-limit"

    # Account state from fills
    acct = compute_account_metrics(
        sink,
        tick_size=tick_size,
        lot_size=lot_size,
        mark_px=mark_px,
        fee_bps=fee_bps,
        starting_cash=risk.cfg.starting_cash,
    )

    # Open orders reservation
    open_info = _open_orders_summary(sink, tick_size=tick_size, lot_size=lot_size)

    open_buy = open_info["open_buy"]
    open_sell = open_info["open_sell"]
    reserved_cash = open_info["reserved_cash"]
    reserved_sell_qty = open_info["reserved_sell_qty"]

    # Max open orders per side
    if side == Side.BUY and open_buy >= risk.cfg.max_open_orders_per_side:
        return False, 0.0, "blocked: max_open_buy"
    if side == Side.SELL and open_sell >= risk.cfg.max_open_orders_per_side:
        return False, 0.0, "blocked: max_open_sell"

    # Basic validation
    if requested_qty_units <= 0:
        return False, 0.0, "blocked: invalid_qty"
    if effective_price_units <= 0:
        return False, 0.0, "blocked: invalid_price"

    pos = float(acct["position"])
    cash = float(acct["cash"])

    free_cash = cash - reserved_cash
    free_long_inventory = pos - reserved_sell_qty  # how much you can sell without going negative (if no shorts)

    max_pos = abs(risk.cfg.max_position)

    # Clip qty based on max_position + cash
    clipped_qty = requested_qty_units

    if side == Side.BUY:
        # Position cap: pos + reserved_buy_qty + qty <= max_pos
        # We'll approximate reserved buy qty from reserved cash / price (safe conservative)
        reserved_buy_qty = reserved_cash / max(effective_price_units, 1e-12)
        max_qty_by_pos = max_pos - (pos + reserved_buy_qty)
        if max_qty_by_pos <= 0:
            return False, 0.0, "blocked: max_position"

        # Cash cap: free_cash >= qty * price (no leverage)
        max_qty_by_cash = free_cash / effective_price_units
        clipped_qty = min(clipped_qty, max_qty_by_pos, max_qty_by_cash)

        if clipped_qty <= 0:
            return False, 0.0, "blocked: cash_or_position"
    elif risk.cfg.allow_short:
        # Short allowed: ensure |new_position| <= max_pos
        # new_pos = pos - qty
        max_qty_by_pos = (pos + max_pos)  # qty <= pos + max_pos
        clipped_qty = min(clipped_qty, max_qty_by_pos)
        if clipped_qty <= 0:
            return False, 0.0, "blocked: max_position"

    else:
        # Must have inventory to sell
        max_qty_by_inv = free_long_inventory
        clipped_qty = min(clipped_qty, max_qty_by_inv)
        if clipped_qty <= 0:
            return False, 0.0, "blocked: no_inventory"
    # Snap to lot size using your helper
    clipped_lots = to_ticks(clipped_qty, lot_size, strict=False)
    clipped_units = clipped_lots * lot_size
    if clipped_lots <= 0 or clipped_units <= 0:
        return False, 0.0, "blocked: lot_rounding"

    # Final cap: don't exceed requested
    clipped_units = min(clipped_units, requested_qty_units)
    return True, clipped_units, "ok"


# --------------------------
# Strategy
# --------------------------
def strategy_tick(
    engine: PaperTradingSimulator,
    sink: InMemoryLogSink,
    adapter: Adapter,
    mid_history: List[Tuple[int, float]],
    risk: RiskState,
    *,
    window: int,
    up_thresh: float,
    down_thresh: float,
    exit_hysteresis: float,
    qty: float,
    offset_ticks: int,
    ttl_events: int,
    aggressive_exits: bool,
    take_profit: float,
    next_order_id: int,
    fee_bps: float,
) -> tuple[int, str]:
    if len(mid_history) < window:
        return next_order_id, "waiting_for_window"

    current_ts = mid_history[-1][0]

    # Rolling signal
    window_slice = [p for _, p in mid_history[-window:]]
    current_mid = window_slice[-1]
    window_avg = sum(window_slice) / len(window_slice)
    delta = current_mid - window_avg

    # (Beginner safety) If thresholds are ~0, you'll trade constantly.
    if up_thresh <= 0 and down_thresh <= 0:
        return next_order_id, "blocked: thresholds=0"

    # Cancel stale orders by TTL (always allowed)
    ledger = sink.get_paper_ledger()
    last_seq = sink.get_events()[-1].seq if sink.get_events() else 0
    for entry in ledger.values():
        if entry.state.status in (PaperOrderLedgerStatus.OPEN, PaperOrderLedgerStatus.PARTIALLY_FILLED) and (ttl_events > 0 and last_seq - entry.state.created_seq >= ttl_events):
            cancel = NormalizedLobEvent(
                ts_exchange=current_ts,
                ts_received=current_ts,
                side=entry.state.side,
                update_type=UpdateType.DELETE,
                price_ticks=entry.state.price_ticks,
                quantity_lots=0,
                order_id=entry.state.order_id,
                trader_id=UnknownTraderIdSentinel,
                aggressor_id=UnknownAggressorIdSentinel,
                symbol_id=adapter.symbol_id,
                update_source=UpdateSource.STRATEGY,
            )
            engine.update(cancel)

    best_bid = engine.get_best_price_ticks(Side.BUY)
    best_ask = engine.get_best_price_ticks(Side.SELL)
    if best_bid is None or best_ask is None:
        return next_order_id, "no_quotes"

    # Current account snapshot
    acct = compute_account_metrics(
        sink,
        tick_size=adapter.tick_size,
        lot_size=adapter.lot_size,
        mark_px=current_mid,
        fee_bps=fee_bps,
        starting_cash=risk.cfg.starting_cash,
    )
    position_units = float(acct["position"])
    avg_cost = float(acct["avg_cost"])

    def submit(
        side: Side,
        limit_price_ticks: int,
        *,
        requested_qty_units: float,
        aggressive: bool,
        reason: str,
    ):
        nonlocal next_order_id

        # Decide what price will be used for budget/risk estimation
        if aggressive:
            eff_price_units = (best_ask if side == Side.BUY else best_bid) * adapter.tick_size
        else:
            eff_price_units = limit_price_ticks * adapter.tick_size

        ok, clipped_units, msg = risk_can_submit(
            risk,
            sink,
            tick_size=adapter.tick_size,
            lot_size=adapter.lot_size,
            mark_px=current_mid,
            fee_bps=fee_bps,
            current_ts=current_ts,
            side=side,
            requested_qty_units=requested_qty_units,
            effective_price_units=eff_price_units,
        )
        if not ok:
            return f"{reason} -> {msg}"

        qty_lots = to_ticks(clipped_units, adapter.lot_size, strict=False)
        if qty_lots <= 0:
            return f"{reason} -> blocked: lot_rounding"

        update_type = UpdateType.AGGRESSIVE_TRADE if aggressive else UpdateType.ADD
        price_ticks = limit_price_ticks

        # If aggressive, cross immediately at top of book
        if aggressive:
            price_ticks = best_ask if side == Side.BUY else best_bid

        ev = NormalizedLobEvent(
            ts_exchange=current_ts,
            ts_received=current_ts,
            side=side,
            update_type=update_type,
            price_ticks=int(price_ticks),
            quantity_lots=int(qty_lots),
            order_id=next_order_id,
            trader_id=UnknownTraderIdSentinel,
            aggressor_id=UnknownAggressorIdSentinel,
            symbol_id=adapter.symbol_id,
            update_source=UpdateSource.STRATEGY,
        )
        engine.update(ev)

        risk.last_order_ts = current_ts
        next_order_id += 1
        return f"{reason} {side.name} {clipped_units:.6f}"

    action = "hold"

    # TAKE PROFIT (optional)
    if position_units > 0 and take_profit > 0 and (current_mid - avg_cost) >= take_profit:
        action = submit(
            Side.SELL,
            limit_price_ticks=int(best_bid + offset_ticks),
            requested_qty_units=abs(position_units),
            aggressive=aggressive_exits,
            reason="take_profit_long",
        )
        return next_order_id, action

    if position_units < 0 and take_profit > 0 and (avg_cost - current_mid) >= take_profit:
        action = submit(
            Side.BUY,
            limit_price_ticks=int(max(0, best_ask - offset_ticks)),
            requested_qty_units=abs(position_units),
            aggressive=aggressive_exits,
            reason="take_profit_short",
        )
        return next_order_id, action

    # EXIT LOGIC (inventory control) — hysteresis avoids flip-flop churn
    if position_units > 0 and delta <= -abs(exit_hysteresis):
        action = submit(
            Side.SELL,
            limit_price_ticks=int(best_bid + offset_ticks),
            requested_qty_units=abs(position_units),
            aggressive=aggressive_exits,
            reason="exit_long",
        )
        return next_order_id, action

    if position_units < 0 and delta >= abs(exit_hysteresis):
        action = submit(
            Side.BUY,
            limit_price_ticks=int(max(0, best_ask - offset_ticks)),
            requested_qty_units=abs(position_units),
            aggressive=aggressive_exits,
            reason="exit_short",
        )
        return next_order_id, action

    # ENTRY LOGIC (passive by default)
    if delta >= up_thresh:
        # Buy at best_bid - offset (passive). Offset>0 becomes more conservative.
        limit_px = int(max(0, best_bid - offset_ticks))
        action = submit(
            Side.BUY,
            limit_price_ticks=limit_px,
            requested_qty_units=qty,
            aggressive=False,
            reason="enter_long",
        )
        return next_order_id, action

    if delta <= -down_thresh:
        # Sell at best_ask + offset (passive).
        limit_px = int(best_ask + offset_ticks)
        action = submit(
            Side.SELL,
            limit_price_ticks=limit_px,
            requested_qty_units=qty,
            aggressive=False,
            reason="enter_short",
        )
        return next_order_id, action

    return next_order_id, action


# --------------------------
# UI / Tables
# --------------------------
def plot_mid(mid_history: List[Tuple[int, float]]):
    if not mid_history:
        st.info("No price history yet. Step the replay to populate the chart.")
        return
    x = [t for t, _ in mid_history[-2000:]]
    y = [p for _, p in mid_history[-2000:]]
    fig = go.Figure()
    fig.add_trace(go.Scatter(x=x, y=y, mode="lines", name="mid"))
    fig.update_layout(
        height=350,
        margin=dict(l=20, r=20, t=30, b=30),
        xaxis_title="ts_received (us)",
        yaxis_title="mid",
    )
    st.plotly_chart(fig, use_container_width=True)


def plot_mid_avg(mid_history: List[Tuple[int, float]], *, window: int):
    if not mid_history:
        st.info("No price history yet. Step the replay to populate the chart.")
        return
    if window <= 1 or len(mid_history) < window:
        st.info("Need more data to plot rolling mid.")
        return

    x = [t for t, _ in mid_history[-2000:]]
    y = [p for _, p in mid_history[-2000:]]

    avg = []
    for i in range(len(y)):
        if i + 1 < window:
            avg.append(None)
        else:
            avg.append(sum(y[i + 1 - window : i + 1]) / window)

    fig = go.Figure()
    fig.add_trace(go.Scatter(x=x, y=avg, mode="lines", name=f"rolling mid ({window})"))
    fig.update_layout(
        height=220,
        margin=dict(l=20, r=20, t=20, b=20),
        xaxis_title="ts_received (us)",
        yaxis_title="mid avg",
        title=f"Rolling Mid (window={window})",
    )
    st.plotly_chart(fig, use_container_width=True)


def render_summary(metrics: dict, risk: RiskState):
    cols = st.columns(7)
    cols[0].metric("Cash", f"{metrics['cash']:.2f}")
    cols[1].metric("Position", f"{metrics['position']:.8f}")
    cols[2].metric("Equity", f"{metrics['equity']:.2f}")
    cols[3].metric("Realized", f"{metrics['realized']:.2f}")
    cols[4].metric("Unrealized", f"{metrics['unrealized']:.2f}")
    cols[5].metric("Total PnL", f"{metrics['total_pnl']:.2f}")
    sharpe_txt = "-" if metrics.get("sharpe") is None else f"{metrics['sharpe']:.2f}"
    cols[6].metric("Sharpe (fills)", sharpe_txt)

    if not risk.enabled:
        st.error(f"Trading disabled: {risk.disabled_reason}")


def strategy_orders_table(sink: InMemoryLogSink, *, view: str):
    ledger = sink.get_paper_ledger()
    rows = []
    adapter = st.session_state.adapter
    aggressive_rows = []

    # Aggressive fills show as fills but won't exist in the paper ledger
    for r in sink.get_fills():
        if r.maker_source == UpdateSource.STRATEGY and r.maker_order_id not in ledger:
            aggressive_rows.append(
                {
                    "type": "aggressive",
                    "order_id": r.maker_order_id,
                    "side": r.maker_side.name,
                    "status": "FILLED",
                    "price": round(r.price_ticks * adapter.tick_size, 2),
                    "qty_filled": round(r.qty_lots * adapter.lot_size, 8),
                    "created_seq": r.seq,
                }
            )
        if r.taker_source == UpdateSource.STRATEGY and r.taker_order_id not in ledger:
            aggressive_rows.append(
                {
                    "type": "aggressive",
                    "order_id": r.taker_order_id,
                    "side": r.taker_side.name,
                    "status": "FILLED",
                    "price": round(r.price_ticks * adapter.tick_size, 2),
                    "qty_filled": round(r.qty_lots * adapter.lot_size, 8),
                    "created_seq": r.seq,
                }
            )

    for entry in ledger.values():
        state = entry.state
        is_active = state.status in (PaperOrderLedgerStatus.OPEN, PaperOrderLedgerStatus.PARTIALLY_FILLED)
        if view == "Active" and not is_active:
            continue
        if view == "Closed" and is_active:
            continue
        if view == "Aggressive":
            continue

        rows.append(
            {
                "type": "resting",
                "order_id": state.order_id,
                "side": state.side.name,
                "status": state.status.name,
                "price": round(state.price_ticks * adapter.tick_size, 2),
                "qty_init": round(state.initial_qty * adapter.lot_size, 8),
                "qty_rem": round(state.remaining_qty * adapter.lot_size, 8),
                "qty_filled": round(state.filled_qty * adapter.lot_size, 8),
                "created_seq": state.created_seq,
                "last_update_seq": state.last_update_seq,
            }
        )

    if view == "Aggressive":
        rows = aggressive_rows
    elif view == "All":
        rows += aggressive_rows

    rows.sort(key=lambda r: r["created_seq"])
    _render_table(rows, height_px=260)


def strategy_fills_table(sink: InMemoryLogSink):
    ledger = sink.get_paper_ledger()
    rows = []
    adapter = st.session_state.adapter
    for entry in ledger.values():
        rows.extend(
            {
                "seq": f.seq,
                "order_id": entry.state.order_id,
                "side": entry.state.side.name,
                "price": round(f.price_ticks * adapter.tick_size, 2),
                "qty": round(f.qty_lots * adapter.lot_size, 8),
                "role": f.role.name,
            }
            for f in entry.fills
        )
    rows.sort(key=lambda r: r["seq"])
    _render_table(rows, height_px=450)


def recent_events(sink: InMemoryLogSink):
    events = sink.get_events()[-100:]
    adapter = st.session_state.adapter
    rows = [
        {
            "seq": ev.seq,
            "ts": ev.ts_received,
            "side": ev.side.name,
            "type": ev.update_type.name,
            "src": ev.source.name,
            "price": round(ev.price_ticks * adapter.tick_size, 2),
            "qty": round(ev.qty_lots * adapter.lot_size, 8),
            "order": ev.order_id,
        }
        for ev in events[::-1]
    ]
    _render_table(rows, height_px=380)


def recent_fills(sink: InMemoryLogSink):
    fills = sink.get_fills()[-100:]
    adapter = st.session_state.adapter
    rows = [
        {
            "seq": r.seq,
            "ts": r.ts_received,
            "price": round(r.price_ticks * adapter.tick_size, 2),
            "qty": round(r.qty_lots * adapter.lot_size, 8),
            "maker_src": r.maker_source.name,
            "taker_src": r.taker_source.name,
        }
        for r in reversed(fills)
    ]
    _render_table(rows, height_px=380)


def recent_diagnostics(sink: InMemoryLogSink):
    diags = sink.get_diagnostics()[-100:]
    rows = [
        {
            "seq": r.seq,
            "ts": r.ts_received,
            "code": r.code,
            "severity": r.severity,
        }
        for r in reversed(diags)
    ]
    _render_table(rows, height_px=340)


# --------------------------
# Init / Step
# --------------------------
def init_engine(path: Path, tick_size: float, lot_size: float, batch_size: int, symbol: str, risk_cfg: RiskConfig):
    engine = PaperTradingSimulator()
    sink = InMemoryLogSink()
    engine.set_log_sink(sink)

    snap_path = find_snapshot_path(path)
    if snap_path is not None:
        sides, prices, quantities, order_ids, trader_ids, dupes = load_l3_snapshot(
            snap_path, tick_size=tick_size, lot_size=lot_size, batch_size=batch_size, symbol_id=symbol
        )
        if sides:
            engine.init_from_l3_snapshot(sides, prices, quantities, order_ids, trader_ids)
            msg = f"Loaded snapshot {snap_path.name} ({len(sides)} orders"
            if dupes:
                msg += f", skipped {dupes} dupes"
            msg += ")"
            st.success(msg)
        else:
            st.warning(f"Snapshot {snap_path.name} had no usable levels.")
    else:
        st.warning("Snapshot not found. Starting from empty book.")

    source = ParquetStream(path, batch_size=batch_size)
    adapter = Adapter(tick_size=tick_size, lot_size=lot_size, symbol_id=symbol)

    st.session_state.engine = engine
    st.session_state.sink = sink
    st.session_state.source = source
    st.session_state.adapter = adapter
    st.session_state.mid_history = []
    st.session_state.pnl_history = []
    st.session_state.current_ts = None
    st.session_state.last_decision_ts = 0
    st.session_state.next_order_id = 1_000_000_000
    st.session_state.last_action = "-"
    st.session_state.risk = RiskState(cfg=risk_cfg, last_order_ts=0, enabled=True, disabled_reason="")


def step_events(
    n: int,
    *,
    run_strategy: bool,
    strategy_params: dict,
    decision_mode: str,
    decision_value: int,
    fee_bps: float,
):
    engine: PaperTradingSimulator = st.session_state.engine
    source: ParquetStream = st.session_state.source
    adapter: Adapter = st.session_state.adapter
    sink: InMemoryLogSink = st.session_state.sink
    mid_history = st.session_state.mid_history
    pnl_history = st.session_state.pnl_history
    risk: RiskState = st.session_state.risk

    applied = 0
    last_ts = None
    last_decision_ts = st.session_state.last_decision_ts or 0

    for _ in range(n):
        try:
            raw = next(source)
        except StopIteration:
            break

        ev = adapter.normalize(raw)
        engine.update(ev)
        applied += 1
        last_ts = ev.ts_received

        _, mid_price = mid_from_engine(engine, adapter.tick_size)
        if mid_price is not None:
            mid_history.append((ev.ts_received, mid_price))

        if run_strategy and mid_price is not None:
            allow = False
            if decision_mode == "Every event":
                allow = True
            elif decision_mode == "Every N events":
                if decision_value > 0 and applied % decision_value == 0:
                    allow = True
            elif decision_mode == "Every N ms":
                if decision_value > 0 and (ev.ts_received - last_decision_ts) >= decision_value * 1000:
                    allow = True

            if allow:
                st.session_state.next_order_id, action = strategy_tick(
                    engine,
                    sink,
                    adapter,
                    mid_history,
                    risk,
                    window=strategy_params["window"],
                    up_thresh=strategy_params["up_thresh"],
                    down_thresh=strategy_params["down_thresh"],
                    exit_hysteresis=strategy_params["exit_hysteresis"],
                    qty=strategy_params["qty"],
                    offset_ticks=strategy_params["offset"],
                    ttl_events=strategy_params["ttl"],
                    aggressive_exits=strategy_params["aggressive_exits"],
                    take_profit=strategy_params["take_profit"],
                    next_order_id=st.session_state.next_order_id,
                    fee_bps=fee_bps,
                )
                st.session_state.last_action = action
                last_decision_ts = ev.ts_received

    if last_ts is not None:
        st.session_state.current_ts = last_ts
        st.session_state.last_decision_ts = last_decision_ts

        _, mid_price = mid_from_engine(engine, adapter.tick_size)
        metrics = compute_account_metrics(
            sink,
            tick_size=adapter.tick_size,
            lot_size=adapter.lot_size,
            mark_px=mid_price,
            fee_bps=fee_bps,
            starting_cash=risk.cfg.starting_cash,
        )
        risk_check_killswitch(risk, metrics["total_pnl"])
        pnl_history.append((last_ts, metrics["total_pnl"]))

    return applied, last_ts


def step_time(
    delta_ms: int,
    *,
    run_strategy: bool,
    strategy_params: dict,
    decision_mode: str,
    decision_value: int,
    fee_bps: float,
):
    engine: PaperTradingSimulator = st.session_state.engine
    source: ParquetStream = st.session_state.source
    adapter: Adapter = st.session_state.adapter
    sink: InMemoryLogSink = st.session_state.sink
    mid_history = st.session_state.mid_history
    pnl_history = st.session_state.pnl_history
    risk: RiskState = st.session_state.risk

    applied = 0
    base = st.session_state.current_ts or 0
    target = base + (delta_ms * 1000)
    last_ts = None
    last_decision_ts = st.session_state.last_decision_ts or 0

    while True:
        try:
            raw = next(source)
        except StopIteration:
            break

        ev = adapter.normalize(raw)
        engine.update(ev)
        applied += 1
        last_ts = ev.ts_received

        _, mid_price = mid_from_engine(engine, adapter.tick_size)
        if mid_price is not None:
            mid_history.append((ev.ts_received, mid_price))

        if run_strategy and mid_price is not None:
            allow = False
            if decision_mode == "Every event":
                allow = True
            elif decision_mode == "Every N events":
                if decision_value > 0 and applied % decision_value == 0:
                    allow = True
            elif decision_mode == "Every N ms":
                if decision_value > 0 and (ev.ts_received - last_decision_ts) >= decision_value * 1000:
                    allow = True

            if allow:
                st.session_state.next_order_id, action = strategy_tick(
                    engine,
                    sink,
                    adapter,
                    mid_history,
                    risk,
                    window=strategy_params["window"],
                    up_thresh=strategy_params["up_thresh"],
                    down_thresh=strategy_params["down_thresh"],
                    exit_hysteresis=strategy_params["exit_hysteresis"],
                    qty=strategy_params["qty"],
                    offset_ticks=strategy_params["offset"],
                    ttl_events=strategy_params["ttl"],
                    aggressive_exits=strategy_params["aggressive_exits"],
                    take_profit=strategy_params["take_profit"],
                    next_order_id=st.session_state.next_order_id,
                    fee_bps=fee_bps,
                )
                st.session_state.last_action = action
                last_decision_ts = ev.ts_received

        if ev.ts_received >= target:
            break

    if last_ts is not None:
        st.session_state.current_ts = last_ts
        st.session_state.last_decision_ts = last_decision_ts

        _, mid_price = mid_from_engine(engine, adapter.tick_size)
        metrics = compute_account_metrics(
            sink,
            tick_size=adapter.tick_size,
            lot_size=adapter.lot_size,
            mark_px=mid_price,
            fee_bps=fee_bps,
            starting_cash=risk.cfg.starting_cash,
        )
        risk_check_killswitch(risk, metrics["total_pnl"])
        pnl_history.append((last_ts, metrics["total_pnl"]))

    return applied, last_ts


# --------------------------
# Main App
# --------------------------
def main():
    st.set_page_config(page_title="Trend-following strategy with LOBSIM", layout="wide")

    st.markdown(
        """
        <style>
        @import url('https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;600&family=IBM+Plex+Mono:wght@400;600&display=swap');
        :root {
            --bg: #f7f4ef;
            --ink: #1b1a17;
            --panel: #fffaf3;
            --border: #e6ddd1;
        }
        .stApp {
            background: radial-gradient(circle at top, #fff 0%, #f7f4ef 50%, #efe7dd 100%);
            color: var(--ink);
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
        button[data-testid="baseButton-primary"]{
            background: var(--ink) !important;
            color: #ffffff !important;
            border: none !important;
            box-shadow: none !important;
        }
        button[data-testid="baseButton-primary"]:hover{
            background: #2d2b27 !important;
            color: #ffffff !important;
        }
        button[data-testid="baseButton-primary"] *{
            color: #ffffff !important;
        }
        </style>
        """,
        unsafe_allow_html=True,
    )

    st.title("Trend-following strategy with LOBSIM")

    with st.sidebar:
        st.header("Data & Market")
        default_path = "sample_data/coinbase_btcusdt_sample_big.parquet"
        data_path = st.text_input("Parquet path", value=default_path)
        tick_size = st.number_input("Tick size", value=0.01, min_value=1e-8, step=0.01, format="%.8f")
        lot_size = st.number_input("Lot size", value=1e-8, min_value=1e-12, step=1e-8, format="%.12f")
        batch_size = st.slider("Batch size", min_value=50, max_value=2000, value=200, step=50)
        symbol = st.text_input("Symbol id", value="BTC-USDT")

        st.header("Risk")
        starting_cash = st.number_input("Starting cash (USDT)", value=10_000.0, min_value=0.0, step=100.0)
        allow_short = st.checkbox("Allow shorting", value=False)
        max_position = st.number_input("Max abs position (units)", value=0.003, min_value=0.0, step=0.001, format="%.6f")
        max_open = st.number_input("Max open orders per side", value=1, min_value=0, step=1)
        min_interval = st.number_input("Min time between orders (ms)", value=250, min_value=0, step=50)
        max_loss = st.number_input("Kill-switch max loss (USDT, 0=off)", value=0.0, min_value=0.0, step=10.0)

        init_btn = st.button("Initialize / Reset", type="primary")

        st.header("Strategy")
        enable_strategy = st.checkbox("Enable strategy", value=True)
        window = st.slider("Mid-price sliding window (#events)", min_value=10, max_value=500, value=100, step=10)
        up_thresh = st.number_input("Up threshold (price units)", value=0.5, step=0.1, format="%.4f")
        down_thresh = st.number_input("Down threshold (price units)", value=0.5, step=0.1, format="%.4f")
        exit_hysteresis = st.number_input("Exit hysteresis (price units)", value=0.1, min_value=0.0, step=0.05, format="%.4f")
        qty = st.number_input("Order quantity (units)", value=0.001, min_value=1e-6, format="%.6f")
        offset = st.number_input("Limit offset (ticks)", value=0, step=1, format="%d")
        ttl = st.number_input("Cancel after N events", value=500, min_value=0, step=50, format="%d")
        aggressive_exits = st.checkbox("Aggressive exits", value=True)
        take_profit = st.number_input("Take profit (price units, 0=off)", value=0.0, min_value=0.0, step=0.1, format="%.4f")
        fee_bps = st.number_input("Trading fee (bps)", value=0.0, min_value=0.0, step=0.5, format="%.2f")

        decision_mode = st.radio(
            "Strategy decision frequency",
            ["Every event", "Every N events", "Every N ms"],
            horizontal=True,
        )
        if decision_mode == "Every N events":
            decision_value = st.number_input("Decision every N events", value=10, min_value=1, step=1, format="%d")
        elif decision_mode == "Every N ms":
            decision_value = st.number_input("Decision every N ms", value=1000, min_value=1, step=100, format="%d")
        else:
            decision_value = 1

        step_mode = st.radio("Step mode", ["Events", "Time (ms)"], horizontal=True)
        if step_mode == "Events":
            step_n = st.number_input("Step size (events)", value=100, min_value=1, step=50, format="%d")
            col_step = st.columns(2)
            with col_step[0]:
                step_once = st.button("Step 1", type="primary")
            with col_step[1]:
                step_n_btn = st.button(f"Step {step_n}", type="primary")
            step_ms = 0
            step_time_btn = False
        else:
            step_ms = st.number_input("Step size (ms)", value=1000, min_value=1, step=100, format="%d")
            step_once = False
            step_n_btn = False
            step_time_btn = st.button(f"Step {step_ms} ms", type="primary")

    if init_btn:
        risk_cfg = RiskConfig(
            starting_cash=float(starting_cash),
            allow_short=bool(allow_short),
            max_position=float(max_position),
            max_open_orders_per_side=int(max_open),
            min_order_interval_ms=int(min_interval),
            max_loss=float(max_loss),
        )
        init_engine(Path(data_path), tick_size, lot_size, batch_size, symbol, risk_cfg)

    if "engine" not in st.session_state:
        st.info("Initialize from the sidebar to start.")
        return

    strategy_params = {
        "window": window,
        "up_thresh": up_thresh,
        "down_thresh": down_thresh,
        "exit_hysteresis": exit_hysteresis,
        "qty": qty,
        "offset": int(offset),
        "ttl": int(ttl),
        "aggressive_exits": aggressive_exits,
        "take_profit": take_profit,
    }
    run_strategy = bool(enable_strategy)

    if step_once and step_mode == "Events":
        step_events(
            1,
            run_strategy=run_strategy,
            strategy_params=strategy_params,
            decision_mode=decision_mode,
            decision_value=int(decision_value),
            fee_bps=fee_bps,
        )
    if step_n_btn and step_mode == "Events":
        step_events(
            int(step_n),
            run_strategy=run_strategy,
            strategy_params=strategy_params,
            decision_mode=decision_mode,
            decision_value=int(decision_value),
            fee_bps=fee_bps,
        )
    if step_time_btn and step_mode == "Time (ms)":
        step_time(
            int(step_ms),
            run_strategy=run_strategy,
            strategy_params=strategy_params,
            decision_mode=decision_mode,
            decision_value=int(decision_value),
            fee_bps=fee_bps,
        )

    engine: PaperTradingSimulator = st.session_state.engine
    sink: InMemoryLogSink = st.session_state.sink
    adapter: Adapter = st.session_state.adapter
    risk: RiskState = st.session_state.risk

    _, mid_price = mid_from_engine(engine, adapter.tick_size)

    metrics = compute_account_metrics(
        sink,
        tick_size=adapter.tick_size,
        lot_size=adapter.lot_size,
        mark_px=mid_price,
        fee_bps=fee_bps,
        starting_cash=risk.cfg.starting_cash,
    )
    risk_check_killswitch(risk, metrics["total_pnl"])

    render_summary(metrics, risk)

    plots_col, book_col = st.columns([1.4, 1])
    with plots_col:
        st.subheader("Dashboard")

        # Existing plot
        plot_mid(st.session_state.mid_history)

        # NEW rolling mid plot
        plot_mid_avg(st.session_state.mid_history, window=window)

        # Keep your existing PnL plot exactly as it is (if you already have one)
        if st.session_state.pnl_history:
            pnl_x = [t for t, _ in st.session_state.pnl_history[-2000:]]
            pnl_y = [v for _, v in st.session_state.pnl_history[-2000:]]
            fig = go.Figure()
            fig.add_trace(go.Scatter(x=pnl_x, y=pnl_y, mode="lines+markers", name="PnL"))
            fig.update_layout(
                height=220,
                margin=dict(l=20, r=20, t=20, b=20),
                xaxis_title="ts_received (us)",
                yaxis_title="PnL",
                title="Strategy PnL (realized + unrealized)",
            )
            st.plotly_chart(fig, use_container_width=True)

    with book_col:
        st.subheader("Order Book (Top 10)")

        asks = engine.l2_top_n(Side.SELL, 10)
        bids = engine.l2_top_n(Side.BUY, 10)

        st.write("Asks")
        _render_table(
            _format_levels(
                asks,
                tick_size=st.session_state.adapter.tick_size,
                lot_size=st.session_state.adapter.lot_size,
            ),
            height_px=460,
        )

        st.text(" ")

        st.write("Bids")
        _render_table(
            _format_levels(
                bids,
                tick_size=st.session_state.adapter.tick_size,
                lot_size=st.session_state.adapter.lot_size,
            ),
            height_px=460,
        )
        
    st.markdown("<div class='section-title'>Strategy Fills (resting)</div>", unsafe_allow_html=True)
    strategy_fills_table(sink)

    st.markdown("<div class='section-title'>Strategy Orders</div>", unsafe_allow_html=True)
    view = st.radio(
        "Strategy Orders",
        ["Active", "Closed", "Aggressive", "All"],
        horizontal=True,
        label_visibility="collapsed",
    )
    strategy_orders_table(sink, view=view)

    st.markdown("<div class='section-title'>Recent Events</div>", unsafe_allow_html=True)
    recent_events(sink)
    st.markdown("<div class='section-title'>Recent Fills</div>", unsafe_allow_html=True)
    recent_fills(sink)
    st.markdown("<div class='section-title'>Recent Diagnostics</div>", unsafe_allow_html=True)
    recent_diagnostics(sink)


if __name__ == "__main__":
    main()
