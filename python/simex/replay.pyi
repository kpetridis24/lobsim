from __future__ import annotations

from typing import Sequence

from .engine import PaperTradingSimulatorCore
from .lob_event import NormalizedLobEvent


class ReplayConfig:
    require_monotonic_ts_received: bool
    fail_fast: bool

    def __init__(
        self,
        require_monotonic_ts_received: bool = True,
        fail_fast: bool = True,
    ) -> None: ...


class RunSummary:
    num_raw_events: int
    num_normalized_events: int
    num_engine_updates: int
    num_adapter_failures: int
    has_ts_range: bool
    first_ts_received: int
    last_ts_received: int


class ReplaySession:
    def __init__(self, engine: PaperTradingSimulatorCore) -> None: ...

    def run(self, events: Sequence[NormalizedLobEvent], config: ReplayConfig = ...) -> RunSummary: ...

