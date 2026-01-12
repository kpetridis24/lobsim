from . import _core
import importlib
import sys
import types as _types

# Typed enums/sentinels under simex.types
types = importlib.import_module(__name__ + "._core.types")
sys.modules[__name__ + ".types"] = types

# Sink submodule grouping sink-related classes/records
sink = _types.ModuleType(__name__ + ".sink")
sink.InMemoryLogSink = _core.InMemoryLogSink
sink.FillRecord = _core.FillRecord
sink.EventApplyRecord = _core.EventApplyRecord
sys.modules[sink.__name__] = sink

# Engine submodule
engine = _types.ModuleType(__name__ + ".engine")
engine.PaperTradingSimulatorCore = _core.PaperTradingSimulatorCore
sys.modules[engine.__name__] = engine

# lob_event submodule
lob_event = _types.ModuleType(__name__ + ".lob_event")
lob_event.NormalizedLobEvent = _core.NormalizedLobEvent
sys.modules[lob_event.__name__] = lob_event

__all__ = [
    "types",
    "sink",
    "engine",
    "lob_event",
]
