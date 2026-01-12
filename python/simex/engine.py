from ._core import engine as _engine
from .sink import InMemoryLogSink


class PaperTradingSimulatorCore(_engine.PaperTradingSimulatorCore):
    def __init__(self) -> None:
        super().__init__()

    def set_log_sink(self, sink: InMemoryLogSink) -> None:  # type: ignore[override]
        super().set_log_sink(sink)


__all__ = ["PaperTradingSimulatorCore"]
