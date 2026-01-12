from inspect import Parameter, Signature

from ._core import replay as _replay

ReplayConfig = _replay.ReplayConfig
RunSummary = _replay.RunSummary
ReplaySession = _replay.ReplaySession

ReplayConfig.__signature__ = Signature(
    parameters=[
        Parameter("require_monotonic_ts_received", Parameter.POSITIONAL_OR_KEYWORD, default=True),
        Parameter("fail_fast", Parameter.POSITIONAL_OR_KEYWORD, default=True),
    ]
)
ReplaySession.__signature__ = Signature(
    parameters=[
        Parameter("engine", Parameter.POSITIONAL_OR_KEYWORD),
        Parameter("config", Parameter.POSITIONAL_OR_KEYWORD, default=ReplayConfig()),
    ]
)

__all__ = ["ReplayConfig", "RunSummary", "ReplaySession"]
