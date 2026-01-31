"""
lobsim: L3 replay + paper trading engine
"""

from __future__ import annotations
import typing
from . import types

__all__: list[str] = [
    "EventApplyRecord",
    "FillRecord",
    "InMemoryLogSink",
    "NormalizedLobEvent",
    "PaperTradingSimulator",
    "types",
]

class EventApplyRecord:
    @property
    def aggressor_id(self) -> int: ...
    @property
    def order_id(self) -> int: ...
    @property
    def price_ticks(self) -> int: ...
    @property
    def qty_lots(self) -> int: ...
    @property
    def seq(self) -> int: ...
    @property
    def side(self) -> types.Side: ...
    @property
    def source(self) -> types.UpdateSource: ...
    @property
    def trader_id(self) -> int: ...
    @property
    def ts_exchange(self) -> int: ...
    @property
    def ts_received(self) -> int: ...
    @property
    def update_type(self) -> types.UpdateType: ...

class FillRecord:
    @property
    def maker_order_id(self) -> int: ...
    @property
    def maker_side(self) -> types.Side: ...
    @property
    def maker_source(self) -> types.UpdateSource: ...
    @property
    def maker_trader_id(self) -> int: ...
    @property
    def price_ticks(self) -> int: ...
    @property
    def qty_lots(self) -> int: ...
    @property
    def seq(self) -> int: ...
    @property
    def taker_order_id(self) -> int: ...
    @property
    def taker_side(self) -> types.Side: ...
    @property
    def taker_source(self) -> types.UpdateSource: ...
    @property
    def taker_trader_id(self) -> int: ...
    @property
    def ts_exchange(self) -> int: ...
    @property
    def ts_received(self) -> int: ...

class InMemoryLogSink:
    def __init__(self) -> None: ...
    def fills(self) -> list[FillRecord]: ...
    def reset(self) -> None: ...

class NormalizedLobEvent:
    aggressor_id: int
    order_id: int
    price_ticks: int
    quantity_lots: int
    side: types.Side
    symbol_id: str
    trader_id: int
    ts_exchange: int
    ts_received: int
    update_source: types.UpdateSource
    update_type: types.UpdateType
    @typing.overload
    def __init__(self) -> None: ...
    @typing.overload
    def __init__(
        self,
        ts_exchange: int = 0,
        ts_received: int = 0,
        side: types.Side = ...,
        update_type: types.UpdateType = ...,
        price_ticks: int = 0,
        quantity_lots: int = 0,
        order_id: int = -1,
        trader_id: int = -1,
        aggressor_id: int = -2,
        update_source: types.UpdateSource = ...,
        symbol_id: str = "",
    ) -> None: ...

class PaperTradingSimulator:
    @typing.overload
    def __init__(self) -> None: ...
    @typing.overload
    def __init__(self, historical_capacity: int, paper_capacity: int) -> None: ...
    @typing.overload
    def __init__(
        self,
        sides: list[types.Side],
        prices: list[int],
        quantities: list[int],
        sink: InMemoryLogSink | None = ...,
    ) -> None: ...
    def depth_at(self, arg0: types.Side, arg1: int) -> int | None: ...
    def init_from_l2_snapshot(
        self, sides: list[types.Side], prices: list[int], quantities: list[int]
    ) -> None: ...
    def init_from_l3_snapshot(
        self,
        sides: list[types.Side],
        prices: list[int],
        quantities: list[int],
        order_ids: list[int],
        trader_ids: list[int],
    ) -> None: ...
    def l2_top_n(self, arg0: types.Side, arg1: int) -> list[tuple[int, int]]: ...
    def set_log_sink(self, arg0: InMemoryLogSink) -> None: ...
    def update(self, event: NormalizedLobEvent) -> None: ...
