from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


def _find_examples_dir() -> Path | None:
    env_path = os.getenv("LOBSIM_DEMO_PATH")
    if env_path:
        candidate = Path(env_path).expanduser().resolve()
        if candidate.is_dir():
            return candidate

    repo_candidate = Path(__file__).resolve().parents[2] / "examples"
    if repo_candidate.is_dir():
        return repo_candidate

    cwd_candidate = Path.cwd() / "examples"
    if cwd_candidate.is_dir():
        return cwd_candidate

    return None


def _resolve_demo_script(name: str) -> Path:
    examples_dir = _find_examples_dir()
    if examples_dir is None:
        raise FileNotFoundError(
            "Examples folder not found. Run from the repo root or set LOBSIM_DEMO_PATH to the examples directory."
        )

    mapping = {
        "replay": "lobsim_replay_explorer.py",
        "trend": "lobsim_trend_follow.py",
        "arb": "lobsim_arbitrage_demo.py",
    }
    if name not in mapping:
        raise ValueError(
            f"Unknown demo '{name}'. Available: {', '.join(sorted(mapping))}."
        )

    script_path = examples_dir / mapping[name]
    if not script_path.exists():
        raise FileNotFoundError(f"Demo script not found: {script_path}")

    return script_path


def _run_streamlit(script_path: Path) -> int:
    cmd = [sys.executable, "-m", "streamlit", "run", str(script_path)]
    return subprocess.call(cmd)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="lobsim", description="LOBSIM CLI")
    sub = parser.add_subparsers(dest="command", required=True)

    demo = sub.add_parser("demo", help="Run a Streamlit demo")
    demo.add_argument("name", choices=["replay", "trend", "arb"], help="Demo name")
    return parser


def main() -> int:
    parser = _build_parser()
    args = parser.parse_args()

    if args.command == "demo":
        script = _resolve_demo_script(args.name)
        return _run_streamlit(script)

    parser.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
