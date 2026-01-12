"""
Enums and sentinel constants
"""
from __future__ import annotations
import typing
__all__: list[str] = ['NoAggressorNeededSentinel', 'Side', 'UnknownAggressorIdSentinel', 'UnknownOrderIdSentinel', 'UnknownTraderIdSentinel', 'UpdateSource', 'UpdateType']
class Side:
    """
    Members:
    
      SELL
    
      BUY
    """
    BUY: typing.ClassVar[Side]  # value = <Side.BUY: 1>
    SELL: typing.ClassVar[Side]  # value = <Side.SELL: 0>
    __members__: typing.ClassVar[dict[str, Side]]  # value = {'SELL': <Side.SELL: 0>, 'BUY': <Side.BUY: 1>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: int) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: int) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class UpdateSource:
    """
    Members:
    
      HISTORICAL
    
      STRATEGY
    """
    HISTORICAL: typing.ClassVar[UpdateSource]  # value = <UpdateSource.HISTORICAL: 0>
    STRATEGY: typing.ClassVar[UpdateSource]  # value = <UpdateSource.STRATEGY: 1>
    __members__: typing.ClassVar[dict[str, UpdateSource]]  # value = {'HISTORICAL': <UpdateSource.HISTORICAL: 0>, 'STRATEGY': <UpdateSource.STRATEGY: 1>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: int) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: int) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
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
    __members__: typing.ClassVar[dict[str, UpdateType]]  # value = {'ADD': <UpdateType.ADD: 0>, 'DELETE': <UpdateType.DELETE: 1>, 'SUBTRACT': <UpdateType.SUBTRACT: 2>, 'MATCH': <UpdateType.MATCH: 3>, 'SET': <UpdateType.SET: 4>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: int) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: int) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
NoAggressorNeededSentinel: int = -2
UnknownAggressorIdSentinel: int = -1
UnknownOrderIdSentinel: int = -1
UnknownTraderIdSentinel: int = -1
