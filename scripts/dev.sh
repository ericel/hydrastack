#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UI_DIR="$ROOT_DIR/ui"
BUILD_DIR="${HYDRA_BUILD_DIR:-$ROOT_DIR/build}"
CONFIG_PATH="${HYDRA_CONFIG_PATH:-$ROOT_DIR/app/config.dev.json}"
RUN_ONCE_SCRIPT="$ROOT_DIR/scripts/run_drogon_dev_once.sh"
APP_PORT="${HYDRA_APP_PORT:-8070}"
VITE_PORT="${HYDRA_VITE_PORT:-5174}"

if [[ ! -f "$ROOT_DIR/app/config.dev.json" ]]; then
  echo "Missing dev config at app/config.dev.json" >&2
  exit 1
fi

if [[ ! -f "$UI_DIR/package.json" ]]; then
  echo "Missing ui/package.json" >&2
  exit 1
fi

if [[ ! -f "$RUN_ONCE_SCRIPT" ]]; then
  echo "Missing run helper script: $RUN_ONCE_SCRIPT" >&2
  exit 1
fi

ensure_port_free() {
  local port="$1"
  local owners
  owners=$(lsof -nP -iTCP:"$port" -sTCP:LISTEN -t 2>/dev/null || true)
  if [[ -n "$owners" ]]; then
    echo "[HydraDev] Port $port is already in use (PID: $owners)." >&2
    echo "[HydraDev] Stop the existing process or override HYDRA_APP_PORT/HYDRA_VITE_PORT." >&2
    exit 1
  fi
}

ensure_port_free "$APP_PORT"
ensure_port_free "$VITE_PORT"

cleanup() {
  local exit_code=$?
  if [[ -n "${drogon_pid:-}" ]]; then
    kill "$drogon_pid" 2>/dev/null || true
  fi
  if [[ -n "${ssr_watch_pid:-}" ]]; then
    kill "$ssr_watch_pid" 2>/dev/null || true
  fi
  if [[ -n "${vite_pid:-}" ]]; then
    kill "$vite_pid" 2>/dev/null || true
  fi
  exit "$exit_code"
}
trap cleanup EXIT INT TERM

echo "[HydraDev] Starting Vite dev server..."
(
  cd "$UI_DIR"
  npm run dev
) &
vite_pid=$!
sleep 1
if ! kill -0 "$vite_pid" 2>/dev/null; then
  echo "[HydraDev] Vite failed to start. Check logs above." >&2
  exit 1
fi

echo "[HydraDev] Starting SSR bundle watch (ui/src/entry-ssr.tsx -> public/assets/ssr-bundle.js)..."
(
  cd "$UI_DIR"
  npm run build:ssr:watch
) &
ssr_watch_pid=$!
sleep 1
if ! kill -0 "$ssr_watch_pid" 2>/dev/null; then
  echo "[HydraDev] SSR watch failed to start. Check logs above." >&2
  exit 1
fi

echo "[HydraDev] Watching C++ sources for rebuild + restart..."

if command -v watchexec >/dev/null 2>&1; then
  HYDRA_BUILD_DIR="$BUILD_DIR" HYDRA_CONFIG_PATH="$CONFIG_PATH" \
    watchexec \
      --restart \
      --signal SIGTERM \
      --watch "$ROOT_DIR/app/src" \
      --watch "$ROOT_DIR/engine/src" \
      --watch "$ROOT_DIR/engine/include" \
      --watch "$ROOT_DIR/app/config.dev.json" \
      --watch "$ROOT_DIR/public/assets/ssr-bundle.js" \
      --watch "$ROOT_DIR/CMakeLists.txt" \
      --exts cc,cpp,cxx,h,hpp,hh,json,txt \
      -- "$RUN_ONCE_SCRIPT"
elif command -v fswatch >/dev/null 2>&1; then
  while true; do
    HYDRA_BUILD_DIR="$BUILD_DIR" HYDRA_CONFIG_PATH="$CONFIG_PATH" "$RUN_ONCE_SCRIPT" &
    drogon_pid=$!

    fswatch -1 \
      "$ROOT_DIR/app/src" \
      "$ROOT_DIR/engine/src" \
      "$ROOT_DIR/engine/include" \
      "$ROOT_DIR/app/config.dev.json" \
      "$ROOT_DIR/public/assets/ssr-bundle.js" \
      "$ROOT_DIR/CMakeLists.txt" >/dev/null

    kill "$drogon_pid" 2>/dev/null || true
    wait "$drogon_pid" 2>/dev/null || true
    unset drogon_pid
  done
else
  echo "[HydraDev] Install either watchexec or fswatch for C++ hot reload." >&2
  echo "[HydraDev] Running Drogon once without watcher." >&2
  HYDRA_BUILD_DIR="$BUILD_DIR" HYDRA_CONFIG_PATH="$CONFIG_PATH" "$RUN_ONCE_SCRIPT"
fi
