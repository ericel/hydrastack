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
