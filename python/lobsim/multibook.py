"""
Thin convenience wrapper so `import lobsim.multibook` works.
Bindings are provided by the compiled `_core` module.
"""

from . import _core

Config = _core.multibook.Config
MultiBookSimulator = _core.multibook.MultiBookSimulator

__all__ = ["Config", "MultiBookSimulator"]
