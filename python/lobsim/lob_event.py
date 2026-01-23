from ._core import NormalizedLobEvent as _NormalizedLobEvent
from .types import Side, UpdateSource, UpdateType


class NormalizedLobEvent(_NormalizedLobEvent):
    def __init__(
        self,
        ts_exchange: int = 0,
        ts_received: int = 0,
        side: Side = Side.BUY,
        update_type: UpdateType = UpdateType.ADD,
        price_ticks: int = 0,
        quantity_lots: int = 0,
        order_id: int = -1,
        trader_id: int = -1,
        aggressor_id: int = -2,
        update_source: UpdateSource = UpdateSource.HISTORICAL,
        symbol_id: str = "",
    ) -> None:
        super().__init__(
            ts_exchange,
            ts_received,
            side,
            update_type,
            price_ticks,
            quantity_lots,
            order_id,
            trader_id,
            aggressor_id,
            update_source,
            symbol_id,
        )


__all__ = ["NormalizedLobEvent"]
