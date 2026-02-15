"""HydraStack CLI."""

from __future__ import annotations

import argparse
import ast
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Mapping, Sequence, Set, Tuple

SOURCE_EXTENSIONS = {".ts", ".tsx", ".js", ".jsx"}
TRANSLATION_CALL_PATTERN = re.compile(
    r"\b(?:gettext|_)\(\s*(?P<quote>['\"])(?P<msgid>(?:\\.|(?!\1).)*)\1\s*\)"
)
SCAFFOLD_DIRS = ("app", "cmake", "engine", "scripts", "ui")
SCAFFOLD_FILES = ("CMakeLists.txt", "conanfile.py", "README.md", "LICENSE", ".gitignore", "hydra")
SCAFFOLD_IGNORE_NAMES = {
    ".git",
    ".idea",
    ".vscode",
    ".DS_Store",
    "__pycache__",
    "node_modules",
}


def resolve_template_root(template_root_arg: str | None) -> Path:
    if template_root_arg:
        candidate = Path(template_root_arg).expanduser()
        if not candidate.is_absolute():
            candidate = Path.cwd() / candidate
        candidate = candidate.resolve()
        if not (candidate / "app").exists() or not (candidate / "engine").exists():
            raise ValueError(f"invalid template root: {candidate}")
        return candidate

    package_root = Path(__file__).resolve().parent
    candidates = [
        package_root.parent,
        package_root / "scaffold",
    ]

    for candidate in candidates:
        if not candidate.exists():
            continue
        if (candidate / "app").exists() and (candidate / "engine").exists():
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
    print(f"[hydra] $ {display}")
    completed = subprocess.run(
        list(cmd),
        cwd=str(cwd) if cwd else None,
        env=dict(env) if env is not None else None,
        check=False,
    )
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
    default_build_dir = root / ("build-prod" if mode == "prod" else "build")
    if build_dir_arg:
        build_dir = Path(build_dir_arg)
        if not build_dir.is_absolute():
            build_dir = root / build_dir
    else:
        build_dir = default_build_dir

    default_config = root / ("app/config.json" if mode == "prod" else "app/config.dev.json")
    if config_arg:
        config_path = Path(config_arg)
        if not config_path.is_absolute():
            config_path = root / config_path
    else:
        config_path = default_config

    return build_dir.resolve(), config_path.resolve()


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
    if not cache_file.exists() and (not v8_include or not v8_libs):
        raise ValueError(
            "Build directory is not configured. Set V8_INCLUDE_DIR and V8_LIBRARIES, "
            "or configure CMake once manually."
        )

    build_dir.mkdir(parents=True, exist_ok=True)
    build_type = "Release" if mode == "prod" else "Debug"
    cmake_cmd = [
        "cmake",
        "-S",
        str(root),
        "-B",
        str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
    ]

    toolchain = os.environ.get("CMAKE_TOOLCHAIN_FILE", "").strip()
    if toolchain:
        cmake_cmd.append(f"-DCMAKE_TOOLCHAIN_FILE={toolchain}")
    if v8_include:
        cmake_cmd.append(f"-DV8_INCLUDE_DIR={v8_include}")
    if v8_libs:
        cmake_cmd.append(f"-DV8_LIBRARIES={v8_libs}")

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
    for directory in SCAFFOLD_DIRS:
        source_dir = root / directory
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

    hydra_path = destination / "hydra"
    if hydra_path.exists():
        hydra_path.chmod(0o755)

    print(f"[new] scaffold created at {destination}")
    print(f"[new] next: cd {destination} && ./hydra dev")
    return 0


def cmd_dev(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    mode = normalize_mode(args)
    build_dir, config_path = resolve_mode_paths(root, mode, args.build_dir, args.config)

    dev_script = root / "scripts" / "dev.sh"
    if not dev_script.exists():
        raise ValueError(f"missing script: {dev_script}")
    if not config_path.exists():
        raise ValueError(f"missing config file: {config_path}")

    env = os.environ.copy()
    env["HYDRA_BUILD_DIR"] = str(build_dir)
    env["HYDRA_CONFIG_PATH"] = str(config_path)
    run_command([str(dev_script)], cwd=root, env=env)
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
        suggested = "./hydra build --prod" if mode == "prod" else "./hydra build"
        raise ValueError(
            f"binary not found: {binary_path}. Run '{suggested}' first."
        )
    if not config_path.exists():
        raise ValueError(f"missing config file: {config_path}")

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
    new_cmd.set_defaults(func=cmd_new)

    dev_cmd = subparsers.add_parser("dev", help="Run Vite + Drogon watcher stack")
    add_mode_flags(dev_cmd)
    dev_cmd.add_argument(
        "--build-dir",
        help="CMake build directory (defaults: build for dev, build-prod for prod)",
    )
    dev_cmd.add_argument(
        "--config",
        help="Config file path (defaults: app/config.dev.json for dev, app/config.json for prod)",
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
        help="Config file path (defaults: app/config.dev.json for dev, app/config.json for prod)",
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
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
