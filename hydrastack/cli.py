"""HydraStack CLI."""

from __future__ import annotations

import argparse
import ast
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Mapping, Sequence, Set, Tuple

SOURCE_EXTENSIONS = {".ts", ".tsx", ".js", ".jsx"}
TRANSLATION_CALL_PATTERN = re.compile(
    r"\b(?:gettext|_)\(\s*(?P<quote>['\"])(?P<msgid>(?:\\.|(?!\1).)*)\1\s*\)"
)
SCAFFOLD_DIRS = ("app", "cmake", "engine", "ui")
SCAFFOLD_FILES = ("CMakeLists.txt", "conanfile.py", ".gitignore")
SCAFFOLD_IGNORE_NAMES = {
    ".git",
    ".idea",
    ".vscode",
    ".DS_Store",
    "__pycache__",
    "node_modules",
}
APP_README_TEMPLATE = """# {app_name}

Generated with HydraStack.

This project expects the `hydra` CLI to be installed and available on `PATH`.

## Quick Start

Development:
```bash
hydra dev
```

Build:
```bash
hydra build
```

Run:
```bash
hydra run
```

Health checks:
```bash
hydra doctor
```
"""

APP_README_EXTERNAL_ENGINE_SECTION = """
## External Engine Dependency

This app links an installed HydraStack engine package via `HydraStack::hydra_engine`.

Before configure/build, ensure CMake can locate HydraStack:
```bash
export CMAKE_PREFIX_PATH="$HOME/.local/hydrastack:$CMAKE_PREFIX_PATH"
```
"""

THIRD_PARTY_NOTICES_TEMPLATE = """# Third-Party Notices

This project vendors parts of a third-party framework.

## HydraStack

- Project: HydraStack
- License: MIT

```text
{license_text}
```
"""
EXTERNAL_ENGINE_CMAKELISTS = """cmake_minimum_required(VERSION 3.20)
project(HydraStackApp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(drogon CONFIG REQUIRED)
find_package(jsoncpp CONFIG REQUIRED)
find_package(HydraStack CONFIG REQUIRED)

add_executable(hydra_demo
  app/src/main.cc
  app/src/controllers/Home.cc
)

target_link_libraries(hydra_demo
  PRIVATE
    HydraStack::hydra_engine
)

target_compile_definitions(hydra_demo
  PRIVATE
    HYDRA_PUBLIC_DIR=\\"${CMAKE_CURRENT_SOURCE_DIR}/public\\"
)
"""

ANSI_RESET = "\033[0m"
ANSI_BOLD = "\033[1m"
ANSI_RED = "\033[31m"
ANSI_GREEN = "\033[32m"
ANSI_YELLOW = "\033[33m"
ANSI_BLUE = "\033[34m"


def use_ansi_colors() -> bool:
    if os.environ.get("NO_COLOR", "").strip():
        return False
    force_color = os.environ.get("FORCE_COLOR", "").strip()
    if force_color and force_color != "0":
        return True
    if os.environ.get("TERM", "").strip().lower() == "dumb":
        return False
    return bool(getattr(sys.stdout, "isatty", lambda: False)())


def colorize(text: str, *styles: str) -> str:
    if not styles or not use_ansi_colors():
        return text
    return f"{''.join(styles)}{text}{ANSI_RESET}"


def print_hydra(message: str, *, color: str | None = None, error: bool = False) -> None:
    stream = sys.stderr if error else sys.stdout
    tag = colorize("[hydra]", ANSI_BOLD, color) if color else "[hydra]"
    print(f"{tag} {message}", file=stream)


def print_new(message: str, *, color: str | None = None) -> None:
    tag = colorize("[new]", ANSI_BOLD, color) if color else "[new]"
    print(f"{tag} {message}")


def resolve_template_root(template_root_arg: str | None) -> Path:
    def is_valid_template_root(path: Path) -> bool:
        has_demo_or_app = (path / "demo").exists() or (path / "app").exists()
        return has_demo_or_app and (path / "engine").exists()

    if template_root_arg:
        candidate = Path(template_root_arg).expanduser()
        if not candidate.is_absolute():
            candidate = Path.cwd() / candidate
        candidate = candidate.resolve()
        if not is_valid_template_root(candidate):
            raise ValueError(f"invalid template root: {candidate}")
        return candidate

    package_root = Path(__file__).resolve().parent
    candidates = [
        package_root / "scaffold",
        package_root.parent,
    ]

    for candidate in candidates:
        if not candidate.exists():
            continue
        if is_valid_template_root(candidate):
            return candidate

    raise ValueError(
        "unable to locate scaffold template. reinstall hydra or pass --template-root"
    )


def normalize_locale_argument(locale: str) -> str:
    value = locale.strip().replace("-", "_")
    if not value:
        raise ValueError("locale cannot be empty")
    if "/" in value or "\\" in value:
        raise ValueError(f"invalid locale: {locale}")
    return value


def decode_js_string(quote: str, value: str) -> str:
    try:
        return ast.literal_eval(f"{quote}{value}{quote}")
    except Exception:
        return value


def decode_po_literal(line: str) -> str:
    line = line.strip()
    if not line.startswith('"') or not line.endswith('"'):
        return ""
    try:
        decoded = ast.literal_eval(line)
    except Exception:
        return line[1:-1]
    if isinstance(decoded, str):
        return decoded
    return str(decoded)


def po_escape(value: str) -> str:
    return (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def discover_msgids(source_root: Path) -> Set[str]:
    msgids: Set[str] = set()
    for path in source_root.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix not in SOURCE_EXTENSIONS:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except Exception:
            continue
        for match in TRANSLATION_CALL_PATTERN.finditer(text):
            msgid = decode_js_string(match.group("quote"), match.group("msgid")).strip()
            if msgid:
                msgids.add(msgid)
    return msgids


def parse_po(path: Path) -> Dict[str, str]:
    if not path.exists():
        return {}

    entries: Dict[str, str] = {}
    current_msgid: List[str] = []
    current_msgstr: List[str] = []
    mode: str | None = None

    def flush() -> None:
        nonlocal current_msgid, current_msgstr, mode
        msgid = "".join(current_msgid)
        msgstr = "".join(current_msgstr)
        if msgid or msgstr:
            entries[msgid] = msgstr
        current_msgid = []
        current_msgstr = []
        mode = None

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line:
            flush()
            continue
        if line.startswith("#"):
            continue
        if line.startswith("msgid "):
            flush()
            current_msgid.append(decode_po_literal(line[len("msgid ") :]))
            mode = "msgid"
            continue
        if line.startswith("msgstr "):
            current_msgstr.append(decode_po_literal(line[len("msgstr ") :]))
            mode = "msgstr"
            continue
        if line.startswith("msgstr[0] "):
            current_msgstr.append(decode_po_literal(line[len("msgstr[0] ") :]))
            mode = "msgstr"
            continue
        if line.startswith("msgctxt ") or line.startswith("msgid_plural "):
            continue
        if line.startswith('"'):
            if mode == "msgid":
                current_msgid.append(decode_po_literal(line))
            elif mode == "msgstr":
                current_msgstr.append(decode_po_literal(line))
            continue

    flush()
    return entries


def write_po(path: Path, locale: str, extracted_msgids: Set[str], existing: Dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)

    # Preserve existing translations by unioning current ids with extracted ids.
    all_msgids = sorted(extracted_msgids | {msgid for msgid in existing.keys() if msgid})

    lines: List[str] = []
    lines.append(f"# HydraStack UI translations ({locale})")
    lines.append("msgid \"\"")
    lines.append("msgstr \"\"")
    lines.append(f"\"Language: {locale}\\n\"")
    lines.append("\"Content-Type: text/plain; charset=UTF-8\\n\"")
    lines.append("")

    for msgid in all_msgids:
        msgstr = existing.get(msgid, "")
        lines.append(f"msgid \"{po_escape(msgid)}\"")
        lines.append(f"msgstr \"{po_escape(msgstr)}\"")
        lines.append("")

    path.write_text("\n".join(lines), encoding="utf-8")


def compile_mo(entries: Dict[str, str], output_path: Path) -> None:
    # Keep header (msgid "") if present and compile standard GNU MO layout.
    items: List[Tuple[bytes, bytes]] = []
    for msgid, msgstr in sorted(entries.items(), key=lambda kv: kv[0]):
        items.append((msgid.encode("utf-8"), msgstr.encode("utf-8")))

    ids_blob = b"\x00".join(msgid for msgid, _ in items) + b"\x00"
    strs_blob = b"\x00".join(msgstr for _, msgstr in items) + b"\x00"

    offsets: List[Tuple[int, int, int, int]] = []
    id_offset = 0
    str_offset = 0
    for msgid, msgstr in items:
        offsets.append((len(msgid), id_offset, len(msgstr), str_offset))
        id_offset += len(msgid) + 1
        str_offset += len(msgstr) + 1

    key_table_offset = 7 * 4
    value_table_offset = key_table_offset + len(offsets) * 8
    ids_offset = value_table_offset + len(offsets) * 8
    strs_offset = ids_offset + len(ids_blob)

    output = bytearray()
    output += struct.pack("Iiiiiii", 0x950412DE, 0, len(offsets), key_table_offset, value_table_offset, 0, 0)

    for id_len, id_pos, _, _ in offsets:
        output += struct.pack("II", id_len, ids_offset + id_pos)
    for _, _, str_len, str_pos in offsets:
        output += struct.pack("II", str_len, strs_offset + str_pos)

    output += ids_blob
    output += strs_blob

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(output)


def resolve_locales(locales_dir: Path, locales: Sequence[str] | None, include_all: bool) -> List[str]:
    selected: List[str] = []

    if include_all:
        for path in sorted(locales_dir.iterdir() if locales_dir.exists() else []):
            if path.is_dir():
                selected.append(path.name)

    if locales:
        for raw in locales:
            normalized = normalize_locale_argument(raw)
            if normalized not in selected:
                selected.append(normalized)

    if not selected:
        raise ValueError("no locale selected. use -l <locale> or --all")

    return selected


def cmd_makemessages(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    source_root = (root / args.source).resolve()
    locales_dir = (root / args.locales_dir).resolve()

    if not source_root.exists():
        raise ValueError(f"source directory not found: {source_root}")

    extracted = discover_msgids(source_root)
    locales = resolve_locales(locales_dir, args.locale, args.all)

    for locale in locales:
        po_path = locales_dir / locale / "LC_MESSAGES" / "messages.po"
        existing = parse_po(po_path)
        write_po(po_path, locale, extracted, existing)
        print(f"[makemessages] updated {po_path}")

    print(f"[makemessages] extracted {len(extracted)} msgid(s) from {source_root}")
    return 0


def cmd_compilemessages(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    locales_dir = (root / args.locales_dir).resolve()
    locales = resolve_locales(locales_dir, args.locale, args.all)

    for locale in locales:
        po_path = locales_dir / locale / "LC_MESSAGES" / "messages.po"
        if not po_path.exists():
            print(f"[compilemessages] skipping missing file: {po_path}")
            continue
        mo_path = po_path.with_suffix(".mo")
        entries = parse_po(po_path)
        compile_mo(entries, mo_path)
        print(f"[compilemessages] wrote {mo_path}")

    return 0


def run_command(
    cmd: Sequence[str],
    *,
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
) -> int:
    display = " ".join(cmd)
    print_hydra(f"$ {display}", color=ANSI_BLUE)
    completed = subprocess.run(
        list(cmd),
        cwd=str(cwd) if cwd else None,
        env=dict(env) if env is not None else None,
        check=False,
    )
    if completed.returncode in (-signal.SIGINT, -signal.SIGTERM):
        raise KeyboardInterrupt
    if completed.returncode != 0:
        raise ValueError(f"command failed ({completed.returncode}): {display}")
    return completed.returncode


def normalize_mode(args: argparse.Namespace) -> str:
    return "prod" if getattr(args, "prod", False) else "dev"


def add_mode_flags(parser: argparse.ArgumentParser) -> None:
    mode_group = parser.add_mutually_exclusive_group()
    mode_group.add_argument("--dev", action="store_true", help="Use development mode (default)")
    mode_group.add_argument("--prod", action="store_true", help="Use production mode")


def resolve_mode_paths(
    root: Path,
    mode: str,
    build_dir_arg: str | None,
    config_arg: str | None,
) -> Tuple[Path, Path]:
    def default_config_for_mode() -> Path:
        preferred = root / ("app/config.json" if mode == "prod" else "app/config.dev.json")
        legacy = root / ("demo/config.json" if mode == "prod" else "demo/config.dev.json")
        if preferred.exists() or not legacy.exists():
            return preferred
        return legacy

    default_build_dir = root / ("build-prod" if mode == "prod" else "build")
    if build_dir_arg:
        build_dir = Path(build_dir_arg)
        if not build_dir.is_absolute():
            build_dir = root / build_dir
    else:
        build_dir = default_build_dir

    default_config = default_config_for_mode()
    if config_arg:
        config_path = Path(config_arg)
        if not config_path.is_absolute():
            config_path = root / config_path
    else:
        config_path = default_config

    return build_dir.resolve(), config_path.resolve()


def parse_cmake_cache(cache_file: Path) -> Dict[str, str]:
    values: Dict[str, str] = {}
    if not cache_file.exists():
        return values
    for raw_line in cache_file.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith("//"):
            continue
        if ":" not in line or "=" not in line:
            continue
        key_with_type, value = line.split("=", 1)
        key, _sep, _type = key_with_type.partition(":")
        cleaned_key = key.strip()
        if cleaned_key and cleaned_key not in values:
            values[cleaned_key] = value.strip()
    return values


def project_uses_external_engine(root: Path) -> bool:
    cmake_file = root / "CMakeLists.txt"
    if not cmake_file.exists():
        return False
    text = cmake_file.read_text(encoding="utf-8", errors="ignore")
    return (
        "find_package(HydraStack CONFIG REQUIRED)" in text
        and "HydraStack::hydra_engine" in text
    )


def find_hydrastack_config_candidates(root: Path, cache: Mapping[str, str]) -> List[Path]:
    results: List[Path] = []
    seen: Set[Path] = set()

    def add_if_exists(path: Path) -> None:
        resolved = path.resolve()
        if resolved in seen:
            return
        if resolved.exists():
            seen.add(resolved)
            results.append(resolved)

    hydra_dir = cache.get("HydraStack_DIR", "").strip()
    if hydra_dir:
        add_if_exists(Path(hydra_dir) / "HydraStackConfig.cmake")

    raw_prefix_path = os.environ.get("CMAKE_PREFIX_PATH", "").strip()
    cache_prefix_path = cache.get("CMAKE_PREFIX_PATH", "").strip()
    prefix_entries: List[str] = []
    if raw_prefix_path:
        prefix_entries.extend(raw_prefix_path.split(os.pathsep))
    if cache_prefix_path:
        prefix_entries.extend(cache_prefix_path.split(os.pathsep))
    prefix_entries.extend(
        [
            str(root / ".local" / "hydrastack"),
            str(Path.home() / ".local" / "hydrastack"),
            "/usr/local",
            "/opt/homebrew",
            "/usr",
        ]
    )

    for raw_prefix in prefix_entries:
        cleaned = raw_prefix.strip()
        if not cleaned:
            continue
        prefix = Path(cleaned)
        add_if_exists(prefix / "lib" / "cmake" / "HydraStack" / "HydraStackConfig.cmake")

    return results


def first_valid_hydrastack_config(candidates: Sequence[Path]) -> Path | None:
    for candidate in candidates:
        targets = candidate.with_name("HydraStackTargets.cmake")
        if targets.exists():
            return candidate
    return None


def detect_hydrastack_source_checkout() -> Path | None:
    package_root = Path(__file__).resolve().parent
    source_root = package_root.parent
    required = ("CMakeLists.txt", "conanfile.py", "engine")
    for entry in required:
        if not (source_root / entry).exists():
            return None
    return source_root


def bootstrap_hydrastack_engine_install(mode: str) -> Path | None:
    source_root = detect_hydrastack_source_checkout()
    if source_root is None:
        return None

    if not shutil.which("conan"):
        raise ValueError(
            "HydraStack package not found for external-engine app, and conan is unavailable "
            "for automatic engine bootstrap."
        )

    prefix_env = os.environ.get("HYDRA_ENGINE_PREFIX", "").strip()
    install_prefix = (
        Path(prefix_env).expanduser().resolve()
        if prefix_env
        else (Path.home() / ".local" / "hydrastack").resolve()
    )
    build_dir = source_root / "build-engine"
    build_type = "Release" if mode == "prod" else "Debug"

    print_hydra(
        f"HydraStack package missing; bootstrapping engine to {install_prefix} "
        f"from {source_root} ...",
        color=ANSI_YELLOW,
    )
    run_command(
        [
            "conan",
            "install",
            ".",
            "--output-folder",
            str(build_dir),
            "--build=missing",
            "-s",
            f"build_type={build_type}",
        ],
        cwd=source_root,
    )

    cmake_cmd = [
        "cmake",
        "-S",
        str(source_root),
        "-B",
        str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DHYDRA_BUILD_DEMO=OFF",
        "-DHYDRA_BUILD_UI=OFF",
        f"-DCMAKE_INSTALL_PREFIX={install_prefix}",
        "-DHYDRA_V8_AUTODETECT=ON",
    ]
    conan_toolchain = build_dir / "conan_toolchain.cmake"
    if conan_toolchain.exists():
        cmake_cmd.append(f"-DCMAKE_TOOLCHAIN_FILE={conan_toolchain}")
    v8_include = os.environ.get("V8_INCLUDE_DIR", "").strip()
    v8_libs = os.environ.get("V8_LIBRARIES", "").strip()
    if v8_include:
        cmake_cmd.append(f"-DV8_INCLUDE_DIR={v8_include}")
    if v8_libs:
        cmake_cmd.append(f"-DV8_LIBRARIES={v8_libs}")

    run_command(cmake_cmd, cwd=source_root)
    run_command(["cmake", "--build", str(build_dir), "-j", str(max(1, os.cpu_count() or 1))], cwd=source_root)
    run_command(["cmake", "--install", str(build_dir)], cwd=source_root)
    return install_prefix


def describe_v8_from_targets(targets_file: Path) -> Tuple[bool, str]:
    if not targets_file.exists():
        return False, f"missing targets file: {targets_file}"
    text = targets_file.read_text(encoding="utf-8", errors="ignore")
    has_v8_hint = any(
        token in text
        for token in (
            "v8_monolith",
            "v8_libplatform",
            "libv8",
            "/v8/",
            "V8_COMPRESS_POINTERS",
            "V8_ENABLE_SANDBOX",
        )
    )
    if not has_v8_hint:
        return False, "HydraStackTargets.cmake has no V8 include/link hints"
    return True, "V8 include/link hints found in HydraStackTargets.cmake"


def validate_v8_for_runtime(root: Path, build_dir: Path) -> None:
    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.exists():
        raise ValueError(
            f"missing CMake cache in {build_dir}. Build/configure the project before running."
        )

    cache = parse_cmake_cache(cache_file)
    if project_uses_external_engine(root):
        hydra_dir = cache.get("HydraStack_DIR", "").strip()
        if not hydra_dir:
            raise ValueError(
                "HydraStack_DIR is missing from CMake cache. "
                "Set CMAKE_PREFIX_PATH to your HydraStack install prefix and reconfigure."
            )
        config_file = Path(hydra_dir) / "HydraStackConfig.cmake"
        if not config_file.exists():
            raise ValueError(f"HydraStackConfig.cmake not found at {config_file}")
        ok, detail = describe_v8_from_targets(config_file.with_name("HydraStackTargets.cmake"))
        if not ok:
            raise ValueError(f"installed HydraStack package is not V8-ready: {detail}")
        return

    v8_include = cache.get("V8_INCLUDE_DIR", "").strip()
    v8_libs = cache.get("V8_LIBRARIES", "").strip()
    if not v8_include or v8_include == "...":
        raise ValueError(
            "V8_INCLUDE_DIR is missing in CMake cache. "
            "Reconfigure with -DV8_INCLUDE_DIR=... or enable autodetect."
        )
    if not Path(v8_include).exists():
        raise ValueError(f"V8 include directory does not exist: {v8_include}")
    if not v8_libs or v8_libs == "...":
        raise ValueError(
            "V8_LIBRARIES is missing in CMake cache. "
            "Reconfigure with -DV8_LIBRARIES=... or enable autodetect."
        )

    missing_libraries: List[str] = []
    for item in [part.strip() for part in v8_libs.split(";") if part.strip()]:
        library_path = Path(item)
        if library_path.is_absolute() and not library_path.exists():
            missing_libraries.append(item)
    if missing_libraries:
        raise ValueError(
            "configured V8 libraries are missing: " + ", ".join(missing_libraries)
        )


def cmd_doctor(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    mode = normalize_mode(args)
    build_dir, _config_path = resolve_mode_paths(root, mode, args.build_dir, None)
    cache_file = build_dir / "CMakeCache.txt"
    cache = parse_cmake_cache(cache_file)
    external_engine = project_uses_external_engine(root)

    failures = 0

    def report(ok: bool, name: str, detail: str) -> None:
        nonlocal failures
        status = "PASS" if ok else "FAIL"
        if not ok:
            failures += 1
        print(f"[doctor] {status} {name}: {detail}")

    report(True, "project", f"root={root}")
    report(
        True,
        "workflow",
        "external-engine" if external_engine else "vendored-engine",
    )

    if cache_file.exists():
        report(True, "build cache", f"found {cache_file}")
    else:
        report(False, "build cache", f"missing {cache_file}")

    if external_engine:
        configs = find_hydrastack_config_candidates(root, cache)
        if configs:
            report(True, "HydraStackConfig visibility", str(configs[0]))
            ok, detail = describe_v8_from_targets(
                configs[0].with_name("HydraStackTargets.cmake")
            )
            report(ok, "V8 detect result", detail)
        else:
            report(
                False,
                "HydraStackConfig visibility",
                "not found in HydraStack_DIR/CMAKE_PREFIX_PATH/default prefixes",
            )
            report(False, "V8 detect result", "cannot verify without HydraStackConfig.cmake")

        raw_prefix_path = os.environ.get("CMAKE_PREFIX_PATH", "").strip()
        if raw_prefix_path:
            entries = [part for part in raw_prefix_path.split(os.pathsep) if part.strip()]
            missing_entries = [entry for entry in entries if not Path(entry).exists()]
            if missing_entries:
                report(
                    False,
                    "prefix sanity",
                    "non-existent entries in CMAKE_PREFIX_PATH: "
                    + ", ".join(missing_entries),
                )
            else:
                report(True, "prefix sanity", "CMAKE_PREFIX_PATH entries exist")
        else:
            hydra_dir = cache.get("HydraStack_DIR", "").strip()
            if hydra_dir and Path(hydra_dir).exists():
                report(True, "prefix sanity", f"using cached HydraStack_DIR={hydra_dir}")
            else:
                report(
                    False,
                    "prefix sanity",
                    "CMAKE_PREFIX_PATH is empty and HydraStack_DIR is unavailable",
                )
    else:
        try:
            validate_v8_for_runtime(root, build_dir)
            v8_include = cache.get("V8_INCLUDE_DIR", "").strip() if cache else "<missing>"
            report(True, "V8 detect result", f"V8_INCLUDE_DIR={v8_include}")
        except ValueError as exc:
            report(False, "V8 detect result", str(exc))

        auto_detect = cache.get("HYDRA_V8_AUTODETECT", "").strip()
        if auto_detect:
            report(True, "prefix sanity", f"HYDRA_V8_AUTODETECT={auto_detect}")
        else:
            report(True, "prefix sanity", "no prefix requirement for vendored-engine mode")

    if failures:
        raise ValueError(f"doctor failed ({failures} check(s))")
    print("[doctor] all checks passed")
    return 0


def write_external_engine_cmakelists(path: Path) -> None:
    path.write_text(EXTERNAL_ENGINE_CMAKELISTS, encoding="utf-8")


def patch_conanfile_for_external_engine(path: Path) -> None:
    if not path.exists():
        return
    original = path.read_text(encoding="utf-8")
    updated = original.replace('        "engine/*",\n', "")
    updated = updated.replace('        "cmake/*",\n', "")
    updated = updated.replace('        "LICENSE",\n', "")
    if updated != original:
        path.write_text(updated, encoding="utf-8")


def replace_in_file(path: Path, replacements: Sequence[Tuple[str, str]]) -> None:
    if not path.exists():
        return
    original = path.read_text(encoding="utf-8")
    updated = original
    for before, after in replacements:
        updated = updated.replace(before, after)
    if updated != original:
        path.write_text(updated, encoding="utf-8")


def write_app_readme(destination: Path, external_engine: bool) -> None:
    content = APP_README_TEMPLATE.format(app_name=destination.name)
    if external_engine:
        content += APP_README_EXTERNAL_ENGINE_SECTION
    (destination / "README.md").write_text(content, encoding="utf-8")


def write_third_party_notices(destination: Path, template_root: Path) -> None:
    license_file = template_root / "LICENSE"
    license_text = "MIT License"
    if license_file.exists():
        license_text = license_file.read_text(encoding="utf-8").strip()
    content = THIRD_PARTY_NOTICES_TEMPLATE.format(license_text=license_text)
    (destination / "THIRD_PARTY_NOTICES.md").write_text(content, encoding="utf-8")


def resolve_dev_script(root: Path) -> Path:
    project_script = root / "scripts" / "dev.sh"
    if project_script.exists():
        return project_script

    template_root = resolve_template_root(None)
    packaged_script = template_root / "scripts" / "dev.sh"
    if packaged_script.exists():
        return packaged_script

    raise ValueError(
        "missing dev script. Expected scripts/dev.sh in project or bundled scaffold package."
    )


def normalize_generated_layout_to_app(destination: Path) -> None:
    replace_in_file(
        destination / "CMakeLists.txt",
        (
            ("demo/src/main.cc", "app/src/main.cc"),
            ("demo/src/controllers/Home.cc", "app/src/controllers/Home.cc"),
        ),
    )
    replace_in_file(
        destination / "conanfile.py",
        (('"demo/*",', '"app/*",'),),
    )
    replace_in_file(
        destination / "scripts" / "dev.sh",
        (
            (
                'DEFAULT_CONFIG_PATH="$ROOT_DIR/demo/config.dev.json"',
                'DEFAULT_CONFIG_PATH="$ROOT_DIR/app/config.dev.json"',
            ),
            (
                'LEGACY_CONFIG_PATH="$ROOT_DIR/app/config.dev.json"',
                'LEGACY_CONFIG_PATH="$ROOT_DIR/demo/config.dev.json"',
            ),
            ('APP_SRC_DIR="$ROOT_DIR/demo/src"', 'APP_SRC_DIR="$ROOT_DIR/app/src"'),
            (
                'if [[ ! -d "$APP_SRC_DIR" && -d "$ROOT_DIR/app/src" ]]; then',
                'if [[ ! -d "$APP_SRC_DIR" && -d "$ROOT_DIR/demo/src" ]]; then',
            ),
            ('  APP_SRC_DIR="$ROOT_DIR/app/src"', '  APP_SRC_DIR="$ROOT_DIR/demo/src"'),
        ),
    )
    replace_in_file(
        destination / "scripts" / "run_drogon_dev_once.sh",
        (
            (
                'DEFAULT_CONFIG_PATH="$ROOT_DIR/demo/config.dev.json"',
                'DEFAULT_CONFIG_PATH="$ROOT_DIR/app/config.dev.json"',
            ),
            (
                'LEGACY_CONFIG_PATH="$ROOT_DIR/app/config.dev.json"',
                'LEGACY_CONFIG_PATH="$ROOT_DIR/demo/config.dev.json"',
            ),
        ),
    )
    replace_in_file(
        destination / "ui" / "vite.config.ts",
        (
            ('const demoConfig = resolve(__dirname, "../demo/config.json");', 'const appConfig = resolve(__dirname, "../app/config.json");'),
            ('if (fs.existsSync(demoConfig)) {', 'if (fs.existsSync(appConfig)) {'),
            ("return demoConfig;", "return appConfig;"),
            ('return resolve(__dirname, "../app/config.json");', 'return resolve(__dirname, "../demo/config.json");'),
        ),
    )
    replace_in_file(
        destination / "app" / "src" / "main.cc",
        (('std::string configPath = "demo/config.json";', 'std::string configPath = "app/config.json";'),),
    )
    replace_in_file(
        destination / "app" / "src" / "controllers" / "Home.cc",
        (
            ("namespace demo::controllers", "namespace app::controllers"),
            ("}  // namespace demo::controllers", "}  // namespace app::controllers"),
        ),
    )
    files = (
        destination / "README.md",
        destination / "app" / "config.json",
        destination / "app" / "config.dev.json",
    )
    for path in files:
        replace_in_file(
            path,
            (
                ("demo/config.dev.json", "app/config.dev.json"),
                ("demo/config.json", "app/config.json"),
            ),
        )


def ensure_cmake_configured(
    root: Path,
    build_dir: Path,
    *,
    mode: str,
    reconfigure: bool,
) -> None:
    cache_file = build_dir / "CMakeCache.txt"
    if cache_file.exists() and not reconfigure:
        return

    v8_include = os.environ.get("V8_INCLUDE_DIR", "").strip()
    v8_libs = os.environ.get("V8_LIBRARIES", "").strip()
    build_dir.mkdir(parents=True, exist_ok=True)
    build_type = "Release" if mode == "prod" else "Debug"

    toolchain = os.environ.get("CMAKE_TOOLCHAIN_FILE", "").strip()
    conan_toolchain = build_dir / "conan_toolchain.cmake"
    if not toolchain and conan_toolchain.exists():
        toolchain = str(conan_toolchain)

    has_conanfile = (root / "conanfile.py").exists()
    if not toolchain and has_conanfile:
        if shutil.which("conan"):
            print_hydra(
                f"Toolchain missing; running conan install ({build_type})...",
                color=ANSI_YELLOW,
            )
            run_command(
                [
                    "conan",
                    "install",
                    ".",
                    "--output-folder",
                    str(build_dir),
                    "--build=missing",
                    "-s",
                    f"build_type={build_type}",
                ],
                cwd=root,
            )
            if conan_toolchain.exists():
                toolchain = str(conan_toolchain)
        else:
            print_hydra(
                "conan not found; trying plain CMake configure "
                "(install conan or set CMAKE_TOOLCHAIN_FILE if configure fails).",
                color=ANSI_YELLOW,
            )

    hydra_stack_dir = os.environ.get("HydraStack_DIR", "").strip()
    if project_uses_external_engine(root) and not hydra_stack_dir:
        candidates = find_hydrastack_config_candidates(root, parse_cmake_cache(cache_file))
        valid = first_valid_hydrastack_config(candidates)
        if valid is None:
            install_prefix = bootstrap_hydrastack_engine_install(mode)
            if install_prefix is not None:
                bootstrapped_config = (
                    install_prefix / "lib" / "cmake" / "HydraStack" / "HydraStackConfig.cmake"
                )
                if bootstrapped_config.exists() and bootstrapped_config.with_name(
                    "HydraStackTargets.cmake"
                ).exists():
                    valid = bootstrapped_config
        if valid is None:
            raise ValueError(
                "HydraStackConfig.cmake not found for external-engine app. "
                "Install HydraStack engine and set CMAKE_PREFIX_PATH (or HydraStack_DIR) "
                "before running hydra dev."
            )
        hydra_stack_dir = str(valid.parent)
        print_hydra(f"Using HydraStack package from {hydra_stack_dir}", color=ANSI_GREEN)

    cmake_cmd = [
        "cmake",
        "-S",
        str(root),
        "-B",
        str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
    ]

    if toolchain:
        cmake_cmd.append(f"-DCMAKE_TOOLCHAIN_FILE={toolchain}")
    if hydra_stack_dir:
        cmake_cmd.append(f"-DHydraStack_DIR={hydra_stack_dir}")
    if v8_include:
        cmake_cmd.append(f"-DV8_INCLUDE_DIR={v8_include}")
    if v8_libs:
        cmake_cmd.append(f"-DV8_LIBRARIES={v8_libs}")
    if not project_uses_external_engine(root):
        cmake_cmd.append("-DHYDRA_V8_AUTODETECT=ON")

    run_command(cmake_cmd, cwd=root)


def cmd_new(args: argparse.Namespace) -> int:
    root = resolve_template_root(args.template_root)
    destination = Path(args.app).expanduser()
    if not destination.is_absolute():
        destination = Path.cwd() / destination
    destination = destination.resolve()

    if destination.exists():
        if any(destination.iterdir()) and not args.force:
            raise ValueError(
                f"destination already exists and is not empty: {destination}. "
                "Use --force to overwrite scaffold files."
            )
    else:
        destination.mkdir(parents=True, exist_ok=True)

    ignore_func = shutil.ignore_patterns(*SCAFFOLD_IGNORE_NAMES)
    use_external_engine = args.external_engine or not args.vendored_engine
    scaffold_dirs: Sequence[str]
    if use_external_engine:
        scaffold_dirs = tuple(
            directory for directory in SCAFFOLD_DIRS if directory not in {"engine", "cmake"}
        )
    else:
        scaffold_dirs = SCAFFOLD_DIRS

    for directory in scaffold_dirs:
        source_dir = root / directory
        if not source_dir.exists() and directory == "app":
            legacy_demo_dir = root / "demo"
            if legacy_demo_dir.exists():
                source_dir = legacy_demo_dir
        target_dir = destination / directory
        if not source_dir.exists():
            continue
        shutil.copytree(source_dir, target_dir, dirs_exist_ok=True, ignore=ignore_func)

    for filename in SCAFFOLD_FILES:
        source_file = root / filename
        target_file = destination / filename
        if not source_file.exists():
            continue
        target_file.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_file, target_file)

    public_dir = destination / "public"
    (public_dir / "assets").mkdir(parents=True, exist_ok=True)
    favicon = root / "public" / "favicon.ico"
    if favicon.exists():
        shutil.copy2(favicon, public_dir / "favicon.ico")

    if use_external_engine:
        write_external_engine_cmakelists(destination / "CMakeLists.txt")
        patch_conanfile_for_external_engine(destination / "conanfile.py")
        print_new("mode: external-engine (HydraStack::hydra_engine)", color=ANSI_GREEN)
    else:
        print_new("mode: vendored-engine (copies engine/ and cmake/)", color=ANSI_YELLOW)

    if (destination / "app").exists() and not (destination / "demo").exists():
        normalize_generated_layout_to_app(destination)

    write_app_readme(destination, use_external_engine)
    if use_external_engine:
        third_party_notices = destination / "THIRD_PARTY_NOTICES.md"
        if third_party_notices.exists():
            third_party_notices.unlink()
    else:
        write_third_party_notices(destination, root)

    print_new(f"scaffold created at {destination}", color=ANSI_GREEN)
    print_new(f"next: cd {destination} && hydra dev", color=ANSI_BLUE)
    return 0


def cmd_dev(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    mode = normalize_mode(args)
    build_dir, config_path = resolve_mode_paths(root, mode, args.build_dir, args.config)

    dev_script = resolve_dev_script(root)
    if not config_path.exists():
        raise ValueError(f"missing config file: {config_path}")

    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.exists():
        print_hydra(f"Build cache missing; configuring {build_dir} ...", color=ANSI_YELLOW)
    ensure_cmake_configured(root, build_dir, mode=mode, reconfigure=False)
    validate_v8_for_runtime(root, build_dir)

    env = os.environ.copy()
    env["HYDRA_BUILD_DIR"] = str(build_dir)
    env["HYDRA_CONFIG_PATH"] = str(config_path)
    env["HYDRA_PROJECT_ROOT"] = str(root)
    run_command(["bash", str(dev_script)], cwd=root, env=env)
    return 0


def cmd_build(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    mode = normalize_mode(args)
    build_dir, config_path = resolve_mode_paths(root, mode, args.build_dir, args.config)

    if not args.skip_ui:
        ui_dir = root / "ui"
        if not ui_dir.exists():
            raise ValueError(f"missing ui directory: {ui_dir}")
        env = os.environ.copy()
        env["HYDRA_UI_CONFIG_PATH"] = str(config_path)
        env["HYDRA_CONFIG_PATH"] = str(config_path)
        run_command(["npm", "run", "build"], cwd=ui_dir, env=env)

    if not args.skip_cpp:
        ensure_cmake_configured(root, build_dir, mode=mode, reconfigure=args.reconfigure)
        run_command(["cmake", "--build", str(build_dir), "-j", str(args.jobs)], cwd=root)

    return 0


def cmd_run(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    mode = normalize_mode(args)
    build_dir, config_path = resolve_mode_paths(root, mode, args.build_dir, args.config)

    binary_path = build_dir / "hydra_demo"
    if not binary_path.exists():
        suggested = "hydra build --prod" if mode == "prod" else "hydra build"
        raise ValueError(
            f"binary not found: {binary_path}. Run '{suggested}' first."
        )
    if not config_path.exists():
        raise ValueError(f"missing config file: {config_path}")
    validate_v8_for_runtime(root, build_dir)

    env = os.environ.copy()
    env["HYDRA_CONFIG"] = str(config_path)
    cmd = [str(binary_path), str(config_path)]
    extra_args = list(args.extra_args or [])
    if extra_args and extra_args[0] == "--":
        extra_args = extra_args[1:]
    if extra_args:
        cmd.extend(extra_args)
    run_command(cmd, cwd=root, env=env)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="hydra",
        description=(
            "HydraStack CLI: scaffold, dev, build, run, and i18n maintenance.\n"
            "Mode defaults to development; pass --prod for production behavior."
        ),
    )
    parser.add_argument("--root", default=Path.cwd(), help="Project root")
    parser.add_argument(
        "--locales-dir",
        default="ui/locales",
        help="Locales directory relative to project root",
    )

    subparsers = parser.add_subparsers(dest="command", required=True)

    makemessages = subparsers.add_parser("makemessages")
    makemessages.add_argument("-l", "--locale", action="append", help="Locale to update")
    makemessages.add_argument("--all", action="store_true", help="Process all locale directories")
    makemessages.add_argument(
        "--source",
        default="ui/src",
        help="Source directory to scan for gettext/_ calls",
    )
    makemessages.set_defaults(func=cmd_makemessages)

    compilemessages = subparsers.add_parser("compilemessages")
    compilemessages.add_argument("-l", "--locale", action="append", help="Locale to compile")
    compilemessages.add_argument("--all", action="store_true", help="Compile all locale directories")
    compilemessages.set_defaults(func=cmd_compilemessages)

    new_cmd = subparsers.add_parser("new", help="Scaffold a new HydraStack app directory")
    new_cmd.add_argument("app", help="Destination directory for the new app")
    new_cmd.add_argument(
        "--force",
        action="store_true",
        help="Allow scaffolding into an existing non-empty directory",
    )
    new_cmd.add_argument(
        "--template-root",
        help="Template root for hydra new (defaults to bundled scaffold)",
    )
    engine_mode = new_cmd.add_mutually_exclusive_group()
    engine_mode.add_argument(
        "--external-engine",
        action="store_true",
        help=(
            "Use external-engine layout (default): do not copy engine/cmake; generated app "
            "links installed HydraStack::hydra_engine and uses system hydra CLI"
        ),
    )
    engine_mode.add_argument(
        "--vendored-engine",
        action="store_true",
        help="Copy engine/cmake into app (legacy behavior)",
    )
    new_cmd.set_defaults(func=cmd_new)

    doctor_cmd = subparsers.add_parser(
        "doctor",
        help="Validate local HydraStack setup (external-engine visibility, V8 readiness, prefix sanity)",
    )
    add_mode_flags(doctor_cmd)
    doctor_cmd.add_argument(
        "--build-dir",
        help="CMake build directory (defaults: build for dev, build-prod for prod)",
    )
    doctor_cmd.set_defaults(func=cmd_doctor)

    dev_cmd = subparsers.add_parser("dev", help="Run Vite + Drogon watcher stack")
    add_mode_flags(dev_cmd)
    dev_cmd.add_argument(
        "--build-dir",
        help="CMake build directory (defaults: build for dev, build-prod for prod)",
    )
    dev_cmd.add_argument(
        "--config",
        help="Config file path (defaults: app/config.dev.json for dev, app/config.json for prod; demo/* also supported)",
    )
    dev_cmd.set_defaults(func=cmd_dev)

    build_cmd = subparsers.add_parser(
        "build",
        help="Build UI bundles and C++ server",
    )
    add_mode_flags(build_cmd)
    build_cmd.add_argument(
        "--build-dir",
        help="CMake build directory (defaults: build for dev, build-prod for prod)",
    )
    build_cmd.add_argument(
        "--config",
        help="Config file path for UI build mode selection",
    )
    build_cmd.add_argument(
        "--skip-ui",
        action="store_true",
        help="Skip npm UI build step",
    )
    build_cmd.add_argument(
        "--skip-cpp",
        action="store_true",
        help="Skip C++ build step",
    )
    build_cmd.add_argument(
        "--reconfigure",
        action="store_true",
        help="Force CMake configure before build",
    )
    build_cmd.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=max(1, os.cpu_count() or 1),
        help="Parallel build jobs for cmake --build (default: CPU count)",
    )
    build_cmd.set_defaults(func=cmd_build)

    run_cmd = subparsers.add_parser(
        "run",
        help="Run built hydra_demo with selected config",
    )
    add_mode_flags(run_cmd)
    run_cmd.add_argument(
        "--build-dir",
        help="CMake build directory (defaults: build for dev, build-prod for prod)",
    )
    run_cmd.add_argument(
        "--config",
        help="Config file path (defaults: app/config.dev.json for dev, app/config.json for prod; demo/* also supported)",
    )
    run_cmd.add_argument(
        "extra_args",
        nargs=argparse.REMAINDER,
        help="Extra arguments passed to hydra_demo",
    )
    run_cmd.set_defaults(func=cmd_run)

    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except KeyboardInterrupt:
        print_hydra("Interrupted. Shutting down...", color=ANSI_YELLOW, error=True)
        return 130
    except ValueError as exc:
        print(colorize(f"error: {exc}", ANSI_BOLD, ANSI_RED), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
