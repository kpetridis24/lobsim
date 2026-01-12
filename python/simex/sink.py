from inspect import Signature

from ._core import EventApplyRecord as EventApplyRecord
from ._core import FillRecord as FillRecord
from ._core import InMemoryLogSink as _InMemoryLogSink


class InMemoryLogSink(_InMemoryLogSink):
    def __init__(self) -> None:
        super().__init__()

    def reset(self) -> None:
        super().reset()

    def get_fills(self) -> list[FillRecord]:
        return super().get_fills()


InMemoryLogSink.__signature__ = Signature()

__all__ = ["InMemoryLogSink", "FillRecord", "EventApplyRecord"]
