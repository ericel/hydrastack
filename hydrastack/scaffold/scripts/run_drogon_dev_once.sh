#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${HYDRA_PROJECT_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
BUILD_DIR="${HYDRA_BUILD_DIR:-$ROOT_DIR/build}"
BUILD_JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-1}"
APP_TARGET="${HYDRA_APP_TARGET:-hydra_app}"
SUPERVISOR="${HYDRA_DEV_SUPERVISOR:-$SCRIPT_DIR/dev_supervisor.py}"
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
  echo "  conan install . --output-folder build --build=missing -s build_type=Debug" >&2
  echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=build/build/Debug/generators/conan_toolchain.cmake" >&2
  exit 1
fi

case "${1:-}" in
  --locked-build)
    cmake --build "$BUILD_DIR" --target "$APP_TARGET" --parallel "$BUILD_JOBS"
    exit 0
    ;;
  ""|--build-only) ;;
  *) echo "Unknown option: $1" >&2; exit 2 ;;
esac

python3 "$SUPERVISOR" lock "$BUILD_DIR/.hydra-build.lock" \
  bash "$SCRIPT_DIR/run_drogon_dev_once.sh" --locked-build
[[ "${1:-}" != "--build-only" ]] || exit 0
exec env HYDRA_CONFIG="$CONFIG_PATH" "$BUILD_DIR/$APP_TARGET"
