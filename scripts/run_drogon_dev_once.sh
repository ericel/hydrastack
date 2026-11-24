#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export HYDRA_PROJECT_ROOT="${HYDRA_PROJECT_ROOT:-$ROOT_DIR}"
export HYDRA_APP_TARGET="${HYDRA_APP_TARGET:-hydra_demo}"
if [[ -z "${HYDRA_CONFIG_PATH:-}" && -f "$HYDRA_PROJECT_ROOT/demo/config.dev.json" ]]; then
  export HYDRA_CONFIG_PATH="$HYDRA_PROJECT_ROOT/demo/config.dev.json"
fi
exec bash "$ROOT_DIR/hydrastack/scaffold/scripts/run_drogon_dev_once.sh" "$@"
