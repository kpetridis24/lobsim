from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _find_examples_dir() -> Path | None:
    if env_path := os.getenv("LOBSIM_DEMO_PATH"):
        candidate = Path(env_path).expanduser().resolve()
        if candidate.is_dir():
            return candidate

    repo_candidate = _repo_root() / "examples"
    if repo_candidate.is_dir():
        return repo_candidate

    cwd_candidate = Path.cwd() / "examples"
    return cwd_candidate if cwd_candidate.is_dir() else None


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


def _default_data_path() -> Path:
    return _repo_root() / "sample_data" / "coinbase_btcusdt_sample.parquet"


def _run_cpp_example(data_path: Path) -> int:
    root = _repo_root()
    build_dir = root / "build"
    subprocess.check_call(
        [
            "cmake",
            "-S",
            str(root),
            "-B",
            str(build_dir),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        ]
    )
    subprocess.check_call(
        ["cmake", "--build", str(build_dir), "--target", "lobsim_cpp_example"]
    )
    return subprocess.call([str(build_dir / "lobsim_cpp_example"), str(data_path)])


def _run_py_example() -> int:
    root = _repo_root()
    script = root / "examples" / "lobsim_multibook_py.py"
    if not script.exists():
        raise FileNotFoundError(f"Python example script not found: {script}")
    env = os.environ.copy()
    env["PYTHONPATH"] = (
        f"{root / 'python'}{os.pathsep}{env.get('PYTHONPATH', '')}".rstrip(os.pathsep)
    )
    return subprocess.call([sys.executable, str(script)], env=env)


def _run_cpp_bench(data_path: Path, max_events: int | None) -> int:
    root = _repo_root()
    build_dir = root / "build"
    subprocess.check_call(
        [
            "cmake",
            "-S",
            str(root),
            "-B",
            str(build_dir),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        ]
    )
    subprocess.check_call(
        ["cmake", "--build", str(build_dir), "--target", "lobsim_bench_cpp"]
    )
    cmd = [str(build_dir / "lobsim_bench_cpp"), "--path", str(data_path)]
    if max_events and max_events > 0:
        cmd += ["--max-events", str(max_events)]
    return subprocess.call(cmd)


def _run_py_bench(data_path: Path, max_events: int | None) -> int:
    root = _repo_root()
    script = root / "benchmark" / "lobsim_bench_py.py"
    if not script.exists():
        raise FileNotFoundError(f"Python benchmark script not found: {script}")
    env = os.environ.copy()
    env["PYTHONPATH"] = (
        f"{root / 'python'}{os.pathsep}{env.get('PYTHONPATH', '')}".rstrip(os.pathsep)
    )
    cmd = [sys.executable, str(script), "--path", str(data_path)]
    if max_events and max_events > 0:
        cmd += ["--max-events", str(max_events)]
    return subprocess.call(cmd, env=env)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="lobsim", description="LOBSIM CLI")
    sub = parser.add_subparsers(dest="command", required=True)

    demo = sub.add_parser("demo", help="Run a Streamlit demo")
    demo.add_argument("name", choices=["replay", "trend", "arb"], help="Demo name")

    example = sub.add_parser("example", help="Run an example")
    example.add_argument("lang", choices=["cpp", "py"], help="Example language")

    bench = sub.add_parser("bench", help="Run benchmarks")
    bench.add_argument(
        "lang", nargs="?", choices=["cpp", "py"], help="Run a single benchmark"
    )
    bench.add_argument(
        "--path", default=None, help="Override parquet path for benchmarks"
    )
    bench.add_argument(
        "--max-events", type=int, default=0, help="Limit events for benchmarks"
    )
    return parser


def main() -> int:
    parser = _build_parser()
    args = parser.parse_args()

    if args.command == "demo":
        script = _resolve_demo_script(args.name)
        return _run_streamlit(script)
    if args.command == "example":
        data_path = _default_data_path()
        return _run_cpp_example(data_path) if args.lang == "cpp" else _run_py_example()
    if args.command == "bench":
        data_path = Path(args.path) if args.path else _default_data_path()
        max_events = args.max_events if args.max_events > 0 else None
        if args.lang == "cpp":
            return _run_cpp_bench(data_path, max_events)
        if args.lang == "py":
            return _run_py_bench(data_path, max_events)
        status = _run_cpp_bench(data_path, max_events)
        return status if status != 0 else _run_py_bench(data_path, max_events)
    parser.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
