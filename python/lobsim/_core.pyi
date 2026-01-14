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
    def makerOrderId(self) -> int: ...
    @property
    def makerSide(self) -> Side: ...
    @property
    def makerSource(self) -> UpdateSource: ...
    @property
    def makerTraderId(self) -> int: ...
    @property
    def priceTicks(self) -> int: ...
    @property
    def qtyLots(self) -> int: ...
    @property
    def seq(self) -> int: ...
    @property
    def takerOrderId(self) -> int: ...
    @property
    def takerSide(self) -> Side: ...
    @property
    def takerSource(self) -> UpdateSource: ...
    @property
    def takerTraderId(self) -> int: ...
    @property
    def tsExchange(self) -> int: ...
    @property
    def tsReceived(self) -> int: ...

class InMemoryLogSink:
    def __init__(self) -> None: ...
    def fills(self) -> list[FillRecord]: ...
    def reset(self) -> None: ...

class NormalizedLobEvent:
    aggressorId: int
    orderId: int
    priceTicks: int
    quantityLots: int
    side: Side
    symbolId: str
    traderId: int
    tsExchange: int
    tsReceived: int
    updateSource: UpdateSource
    updateType: UpdateType
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
        orderIds: list[int],
        traderIds: list[int],
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
