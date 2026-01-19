from inspect import Signature

from ._core import DiagnosticRecord as DiagnosticRecord
from ._core import EventApplyRecord as EventApplyRecord
from ._core import FillRecord as FillRecord
from ._core import InMemoryLogSink as _InMemoryLogSink
from ._core import InMemoryMultiLogSink as _InMemoryMultiLogSink
from ._core import PaperOrderFill as PaperOrderFill
from ._core import PaperOrderFillRole as PaperOrderFillRole
from ._core import PaperOrderLedgerEntry as PaperOrderLedgerEntry
from ._core import PaperOrderLedgerStatus as PaperOrderLedgerStatus
from ._core import PaperOrderState as PaperOrderState


class InMemoryLogSink(_InMemoryLogSink):
    def __init__(self) -> None:
        super().__init__()

    def reset(self) -> None:
        super().reset()

    def get_fills(self) -> list[FillRecord]:
        return super().get_fills()

    def get_events(self) -> list[EventApplyRecord]:
        return super().get_events()

    def get_diagnostics(self) -> list[DiagnosticRecord]:
        return super().get_diagnostics()

    def get_paper_ledger(self) -> dict[int, PaperOrderLedgerEntry]:
        return super().get_paper_ledger()

    def find_paper_order(self, order_id: int) -> PaperOrderLedgerEntry | None:
        return super().find_paper_order(order_id)

    def get_rejected_strategy_events(self) -> list[EventApplyRecord]:
        return super().get_rejected_strategy_events()


InMemoryLogSink.__signature__ = Signature()


class InMemoryMultiLogSink(_InMemoryMultiLogSink):
    def __init__(self) -> None:
        super().__init__()

    def reset(self) -> None:
        super().reset()

    def fills(self) -> list[FillRecord]:
        return super().fills()

    def events(self) -> list[EventApplyRecord]:
        return super().events()

    def diagnostics(self) -> list[DiagnosticRecord]:
        return super().diagnostics()

    def fills_for(self, book_key: str) -> list[FillRecord]:
        return super().fills_for(book_key)

    def events_for(self, book_key: str) -> list[EventApplyRecord]:
        return super().events_for(book_key)

    def diagnostics_for(self, book_key: str) -> list[DiagnosticRecord]:
        return super().diagnostics_for(book_key)

    def paper_ledger(self) -> dict[str, dict[int, PaperOrderLedgerEntry]]:
        return super().paper_ledger()

    def paper_ledger_for(self, book_key: str) -> dict[int, PaperOrderLedgerEntry]:
        return super().paper_ledger_for(book_key)

    def find_paper_order(
        self, book_key: str, order_id: int
    ) -> PaperOrderLedgerEntry | None:
        return super().find_paper_order(book_key, order_id)

    def rejected_strategy_events_for(self, book_key: str) -> list[EventApplyRecord]:
        return super().rejected_strategy_events_for(book_key)


InMemoryMultiLogSink.__signature__ = Signature()

__all__ = [
    "InMemoryLogSink",
    "InMemoryMultiLogSink",
    "FillRecord",
    "EventApplyRecord",
    "DiagnosticRecord",
    "PaperOrderLedgerStatus",
    "PaperOrderFillRole",
    "PaperOrderFill",
    "PaperOrderState",
    "PaperOrderLedgerEntry",
]
