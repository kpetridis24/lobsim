from ._core import NormalizedLobEvent as _NormalizedLobEvent
from .types import Side, UpdateSource, UpdateType


class NormalizedLobEvent(_NormalizedLobEvent):
    def __init__(
        self,
        tsExchange: int = 0,
        tsReceived: int = 0,
        side: Side = Side.BUY,
        updateType: UpdateType = UpdateType.ADD,
        priceTicks: int = 0,
        quantityLots: int = 0,
        orderId: int = -1,
        traderId: int = -1,
        aggressorId: int = -2,
        updateSource: UpdateSource = UpdateSource.HISTORICAL,
        symbolId: str = "",
    ) -> None:
        super().__init__(
            tsExchange,
            tsReceived,
            side,
            updateType,
            priceTicks,
            quantityLots,
            orderId,
            traderId,
            aggressorId,
            updateSource,
            symbolId,
        )


__all__ = ["NormalizedLobEvent"]
