# HydraStack

Drogon routes. V8 renders. React designs.

HydraStack is a framework layer on top of Drogon that embeds V8 for React SSR. Drogon remains the system of record for routing, auth, and data. React SSR is treated as a pure render contract:

`(url, propsJson) -> html`

Rendering boundary (React-first):

- App/page markup templates live in React SSR (`ui/src/entry-ssr.tsx`, `ui/src/App.tsx`).
- C++ owns outer document shell + runtime policy (`HtmlShell` wrapping, CSP/security headers in `HydraSsrPlugin`).
- No separate server-side HTML template engine is required for app pages.

## v0.1 Scope

- No Node runtime dependency in production.
- V8 isolate pool for multi-threaded rendering.
- Single deterministic SSR bundle loaded by C++.
- Independent client bundle for hydration.

## Stability & Support

HydraStack is currently **v0.1 beta**.

- Stable for internal use and prototype production deployments.
- Core architecture is stable: Drogon routing/data authority + React SSR rendering + C++ shell/security policy.
- Before `v1.0`, some config keys, CLI/scaffold ergonomics, and demo/template details may still evolve.

Compatibility policy for `v0.x`:

- Breaking changes are still possible.
- When breaking changes happen, they are documented in `README` milestone notes and migration guidance.

## Build Notes

### Conan dependencies

Conan manages Drogon and JSON:

- `drogon/1.9.10`
- `jsoncpp/1.9.5`

Conan recipe intent:

- `conanfile.py` packages the engine boundary (`hydra_engine`) only.
- Demo/template sources (`app/`, `ui/`) are intentionally excluded from package exports.

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

Engine-only boundary build:

```bash
cmake -S . -B build-engine \
  -DHYDRA_BUILD_DEMO=OFF \
  -DV8_INCLUDE_DIR=/path/to/v8/include \
  -DV8_LIBRARIES="/path/to/v8/libs"
cmake --build build-engine -j
```

## Split-Ready Boundary (Milestone 6)

HydraStack is structured as a monorepo that is ready for a future engine/template split.

Package boundary:

- `engine/` and `cmake/` are the stable C++ package surface.
- `app/`, `ui/`, and most `scripts/` are demo/template workflow.

What this means today:

- CMake supports engine-only builds via `-DHYDRA_BUILD_DEMO=OFF`.
- Conan packaging exports engine-focused sources only and builds/installs `hydra_engine`.
- Installed CMake target for consumers is `HydraStack::hydra_engine`.

Versioning recommendation:

- Engine versions move independently from template/scaffold revisions.
- Template changes should not force engine API churn.

When to split into separate repos:

- when publishing `hydrastack` as a stable Conan dependency for external teams
- when template/UI release cadence diverges from engine cadence
- when CI/security requirements demand strict separation of Node and C++ artifacts

## Hydra CLI

Install editable while developing this repo:

```bash
pip install -e .
```

Then use the global command:

- `hydra new <app>`: scaffold a new HydraStack app directory
- `hydra dev`: run Vite + Drogon watcher stack (development mode by default)
- `hydra build`: build UI bundles + C++ server
- `hydra run`: run built `hydra_demo` with selected config
- `hydra makemessages ...`, `hydra compilemessages ...`: i18n maintenance

Mode flags:

- `--dev` (default)
- `--prod`

Examples:

```bash
hydra dev
hydra dev --prod

hydra build
hydra build --prod

hydra run
hydra run --prod
```

`./hydra ...` still works in this repo as a local launcher.

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
- Dev output is concise by default (focus on app URL and rebuild flow).
  - Full Vite terminal output: `HYDRA_DEV_VERBOSE=1 ./scripts/dev.sh`
  - Default Vite log file: `.hydra/logs/vite-dev.log`
- `hydra dev --prod` now runs in production-asset loop:
  - skips Vite dev server (`asset_mode=prod`)
  - builds SSR + client bundles once at startup
  - watches both SSR and client builds so manifest/hash changes are refreshed
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

### Local Load Baseline (Example, localhost)

The following measurements are from one local machine and are intended as a baseline, not a hard SLA:

- Fast route (`/`) at `c=100`: ~9,278 req/s, p95 ~16ms, p99 ~31ms, 0 failures.
- Fast route (`/`) at `c=200`: ~8,964 req/s, p95 ~36ms, p99 ~74ms, 0 failures.
- Fast route (`/`) at `c=400`: timeouts/errors begin (observed non-2xx and timeout counter increments), p99 ~189ms.
- Practical takeaway for this setup: route `/` is clean at 100-200 concurrency; 400 is beyond clean stability.

Real SSR saturation sample (`?burn_ms=10`):

- `c=50`: ~824.6 req/s, mean ~60.6ms, p95 ~76ms, p99 ~118ms, 0 AB failures.
- Mean latency above 10ms is expected: queueing + isolate pool contention + HTTP overhead.

Metric interpretation notes:

- `hydra_render_latency_ms` is engine-side SSR render time, not full request time.
- `hydra_request_total_ms` is end-to-end server-side request handling time.
- `hydra_acquire_wait_ms` measures isolate wait inside handler execution. If `threads_num == pool_size`, queueing can happen before isolate acquire, so acquire-wait may remain low while request latency grows.

To force observable isolate contention and validate acquire-wait:

1. Run with `threads_num > pool_size` (for example `threads_num=16`, `pool_size=4`) and `acquire_timeout_ms=0`.
2. Capture metrics before and after a burn run, then compare deltas:
```bash
curl -s http://127.0.0.1:8070/__hydra/metrics > /tmp/hydra-metrics-before.txt
ab -k -n 20000 -c 100 "http://127.0.0.1:8070/?burn_ms=20"
curl -s http://127.0.0.1:8070/__hydra/metrics > /tmp/hydra-metrics-after.txt
```
3. Inspect delta changes in `hydra_acquire_wait_ms_(bucket|sum|count)`, `hydra_request_total_ms_(bucket|sum|count)`, and `hydra_requests_by_code_total`.

To protect timeout tests from acquire-timeout interference:

- Use `acquire_timeout_ms=0` (or a very large value) when validating render timeouts.
- Keep concurrency modest for timeout-path checks (for example `c=4`) so failures are render-driven, not acquire-driven.

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

- `asset_mode`: explicit asset pipeline mode.
  - `prod`: use manifest/static resolved assets (hashed output).
  - `dev`: use Vite dev assets/HMR paths.
  - `auto`: backward-compatible behavior inferred from `dev_mode.enabled`.
- `dev_mode.enabled`: enable dev mode.
- `dev_mode.proxy_assets`: proxy `/assets/*`, `/@vite/*`, `/src/*`, `/node_modules/*` to Vite.
- `dev_mode.vite_origin`: Vite server origin (default `http://127.0.0.1:5174`).
- `dev_mode.css_path`: dev stylesheet path (default `/src/styles.css`).
- `dev_mode.inject_hmr_client`: inject `@vite/client` into HTML shell.
- `dev_mode.client_entry_path`: module entry used for client hydration in dev.
- `dev_mode.ansi_color_logs`: enable ANSI-colored Hydra startup logs in dev (TTY only).
- `api_bridge_enabled`: enable `globalThis.hydra.fetch(...)` bridge in SSR runtime.

Asset mode precedence:

1. explicit `asset_mode` (recommended)
2. legacy `dev_mode.enabled` (used only when `asset_mode=auto` or missing)

### Milestone 5.4 Notes (CSS Obfuscation Build Option)

HydraStack supports optional build-time CSS class mangling (advanced mode).

Plugin config:

```json
"css_obfuscation": {
  "enabled": true,
  "emit_classmap": true
}
```

Behavior:

- When enabled, `ui` build rewrites static JSX `className` tokens to mangled names.
- Generated CSS selectors are rewritten to the same mangled names.
- `public/assets/classmap.json` is emitted when `emit_classmap=true`.
- Runtime config does not perform obfuscation; this is build-time only.

Build picks config from:

1. `HYDRA_UI_CONFIG_PATH` (if set)
2. `HYDRA_CONFIG_PATH` (if set)
3. fallback `app/config.json`

SSR request context:

- C++ now calls `globalThis.render(url, propsJson, requestContextJson)`.
- `requestContextJson` includes `url`, `routePath`, `pathWithQuery`, `routeUrl`, `path`, `query`,
  `method`, `headers`, and `locale`.
- UI receives this as `initialProps.__hydra_request`.

Demo props naming:

- `pathWithQuery` is the canonical key for app props.
- Snake_case aliases have been removed from the public props/request-context API.

### Milestone 5.1 Notes (I18n Locale Resolution)

HydraStack can resolve request locale at the framework layer and inject it into SSR context.

`i18n` plugin config keys:

- `defaultLocale`: fallback locale (default `en`)
- `supportedLocales`: ordered list of supported locales
- `queryParam`: locale query key (default `lang`)
- `cookieName`: locale cookie key (default `hydra_lang`)
- `includeLocaleCandidates`: include fallback candidates in `__hydra_request.localeCandidates`

Locale files (Django-style layout):

- `ui/locales/en/LC_MESSAGES/messages.po`
- `ui/locales/fr/LC_MESSAGES/messages.po`
- `ui/locales/ko/LC_MESSAGES/messages.po`

Hydra CLI (Django-like):

- `hydra makemessages -l fr`
- `hydra makemessages --all`
- `hydra compilemessages -l fr`
- `hydra compilemessages --all`

Notes:

- `makemessages` scans `ui/src` for `_("...")` and `gettext("...")`.
- It creates/updates `ui/locales/<locale>/LC_MESSAGES/messages.po`.
- `compilemessages` compiles `.po` into `.mo` at the same location.
- Locale args accept Django-style names such as `zh_Hans`.

React translation usage:

- Use `createGettext(locale)` from `ui/src/i18n/gettext.ts`.
- Use either `gettext("key")` or `_("key")` style (same function).
- Demo page uses keys like `hello_from_hydrastack`, `route`, `hydrated_clicks`.

## Milestone 7 Notes (Production Guarantees)

### Config Validation (Fail-Fast)

HydraSsrPlugin now validates and normalizes startup config via:

- `engine/include/hydra/Config.h`
- `engine/src/Config.cc`

`initAndStart()` consumes `validateAndNormalizeHydraSsrPluginConfig(config)` and logs a
single resolved summary line.

Validation rules include:

- invalid `asset_mode` -> startup error
- malformed `dev_mode.vite_origin` -> startup error
- invalid timeout ranges -> startup error
- unknown keys under `dev_mode` -> startup error
- in prod mode, missing/invalid manifest -> startup error
- API bridge defaults to off in prod unless explicitly enabled

### SSR HTTP Contract (Status + Redirect + Headers)

SSR contract now supports either:

- legacy plain HTML string (still supported)
- JSON envelope string:

```json
{
  "html": "<div>...</div>",
  "status": 200,
  "headers": {},
  "redirect": null
}
```

In `ui/src/entry-ssr.tsx`, `globalThis.render` returns the envelope.
In C++, `HydraSsrPlugin` parses it and `Home.cc` applies status + headers on the Drogon response.

Demo routes for semantics checks:

- `GET /go-home` -> SSR 302 redirect (`Location: /`)
- `GET /not-found` -> SSR 404 page from a matched Hydra route

404 behavior is unified for document requests:

- Unmatched route (for example `GET /m`) now goes through Drogon's custom error handler and renders Hydra SSR error UI with HTTP 404.
- Matched route can also return HTTP 404 via SSR envelope (for example `GET /not-found`).
- Non-document requests (assets/modules/etc.) still bypass this UI path and use Drogon's fallback error response.

### Observability

Request IDs:

- Accepts incoming `X-Request-Id` (sanitized), otherwise generates `hydra-<counter>`.
- Exposed in `__hydra_request.requestId`.
- Included in request/metrics/error logs.

Prometheus endpoint:

- `GET /__hydra/metrics`

Metrics include:

- `hydra_render_latency_ms` (histogram, engine-side SSR render time)
- `hydra_acquire_wait_ms` (histogram, isolate acquire wait time)
- `hydra_request_total_ms` (histogram, end-to-end request handling time)
- `hydra_pool_in_use` (gauge)
- `hydra_render_timeouts_total`
- `hydra_recycles_total`
- `hydra_render_errors_total`
- `hydra_requests_by_code_total` (counter with `code` label)
- `hydra_requests_total` (counter with `status=ok|fail`)

API bridge hardening:

- allowed methods (default `GET,POST`)
- allowed path prefixes (default `/hydra/internal/`)
- max request body bytes (default `65536`)

Resolution order:

1. cookie (`cookieName`)
2. query parameter (`queryParam`)
3. `Accept-Language` (q-values honored)
4. `defaultLocale`

Normalization and fallback:

- locale tags are normalized (for example `fr_CA` -> `fr-ca`)
- region fallback is applied (`fr-ca` -> `fr`)
- unsupported locales fall back to `defaultLocale`

### Theme Pathway

HydraStack can resolve a request theme and inject it into SSR context as
`__hydra_request.theme` (and optionally `__hydra_request.themeCandidates`).

`theme` plugin config keys:

- `defaultTheme`: fallback theme (default `ocean`)
- `supportedThemes`: ordered list of allowed themes
- `queryParam`: theme query key (default `theme`)
- `cookieName`: theme cookie key (default `hydra_theme`)
- `includeThemeCandidates`: include candidate chain in request context

Resolution order:

1. cookie (`cookieName`)
2. query parameter (`queryParam`)
3. `defaultTheme`

UI behavior:

- App root sets `data-theme` from `__hydra_request.theme`.
- Theme tokens are defined in `ui/src/styles.css`.
- Built-in demo themes: `ocean`, `sunset`, `forest`.

API bridge contract:

- `globalThis.hydra.fetch({ method, path, query, headers, body })`
- returns `{ status, body, headers }`
- default demo handlers:
  - `GET /hydra/internal/health` -> `200 ok`
  - `* /hydra/internal/echo` -> echoes request body

## Test Route

Use these routes to validate the app and hot-restart behavior:

- `GET /__hydra/test`
- `GET /__hydra/metrics` (Prometheus-style SSR counters/latency sums)
- `GET /go-home` (SSR redirect contract)
- `GET /not-found` (SSR 404 contract on matched route)
- `GET /m` (unmatched route rendered by unified Hydra error UI)

It returns JSON including:

- `ok`
- `path`
- `query`
- `process_started_ms` (changes after app restart)

## Test Gates

Native config fail-fast unit test (Milestone 8.1):

```bash
ctest --test-dir build --output-on-failure -R hydra_config_validation
```

End-to-end SSR integration smoke test (routing + i18n + request context + metrics):

```bash
python3 scripts/test_ssr_integration.py
```

Micro load regression gate (Milestone 8.3):

```bash
python3 scripts/test_load_regression.py
```

Run all CI-facing gates after building:

```bash
ctest --test-dir build --output-on-failure -R "hydra_config_validation|hydra_ssr_integration|hydra_load_regression"
```
