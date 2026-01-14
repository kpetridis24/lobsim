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
    def aggressorId(self) -> int: ...
    @property
    def orderId(self) -> int: ...
    @property
    def priceTicks(self) -> int: ...
    @property
    def qtyLots(self) -> int: ...
    @property
    def seq(self) -> int: ...
    @property
    def side(self) -> types.Side: ...
    @property
    def source(self) -> types.UpdateSource: ...
    @property
    def traderId(self) -> int: ...
    @property
    def tsExchange(self) -> int: ...
    @property
    def tsReceived(self) -> int: ...
    @property
    def updateType(self) -> types.UpdateType: ...

class FillRecord:
    @property
    def makerOrderId(self) -> int: ...
    @property
    def makerSide(self) -> types.Side: ...
    @property
    def makerSource(self) -> types.UpdateSource: ...
    @property
    def makerTraderId(self) -> int: ...
    @property
    def priceTicks(self) -> int: ...
    @property
    def qtyLots(self) -> int: ...
    @property
    def seq(self) -> int: ...
    @property
    def takerOrderId(self) -> int: ...
    @property
    def takerSide(self) -> types.Side: ...
    @property
    def takerSource(self) -> types.UpdateSource: ...
    @property
    def takerTraderId(self) -> int: ...
    @property
    def tsExchange(self) -> int: ...
    @property
    def tsReceived(self) -> int: ...

class InMemoryLogSink:
    def __init__(self) -> None: ...
    def fills(self) -> list[FillRecord]: ...
    def reset(self) -> None: ...

class NormalizedLobEvent:
    aggressorId: int
    orderId: int
    priceTicks: int
    quantityLots: int
    side: types.Side
    symbolId: str
    traderId: int
    tsExchange: int
    tsReceived: int
    updateSource: types.UpdateSource
    updateType: types.UpdateType
    @typing.overload
    def __init__(self) -> None: ...
    @typing.overload
    def __init__(
        self,
        tsExchange: int = 0,
        tsReceived: int = 0,
        side: types.Side = ...,
        updateType: types.UpdateType = ...,
        priceTicks: int = 0,
        quantityLots: int = 0,
        orderId: int = -1,
        traderId: int = -1,
        aggressorId: int = -2,
        updateSource: types.UpdateSource = ...,
        symbolId: str = "",
    ) -> None: ...

class PaperTradingSimulator:
    def __init__(self) -> None: ...
    def depth_at(self, arg0: types.Side, arg1: int) -> int | None: ...
    def init_from_l2_snapshot(
        self, sides: list[types.Side], prices: list[int], quantities: list[int]
    ) -> None: ...
    def init_from_l3_snapshot(
        self,
        sides: list[types.Side],
        prices: list[int],
        quantities: list[int],
        orderIds: list[int],
        traderIds: list[int],
    ) -> None: ...
    def l2_top_n(self, arg0: types.Side, arg1: int) -> list[tuple[int, int]]: ...
    def set_log_sink(self, arg0: InMemoryLogSink) -> None: ...
    def update(self, event: NormalizedLobEvent) -> None: ...
