from __future__ import annotations

from enum import Enum

class Side(Enum):
    SELL = 0
    BUY = 1

class UpdateType(Enum):
    ADD = 0
    DELETE = 1
    SUBTRACT = 2
    MATCH = 3
    SET = 4

class UpdateSource(Enum):
    HISTORICAL = 0
    STRATEGY = 1

UnknownOrderIdSentinel: int
UnknownTraderIdSentinel: int
UnknownAggressorIdSentinel: int
NoAggressorNeededSentinel: int

__all__: list[str]
