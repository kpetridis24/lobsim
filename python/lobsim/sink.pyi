from __future__ import annotations

from .types import (
    DiagnosticRecordCode,
    DiagnosticRecordSeverity,
    Side,
    UpdateSource,
    UpdateType,
)

class PaperOrderLedgerStatus: ...
class PaperOrderFillRole: ...

class PaperOrderFill:
    seq: int
    tsExchange: int
    tsReceived: int
    priceTicks: int
    qtyLots: int
    role: PaperOrderFillRole

class PaperOrderState:
    orderId: int
    side: Side
    priceTicks: int
    initialQty: int
    remainingQty: int
    filledQty: int
    status: PaperOrderLedgerStatus
    createdSeq: int
    lastUpdateSeq: int

class PaperOrderLedgerEntry:
    state: PaperOrderState
    fills: list[PaperOrderFill]

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
    bookKey: str

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
    bookKey: str

class DiagnosticRecord:
    seq: int
    tsExchange: int
    tsReceived: int
    code: DiagnosticRecordCode
    severity: DiagnosticRecordSeverity
    bookKey: str

class InMemoryLogSink:
    def __init__(self) -> None: ...
    def reset(self) -> None: ...
    def get_fills(self) -> list[FillRecord]: ...
    def get_events(self) -> list[EventApplyRecord]: ...
    def get_diagnostics(self) -> list[DiagnosticRecord]: ...
    def get_paper_ledger(self) -> dict[int, PaperOrderLedgerEntry]: ...
    def find_paper_order(self, order_id: int) -> PaperOrderLedgerEntry | None: ...
    def get_rejected_strategy_events(self) -> list[EventApplyRecord]: ...

class InMemoryMultiLogSink:
    def __init__(self) -> None: ...
    def reset(self) -> None: ...
    def fills(self) -> list[FillRecord]: ...
    def events(self) -> list[EventApplyRecord]: ...
    def diagnostics(self) -> list[DiagnosticRecord]: ...
    def fills_for(self, bookKey: str) -> list[FillRecord]: ...
    def events_for(self, bookKey: str) -> list[EventApplyRecord]: ...
    def diagnostics_for(self, bookKey: str) -> list[DiagnosticRecord]: ...
    def paper_ledger(self) -> dict[str, dict[int, PaperOrderLedgerEntry]]: ...
    def paper_ledger_for(self, bookKey: str) -> dict[int, PaperOrderLedgerEntry]: ...
    def find_paper_order(
        self, bookKey: str, order_id: int
    ) -> PaperOrderLedgerEntry | None: ...
    def rejected_strategy_events_for(self, bookKey: str) -> list[EventApplyRecord]: ...

__all__: list[str]
