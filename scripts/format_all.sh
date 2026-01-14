#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"${script_dir}/format_cpp.sh"

if ! command -v ruff >/dev/null 2>&1; then
  echo "ruff not found. Install with: python -m pip install ruff"
  exit 1
fi

ruff format "${script_dir}/../python"
