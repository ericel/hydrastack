# HydraStack

Drogon routes. V8 renders. React designs.

HydraStack is a framework layer on top of Drogon that embeds V8 for React SSR. Drogon remains the system of record for routing, auth, and data. React SSR is treated as a pure render contract:

`(url, propsJson) -> html`

## v0.1 Scope

- No Node runtime dependency in production.
- V8 isolate pool for multi-threaded rendering.
- Single deterministic SSR bundle loaded by C++.
- Independent client bundle for hydration.

## Build Notes

### Conan dependencies

Conan manages Drogon and JSON:

- `drogon/1.9.10`
- `jsoncpp/1.9.5`

### V8 dependency

For v0.1, V8 is provided externally (system install or prebuilt path). Configure CMake with:

- `-DV8_INCLUDE_DIR=/path/to/v8/include`
- `-DV8_LIBRARIES="/path/to/libv8.a;/path/to/libv8_libplatform.a;..."`
- optional: `-DV8_COMPILE_DEFINITIONS="V8_COMPRESS_POINTERS;V8_ENABLE_SANDBOX"` for V8 builds that require feature macros to match.

On macOS with Homebrew V8 (`/opt/homebrew/opt/v8`), HydraStack auto-applies:

- `V8_COMPRESS_POINTERS`
- `V8_ENABLE_SANDBOX`

### Typical flow

```bash
cd hydrastack
conan install . --output-folder=build --build=missing -s build_type=Release
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake \
  -DV8_INCLUDE_DIR=/path/to/v8/include \
  -DV8_LIBRARIES="/path/to/v8/libs"
cmake --build build -j
./build/hydra_demo
```

If you want CMake to trigger UI bundling, configure with `-DHYDRA_BUILD_UI=ON`.

## Dev Hot Reload

HydraStack now supports dev flow with:

- Vite HMR for React/Tailwind changes.
- Auto rebuild + restart for Drogon/C++ source changes.
- SSR bundle watch + app restart when `ui/src/entry-ssr.tsx` changes.

Files:

- `app/config.dev.json`: dev config with `dev_mode.enabled=true`.
- `scripts/dev.sh`: starts Vite and watches C++ source changes.
- `scripts/dev.sh`: starts Vite, SSR bundle watch, and watches C++ source changes.
- `scripts/run_drogon_dev_once.sh`: build + run one Drogon instance.

Usage:

```bash
cd hydrastack
./scripts/dev.sh
```

Notes:

- C++ watcher uses `watchexec` if available, otherwise `fswatch`.
- If neither is installed, the script runs Drogon once and prints a warning.
- You can override paths:
  - `HYDRA_BUILD_DIR=/path/to/build`
  - `HYDRA_CONFIG_PATH=/path/to/config.dev.json`
- You can override expected ports for startup checks:
  - `HYDRA_APP_PORT=8070`
  - `HYDRA_VITE_PORT=5174`
- `main.cc` also supports `HYDRA_CONFIG` env var or a CLI arg:
  - `HYDRA_CONFIG=app/config.dev.json ./build/hydra_demo`
  - `./build/hydra_demo app/config.dev.json`

### UI artifact build

HydraStack expects two UI build passes:

- SSR bundle: deterministic `public/assets/ssr-bundle.js`
- Client bundle: hashed JS/CSS plus `public/assets/manifest.json`

```bash
cd ui
npm install
npm run build
```

## Milestone 2 Notes (Concurrency)

HydraStack uses an isolate pool with RAII leases.

- `pool_size`: optional pool size override.
- `isolate_pool_size`: legacy key still supported.
- `acquire_timeout_ms`: optional timeout while waiting for a free isolate (`0` means wait forever).

Quick load check:

```bash
ab -n 200 -c 20 http://127.0.0.1:8070/
```

Contention and isolate-state proof checks:

1. Start with serialized pool:
   set `pool_size` to `1` in `app/config.json`, run `./build/hydra_demo`, then:
```bash
ab -k -n 5000 -c 200 "http://127.0.0.1:8070/?burn_ms=3"
```
2. Switch to parallel pool:
   set `pool_size` to `4` (or thread count), restart, run the same command.
3. Isolate global persistence check:
```bash
curl -s "http://127.0.0.1:8070/?counter=1" | rg "Isolate counter"
curl -s "http://127.0.0.1:8070/?counter=1" | rg "Isolate counter"
```
With `pool_size=1`, the counter should increase monotonically between requests.

## Milestone 3 Notes (Manifest + Hashed Assets)

HydraSsrPlugin can resolve CSS/JS from Vite's manifest automatically. You no longer need
hardcoded `css_path` and `client_js_path` in `app/config.json`.

Recommended plugin config:

```json
{
  "name": "hydra::HydraSsrPlugin",
  "dependencies": [],
  "config": {
    "ssr_bundle_path": "./public/assets/ssr-bundle.js",
    "asset_manifest_path": "./public/assets/manifest.json",
    "asset_public_prefix": "/assets",
    "client_manifest_entry": "src/entry-client.tsx",
    "pool_size": 4,
    "acquire_timeout_ms": 200,
    "render_timeout_ms": 50,
    "wrap_fragment": true
  }
}
```

Notes:

- `wrap_fragment` should be `true` when `globalThis.render` returns only app markup.
- `asset_public_prefix` is prepended to manifest file names when they are relative.
- `css_path` and `client_js_path` are still accepted as explicit overrides.

## Milestone 4 Notes (Dev Mode + Routing Context + API Bridge)

HydraStack now supports a development mode where Drogon continues SSR in C++/V8 while
frontend requests can be proxied to Vite.

`app/config.json` plugin keys:

- `dev_mode.enabled`: enable dev mode.
- `dev_mode.proxy_assets`: proxy `/assets/*`, `/@vite/*`, `/src/*`, `/node_modules/*` to Vite.
- `dev_mode.vite_origin`: Vite server origin (default `http://127.0.0.1:5174`).
- `dev_mode.css_path`: dev stylesheet path (default `/src/styles.css`).
- `dev_mode.inject_hmr_client`: inject `@vite/client` into HTML shell.
- `dev_mode.client_entry_path`: module entry used for client hydration in dev.
- `api_bridge_enabled`: enable `globalThis.hydra.fetch(...)` bridge in SSR runtime.

SSR request context:

- C++ now calls `globalThis.render(url, propsJson, requestContextJson)`.
- `requestContextJson` includes `url`, `routePath`, `pathWithQuery`, `routeUrl`, `path`, `query`,
  `method`, and `headers`.
- UI receives this as `initialProps.__hydra_request`.

Demo props naming:

- `pathWithQuery` is the canonical key for app props.
- Snake_case aliases have been removed from the public props/request-context API.

API bridge contract:

- `globalThis.hydra.fetch({ method, path, query, headers, body })`
- returns `{ status, body, headers }`
- default demo handlers:
  - `GET /hydra/internal/health` -> `200 ok`
  - `* /hydra/internal/echo` -> echoes request body

## Test Route

Use this route to validate the app and hot-restart behavior:

- `GET /__hydra/test`

It returns JSON including:

- `ok`
- `path`
- `query`
- `process_started_ms` (changes after app restart)
