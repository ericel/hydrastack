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
