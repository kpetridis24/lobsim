class NormalizedLobEvent:
    tsExchange: int
    tsReceived: int
    side: object
    updateType: object
    priceTicks: int
    quantityLots: int
    orderId: int
    traderId: int
    aggressorId: int
    updateSource: object
    symbolId: str
    def __init__(
        self,
        tsExchange: int = 0,
        tsReceived: int = 0,
        side=...,  # lobsim.types.Side
        updateType=...,  # lobsim.types.UpdateType
        priceTicks: int = 0,
        quantityLots: int = 0,
        orderId: int = ...,
        traderId: int = ...,
        aggressorId: int = ...,
        updateSource=...,  # lobsim.types.UpdateSource
        symbolId: str = "",
    ) -> None: ...
