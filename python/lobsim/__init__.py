from __future__ import annotations

import importlib

from . import _core  # noqa: F401
from ._core import BookId  # re-export
from . import engine, lob_event, replay, sink, types

# Expose multibook submodule from the pybind package for convenience.
multibook = _core.multibook

__all__ = [
    "types",
    "replay",
    "sink",
    "engine",
    "lob_event",
    "multibook",
    "BookId",
    "demo_utils",
]


def __getattr__(name: str):
    if name == "demo_utils":
        mod = importlib.import_module(".demo_utils", __name__)
        globals()["demo_utils"] = mod
        return mod
    raise AttributeError(f"module '{__name__}' has no attribute '{name}'")
