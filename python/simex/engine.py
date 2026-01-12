from inspect import Parameter, Signature
from typing import Sequence

from ._core import PaperTradingSimulatorCore as _PaperTradingSimulatorCore
from .sink import InMemoryLogSink
from .types import Side


class PaperTradingSimulatorCore(_PaperTradingSimulatorCore):
    def __init__(
        self,
        sides: Sequence[Side] | None = None,
        prices: Sequence[int] | None = None,
        quantities: Sequence[int] | None = None,
        sink: InMemoryLogSink | None = None,
    ) -> None:
        if sides is None and prices is None and quantities is None:
            super().__init__()
            if sink is not None:
                super().set_log_sink(sink)
            return

        if sides is None or prices is None or quantities is None:
            raise TypeError("sides, prices, and quantities must be provided together")

        super().__init__(sides, prices, quantities, sink)

    def set_log_sink(self, sink: InMemoryLogSink) -> None:  # type: ignore[override]
        super().set_log_sink(sink)


PaperTradingSimulatorCore.__signature__ = Signature(
    parameters=[
        Parameter("sides", Parameter.POSITIONAL_OR_KEYWORD, default=None),
        Parameter("prices", Parameter.POSITIONAL_OR_KEYWORD, default=None),
        Parameter("quantities", Parameter.POSITIONAL_OR_KEYWORD, default=None),
        Parameter("sink", Parameter.POSITIONAL_OR_KEYWORD, default=None),
    ]
)

__all__ = ["PaperTradingSimulatorCore"]
