#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${HYDRA_PROJECT_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
BUILD_DIR="${HYDRA_BUILD_DIR:-$ROOT_DIR/build}"
DEFAULT_CONFIG_PATH="$ROOT_DIR/app/config.dev.json"
LEGACY_CONFIG_PATH="$ROOT_DIR/demo/config.dev.json"
if [[ -n "${HYDRA_CONFIG_PATH:-}" ]]; then
  CONFIG_PATH="$HYDRA_CONFIG_PATH"
elif [[ -f "$DEFAULT_CONFIG_PATH" ]]; then
  CONFIG_PATH="$DEFAULT_CONFIG_PATH"
else
  CONFIG_PATH="$LEGACY_CONFIG_PATH"
fi

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  echo "Build directory is not configured: $BUILD_DIR" >&2
  echo "Run CMake configure first (example):" >&2
  echo "  cmake -S . -B build -DV8_INCLUDE_DIR=... -DV8_LIBRARIES=..." >&2
  exit 1
fi

cmake --build "$BUILD_DIR" -j
exec env HYDRA_CONFIG="$CONFIG_PATH" "$BUILD_DIR/hydra_demo"
