from __future__ import annotations

from .types import Side, UpdateSource, UpdateType

class FillRecord:
    seq: int
    tsExchange: int
    tsReceived: int
    priceTicks: int
    qtyLots: int
    makerSide: Side
    makerOrderId: int
    makerTraderId: int
    makerSource: UpdateSource
    takerSide: Side
    takerOrderId: int
    takerTraderId: int
    takerSource: UpdateSource

class EventApplyRecord:
    seq: int
    tsExchange: int
    tsReceived: int
    side: Side
    updateType: UpdateType
    source: UpdateSource
    priceTicks: int
    qtyLots: int
    orderId: int
    traderId: int
    aggressorId: int

class InMemoryLogSink:
    def __init__(self) -> None: ...
    def reset(self) -> None: ...
    def get_fills(self) -> list[FillRecord]: ...

class InMemoryMultiLogSink:
    def __init__(self) -> None: ...
    def reset(self) -> None: ...
    def fills(self) -> list[FillRecord]: ...
    def events(self) -> list[EventApplyRecord]: ...
    def diagnostics(self) -> list: ...
    def fills_for(self, bookKey: str) -> list[FillRecord]: ...
    def events_for(self, bookKey: str) -> list[EventApplyRecord]: ...
    def diagnostics_for(self, bookKey: str) -> list: ...

__all__: list[str]
