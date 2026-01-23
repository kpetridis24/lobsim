"""
lobsim: L3 replay + paper trading engine
"""

from __future__ import annotations
import typing

__all__: list[str] = [
    "ADD",
    "BUY",
    "DELETE",
    "FillRecord",
    "HISTORICAL",
    "InMemoryLogSink",
    "MATCH",
    "NoAggressorNeededSentinel",
    "NormalizedLobEvent",
    "PaperTradingSimulator",
    "SELL",
    "SET",
    "STRATEGY",
    "SUBTRACT",
    "Side",
    "UnknownAggressorIdSentinel",
    "UnknownOrderIdSentinel",
    "UnknownTraderIdSentinel",
    "UpdateSource",
    "UpdateType",
]

class FillRecord:
    @property
    def maker_order_id(self) -> int: ...
    @property
    def maker_side(self) -> Side: ...
    @property
    def maker_source(self) -> UpdateSource: ...
    @property
    def maker_trader_id(self) -> int: ...
    @property
    def price_ticks(self) -> int: ...
    @property
    def qty_lots(self) -> int: ...
    @property
    def seq(self) -> int: ...
    @property
    def taker_order_id(self) -> int: ...
    @property
    def taker_side(self) -> Side: ...
    @property
    def taker_source(self) -> UpdateSource: ...
    @property
    def taker_trader_id(self) -> int: ...
    @property
    def ts_exchange(self) -> int: ...
    @property
    def ts_received(self) -> int: ...

class InMemoryLogSink:
    def __init__(self) -> None: ...
    def fills(self) -> list[FillRecord]: ...
    def reset(self) -> None: ...

class NormalizedLobEvent:
    aggressor_id: int
    order_id: int
    price_ticks: int
    quantity_lots: int
    side: Side
    symbol_id: str
    trader_id: int
    ts_exchange: int
    ts_received: int
    update_source: UpdateSource
    update_type: UpdateType
    def __init__(self) -> None: ...

class PaperTradingSimulator:
    def __init__(self) -> None: ...
    def depth_at(self, arg0: Side, arg1: int) -> int | None: ...
    def init_from_l2_snapshot(
        self, sides: list[Side], prices: list[int], quantities: list[int]
    ) -> None: ...
    def init_from_l3_snapshot(
        self,
        sides: list[Side],
        prices: list[int],
        quantities: list[int],
        order_ids: list[int],
        trader_ids: list[int],
    ) -> None: ...
    def l2_top_n(self, arg0: Side, arg1: int) -> list[tuple[int, int]]: ...
    def set_log_sink(self, arg0: InMemoryLogSink) -> None: ...
    def update(self, event: NormalizedLobEvent) -> None: ...

class Side:
    """
    Members:

      SELL

      BUY
    """

    BUY: typing.ClassVar[Side]  # value = <Side.BUY: 1>
    SELL: typing.ClassVar[Side]  # value = <Side.SELL: 0>
    __members__: typing.ClassVar[
        dict[str, Side]
    ]  # value = {'SELL': <Side.SELL: 0>, 'BUY': <Side.BUY: 1>}
    def __eq__(self, other: typing.Any) -> bool: ...
    def __getstate__(self) -> int: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __init__(self, value: int) -> None: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: typing.Any) -> bool: ...
    def __repr__(self) -> str: ...
    def __setstate__(self, state: int) -> None: ...
    def __str__(self) -> str: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class UpdateSource:
    """
    Members:

      HISTORICAL

      STRATEGY
    """

    HISTORICAL: typing.ClassVar[UpdateSource]  # value = <UpdateSource.HISTORICAL: 0>
    STRATEGY: typing.ClassVar[UpdateSource]  # value = <UpdateSource.STRATEGY: 1>
    __members__: typing.ClassVar[
        dict[str, UpdateSource]
    ]  # value = {'HISTORICAL': <UpdateSource.HISTORICAL: 0>, 'STRATEGY': <UpdateSource.STRATEGY: 1>}
    def __eq__(self, other: typing.Any) -> bool: ...
    def __getstate__(self) -> int: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __init__(self, value: int) -> None: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: typing.Any) -> bool: ...
    def __repr__(self) -> str: ...
    def __setstate__(self, state: int) -> None: ...
    def __str__(self) -> str: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class UpdateType:
    """
    Members:

      ADD

      DELETE

      SUBTRACT

      MATCH

      SET
    """

    ADD: typing.ClassVar[UpdateType]  # value = <UpdateType.ADD: 0>
    DELETE: typing.ClassVar[UpdateType]  # value = <UpdateType.DELETE: 1>
    MATCH: typing.ClassVar[UpdateType]  # value = <UpdateType.MATCH: 3>
    SET: typing.ClassVar[UpdateType]  # value = <UpdateType.SET: 4>
    SUBTRACT: typing.ClassVar[UpdateType]  # value = <UpdateType.SUBTRACT: 2>
    __members__: typing.ClassVar[
        dict[str, UpdateType]
    ]  # value = {'ADD': <UpdateType.ADD: 0>, 'DELETE': <UpdateType.DELETE: 1>, 'SUBTRACT': <UpdateType.SUBTRACT: 2>, 'MATCH': <UpdateType.MATCH: 3>, 'SET': <UpdateType.SET: 4>}
    def __eq__(self, other: typing.Any) -> bool: ...
    def __getstate__(self) -> int: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __init__(self, value: int) -> None: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: typing.Any) -> bool: ...
    def __repr__(self) -> str: ...
    def __setstate__(self, state: int) -> None: ...
    def __str__(self) -> str: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

ADD: UpdateType  # value = <UpdateType.ADD: 0>
BUY: Side  # value = <Side.BUY: 1>
DELETE: UpdateType  # value = <UpdateType.DELETE: 1>
HISTORICAL: UpdateSource  # value = <UpdateSource.HISTORICAL: 0>
MATCH: UpdateType  # value = <UpdateType.MATCH: 3>
NoAggressorNeededSentinel: int = -2
SELL: Side  # value = <Side.SELL: 0>
SET: UpdateType  # value = <UpdateType.SET: 4>
STRATEGY: UpdateSource  # value = <UpdateSource.STRATEGY: 1>
SUBTRACT: UpdateType  # value = <UpdateType.SUBTRACT: 2>
UnknownAggressorIdSentinel: int = -1
UnknownOrderIdSentinel: int = -1
UnknownTraderIdSentinel: int = -1
