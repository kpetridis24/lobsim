class NormalizedLobEvent:
    ts_exchange: int
    ts_received: int
    side: object
    update_type: object
    price_ticks: int
    quantity_lots: int
    order_id: int
    trader_id: int
    aggressor_id: int
    update_source: object
    symbol_id: str
    def __init__(
        self,
        ts_exchange: int = 0,
        ts_received: int = 0,
        side=...,  # lobsim.types.Side
        update_type=...,  # lobsim.types.UpdateType
        price_ticks: int = 0,
        quantity_lots: int = 0,
        order_id: int = ...,
        trader_id: int = ...,
        aggressor_id: int = ...,
        update_source=...,  # lobsim.types.UpdateSource
        symbol_id: str = "",
    ) -> None: ...
