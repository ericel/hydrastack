#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UI_DIR="$ROOT_DIR/ui"
BUILD_DIR="${HYDRA_BUILD_DIR:-$ROOT_DIR/build}"
CONFIG_PATH="${HYDRA_CONFIG_PATH:-$ROOT_DIR/app/config.dev.json}"
RUN_ONCE_SCRIPT="$ROOT_DIR/scripts/run_drogon_dev_once.sh"
DEV_VERBOSE="${HYDRA_DEV_VERBOSE:-0}"
DEV_LOG_DIR="${HYDRA_DEV_LOG_DIR:-$ROOT_DIR/.hydra/logs}"
VITE_LOG_FILE="${HYDRA_VITE_LOG_FILE:-$DEV_LOG_DIR/vite-dev.log}"

if [[ ! -f "$CONFIG_PATH" ]]; then
  echo "Missing config file at $CONFIG_PATH" >&2
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

read_config_values() {
  python3 - "$CONFIG_PATH" <<'PY'
import json
import sys
from urllib.parse import urlparse

path = sys.argv[1]
address = "0.0.0.0"
port = "8070"
https = "0"
vite_origin = ""
vite_port = ""

try:
    with open(path, "r", encoding="utf-8") as fh:
        cfg = json.load(fh)
except Exception:
    print(f"{address}|{port}|{https}|{vite_origin}|{vite_port}")
    raise SystemExit(0)

listeners = cfg.get("listeners")
if isinstance(listeners, list) and listeners:
    listener = listeners[0]
    if isinstance(listener, dict):
        raw_address = listener.get("address")
        if raw_address:
            address = str(raw_address)
        raw_port = listener.get("port")
        if raw_port is not None:
            port = str(raw_port)
        https = "1" if bool(listener.get("https")) else "0"

plugins = cfg.get("plugins")
if isinstance(plugins, list):
    for plugin in plugins:
        if not isinstance(plugin, dict):
            continue
        if plugin.get("name") != "hydra::HydraSsrPlugin":
            continue
        plugin_cfg = plugin.get("config")
        if not isinstance(plugin_cfg, dict):
            break
        dev_mode = plugin_cfg.get("dev_mode")
        if isinstance(dev_mode, dict):
            raw_vite_origin = dev_mode.get("vite_origin")
            if isinstance(raw_vite_origin, str):
                vite_origin = raw_vite_origin.strip()
        break

if vite_origin:
    parsed = urlparse(vite_origin)
    if parsed.port:
        vite_port = str(parsed.port)

print(f"{address}|{port}|{https}|{vite_origin}|{vite_port}")
PY
}

CONFIG_VALUES="$(read_config_values)"
IFS='|' read -r CONFIG_APP_ADDRESS CONFIG_APP_PORT CONFIG_APP_HTTPS CONFIG_VITE_ORIGIN CONFIG_VITE_PORT <<<"$CONFIG_VALUES"

APP_BIND_ADDRESS="${HYDRA_APP_HOST:-${CONFIG_APP_ADDRESS:-0.0.0.0}}"
APP_PORT="${HYDRA_APP_PORT:-${CONFIG_APP_PORT:-8070}}"
VITE_PORT="${HYDRA_VITE_PORT:-${CONFIG_VITE_PORT:-5174}}"
VITE_ORIGIN="${HYDRA_VITE_ORIGIN:-${CONFIG_VITE_ORIGIN:-http://127.0.0.1:${VITE_PORT}}}"
VITE_ORIGIN="${VITE_ORIGIN%/}"
if [[ -z "$VITE_ORIGIN" ]]; then
  VITE_ORIGIN="http://127.0.0.1:${VITE_PORT}"
fi

APP_SCHEME="http"
if [[ "${CONFIG_APP_HTTPS:-0}" == "1" ]]; then
  APP_SCHEME="https"
fi

if [[ "$APP_BIND_ADDRESS" == "0.0.0.0" || "$APP_BIND_ADDRESS" == "::" || "$APP_BIND_ADDRESS" == "[::]" ]]; then
  APP_PUBLIC_HOST="127.0.0.1"
else
  APP_PUBLIC_HOST="$APP_BIND_ADDRESS"
fi

APP_URL="${APP_SCHEME}://${APP_PUBLIC_HOST}:${APP_PORT}/"

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

print_service_banner() {
  echo
  echo "+---------------------------------------------------------------------+"
  echo "| Hydra dev stack                                                     |"
  echo "+---------------------------------------------------------------------+"
  printf "| %-14s | %s\n" "Drogon app" "$APP_URL"
  printf "| %-14s | %s\n" "Vite (HMR)" "${VITE_ORIGIN}/"
  printf "| %-14s | %s\n" "Config" "$CONFIG_PATH"
  echo "+---------------------------------------------------------------------+"
  echo
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

print_service_banner

echo "[HydraDev] Open your app at: $APP_URL"
echo "[HydraDev] Starting Vite dev server..."
if [[ "$DEV_VERBOSE" == "1" ]]; then
  (
    cd "$UI_DIR"
    HYDRA_UI_CONFIG_PATH="$CONFIG_PATH" npm run dev -- --host 127.0.0.1 --port "$VITE_PORT" --strictPort
  ) &
else
  mkdir -p "$DEV_LOG_DIR"
  : >"$VITE_LOG_FILE"
  echo "[HydraDev] Vite logs: $VITE_LOG_FILE (set HYDRA_DEV_VERBOSE=1 for full output)"
  (
    cd "$UI_DIR"
    HYDRA_UI_CONFIG_PATH="$CONFIG_PATH" npm run dev -- --host 127.0.0.1 --port "$VITE_PORT" --strictPort >"$VITE_LOG_FILE" 2>&1
  ) &
fi
vite_pid=$!
sleep 1
if ! kill -0 "$vite_pid" 2>/dev/null; then
  echo "[HydraDev] Vite failed to start." >&2
  if [[ "$DEV_VERBOSE" != "1" && -f "$VITE_LOG_FILE" ]]; then
    echo "[HydraDev] Last Vite log lines:" >&2
    tail -n 40 "$VITE_LOG_FILE" >&2
  fi
  exit 1
fi

echo "[HydraDev] Starting SSR bundle watch (ui/src/entry-ssr.tsx -> public/assets/ssr-bundle.js)..."
(
  cd "$UI_DIR"
  HYDRA_UI_CONFIG_PATH="$CONFIG_PATH" npm run build:ssr:watch
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
      --watch "$CONFIG_PATH" \
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
      "$CONFIG_PATH" \
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
