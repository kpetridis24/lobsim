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
    ts_exchange: int
    ts_received: int
    price_ticks: int
    qty_lots: int
    role: PaperOrderFillRole

class PaperOrderState:
    order_id: int
    side: Side
    price_ticks: int
    initial_qty: int
    remaining_qty: int
    filled_qty: int
    status: PaperOrderLedgerStatus
    created_seq: int
    last_update_seq: int

class PaperOrderLedgerEntry:
    state: PaperOrderState
    fills: list[PaperOrderFill]

class FillRecord:
    seq: int
    ts_exchange: int
    ts_received: int
    price_ticks: int
    qty_lots: int
    maker_side: Side
    maker_order_id: int
    maker_trader_id: int
    maker_source: UpdateSource
    taker_side: Side
    taker_order_id: int
    taker_trader_id: int
    taker_source: UpdateSource
    book_key: str

class EventApplyRecord:
    seq: int
    ts_exchange: int
    ts_received: int
    side: Side
    update_type: UpdateType
    source: UpdateSource
    price_ticks: int
    qty_lots: int
    order_id: int
    trader_id: int
    aggressor_id: int
    book_key: str

class DiagnosticRecord:
    seq: int
    ts_exchange: int
    ts_received: int
    code: DiagnosticRecordCode
    severity: DiagnosticRecordSeverity
    book_key: str

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
    def fills_for(self, book_key: str) -> list[FillRecord]: ...
    def events_for(self, book_key: str) -> list[EventApplyRecord]: ...
    def diagnostics_for(self, book_key: str) -> list[DiagnosticRecord]: ...
    def paper_ledger(self) -> dict[str, dict[int, PaperOrderLedgerEntry]]: ...
    def paper_ledger_for(self, book_key: str) -> dict[int, PaperOrderLedgerEntry]: ...
    def find_paper_order(
        self, book_key: str, order_id: int
    ) -> PaperOrderLedgerEntry | None: ...
    def rejected_strategy_events_for(self, book_key: str) -> list[EventApplyRecord]: ...

__all__: list[str]
