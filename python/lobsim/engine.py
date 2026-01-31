from inspect import Parameter, Signature
from typing import Sequence

from ._core import PaperTradingSimulator as _PaperTradingSimulator
from .lob_event import NormalizedLobEvent
from .sink import InMemoryLogSink
from .types import Side


class PaperTradingSimulator(_PaperTradingSimulator):
    def __init__(
        self,
        sides: Sequence[Side] | None = None,
        prices: Sequence[int] | None = None,
        quantities: Sequence[int] | None = None,
        sink: InMemoryLogSink | None = None,
        historical_capacity: int | None = None,
        paper_capacity: int | None = None,
    ) -> None:
        if sides is None and prices is None and quantities is None:
            if historical_capacity is not None or paper_capacity is not None:
                hist = (
                    historical_capacity if historical_capacity is not None else 200000
                )
                paper = paper_capacity if paper_capacity is not None else 100000
                super().__init__(hist, paper)
            else:
                super().__init__()
            if sink is not None:
                super().set_log_sink(sink)
            return

        if sides is None or prices is None or quantities is None:
            raise TypeError("sides, prices, and quantities must be provided together")

        if historical_capacity is not None or paper_capacity is not None:
            hist = historical_capacity if historical_capacity is not None else 200000
            paper = paper_capacity if paper_capacity is not None else 100000
            super().__init__(hist, paper)
            if sink is not None:
                super().set_log_sink(sink)
            super().init_from_l2_snapshot(sides, prices, quantities)
        else:
            super().__init__(sides, prices, quantities, sink)

    def set_log_sink(self, sink: InMemoryLogSink) -> None:  # type: ignore[override]
        super().set_log_sink(sink)

    def update(self, event: NormalizedLobEvent) -> None:  # type: ignore[override]
        super().update(event)

    def init_from_l2_snapshot(
        self, sides: Sequence[Side], prices: Sequence[int], quantities: Sequence[int]
    ) -> None:
        super().init_from_l2_snapshot(sides, prices, quantities)

    def init_from_l3_snapshot(
        self,
        sides: Sequence[Side],
        prices: Sequence[int],
        quantities: Sequence[int],
        order_ids: Sequence[int],
        trader_ids: Sequence[int],
    ) -> None:
        super().init_from_l3_snapshot(sides, prices, quantities, order_ids, trader_ids)

    def depth_at(self, side: Side, price_ticks: int) -> int | None:
        return super().depth_at(side, price_ticks)

    def l2_top_n(self, side: Side, n: int) -> list[tuple[int, int]]:
        return super().l2_top_n(side, n)

    def get_best_price_ticks(self, side: Side) -> int | None:
        return super().get_best_price_ticks(side)


PaperTradingSimulator.__signature__ = Signature(
    parameters=[
        Parameter("sides", Parameter.POSITIONAL_OR_KEYWORD, default=None),
        Parameter("prices", Parameter.POSITIONAL_OR_KEYWORD, default=None),
        Parameter("quantities", Parameter.POSITIONAL_OR_KEYWORD, default=None),
        Parameter("sink", Parameter.POSITIONAL_OR_KEYWORD, default=None),
        Parameter("historical_capacity", Parameter.POSITIONAL_OR_KEYWORD, default=None),
        Parameter("paper_capacity", Parameter.POSITIONAL_OR_KEYWORD, default=None),
    ]
)

__all__ = ["PaperTradingSimulator"]
