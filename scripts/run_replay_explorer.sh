#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app_path="${root_dir}/examples/lobsim_replay_explorer.py"

python -m streamlit run "${app_path}"
