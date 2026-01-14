from ._core import PaperTradingSimulatorCore

from typing import Sequence

from .sink import InMemoryLogSink
from .lob_event import NormalizedLobEvent
from .types import Side

class PaperTradingSimulatorCore:
    def __init__(
        self,
        sides: Sequence[Side] | None = ...,
        prices: Sequence[int] | None = ...,
        quantities: Sequence[int] | None = ...,
        sink: InMemoryLogSink | None = ...,
    ) -> None: ...
    def set_log_sink(self, sink: InMemoryLogSink) -> None: ...
    def update(self, event: NormalizedLobEvent) -> None: ...
    def init_from_l2_snapshot(
        self, sides: Sequence[Side], prices: Sequence[int], quantities: Sequence[int]
    ) -> None: ...
    def init_from_l3_snapshot(
        self,
        sides: Sequence[Side],
        prices: Sequence[int],
        quantities: Sequence[int],
        orderIds: Sequence[int],
        traderIds: Sequence[int],
    ) -> None: ...
    def depth_at(self, side: Side, price_ticks: int) -> int | None: ...
    def l2_top_n(self, side: Side, n: int) -> list[tuple[int, int]]: ...
    def get_best_price_ticks(self, side: Side) -> int | None: ...
