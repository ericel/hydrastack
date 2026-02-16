# HydraStack Getting Started

This guide is the shortest path from clone to a running app.

## Prerequisites

Install these first:

- `git`
- `cmake >= 3.20`
- C++20 compiler (`clang`/`gcc`; on macOS, Xcode Command Line Tools)
- `python >= 3.10`
- `pip`
- `node >= 18` and `npm`
- `conan >= 2`
- V8 headers/libs (or Homebrew V8 on macOS)

Optional but recommended for hot-reload ergonomics:

- `watchexec` (preferred) or `fswatch`

### V8 Notes

HydraStack needs V8 at build/runtime.

- Auto-detect is enabled by default (`HYDRA_V8_AUTODETECT=ON`).
- You can also set explicit paths:
  - `V8_INCLUDE_DIR`
  - `V8_LIBRARIES`

On macOS (Homebrew):

```bash
brew install v8
```

## 1) Clone And Install CLI

```bash
git clone <your-hydrastack-repo-url> hydrastack
cd hydrastack
pip install -e .
hydra --help
```

## 2) Run HydraStack Demo (Repo Workflow)

This runs the built-in demo app in this repository.

```bash
cd hydrastack
conan install . --output-folder=build --build=missing -s build_type=Debug
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
cmake --build build -j
hydra dev
```

Production asset mode:

```bash
hydra dev --prod
```

## 3) Create A New App (External Engine Workflow)

Use this when you want your app to consume installed HydraStack engine as a dependency.

### 3.1 Install HydraStack Engine From Source

```bash
cd hydrastack
conan install . --output-folder=build-engine --build=missing -s build_type=Release
cmake -S . -B build-engine \
  -DCMAKE_TOOLCHAIN_FILE=build-engine/conan_toolchain.cmake \
  -DHYDRA_BUILD_DEMO=OFF \
  -DHYDRA_BUILD_UI=OFF \
  -DCMAKE_INSTALL_PREFIX=$HOME/.local/hydrastack
cmake --build build-engine -j
cmake --install build-engine
```

### 3.2 Scaffold And Run App

```bash
hydra new myapp --external-engine
cd myapp
export CMAKE_PREFIX_PATH="$HOME/.local/hydrastack:$CMAKE_PREFIX_PATH"
hydra doctor
hydra dev
```

Production asset mode:

```bash
hydra dev --prod
```

## 4) Useful Commands

- Build UI + C++:
  - `hydra build`
  - `hydra build --prod`
- Run built server:
  - `hydra run`
  - `hydra run --prod`
- Validate environment:
  - `hydra doctor`

## Troubleshooting

- `HydraStackConfig.cmake not found`
  - set `CMAKE_PREFIX_PATH` to your install prefix (`$HOME/.local/hydrastack`)
  - run `hydra doctor`

- `HydraSsrPlugin manifest not found` during `hydra dev --prod`
  - ensure you are on latest HydraStack scaffold/CLI
  - `hydra dev --prod` should build client + SSR assets before app start

- V8 errors at configure/runtime
  - set `V8_INCLUDE_DIR` and `V8_LIBRARIES` explicitly
  - rerun `hydra doctor`
