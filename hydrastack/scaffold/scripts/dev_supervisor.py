#!/usr/bin/env python3
"""Serialize local builds and supervise development process groups (POSIX)."""

import argparse
import fcntl
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


STOP = False


def request_stop(signum, frame):
    global STOP
    STOP = True


def log(message):
    print(f"[HydraDev] {message}", flush=True)


def signal_group(pgid, signum):
    try:
        os.killpg(pgid, signum)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        # macOS can return EPERM while a terminated group is being reaped.
        # Verify that it has no live members before treating it as gone.
        members = subprocess.run(["ps", "-axo", "pgid=,stat="],
                                 capture_output=True, text=True, check=True)
        for row in members.stdout.splitlines():
            group, state = row.split(maxsplit=1)
            if int(group) == pgid and not state.startswith("Z"):
                raise
        return False


def stop_process(process):
    """Reap the leader and terminate descendants even if the leader has exited."""
    if process is None:
        return
    if signal_group(process.pid, signal.SIGTERM):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            process.poll()
            if not signal_group(process.pid, 0):
                break
            time.sleep(0.05)
        else:
            signal_group(process.pid, signal.SIGKILL)
    process.wait()


def acquire_lock(path, wait=True):
    # Never unlink lock files: a waiter may already have this inode open.
    lock = open(path, "a+")
    announced = False
    while not STOP:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return lock
        except BlockingIOError:
            if not wait:
                lock.close()
                raise RuntimeError(f"Another development watcher owns {path}")
            if not announced:
                log("Waiting for the current configure/build to finish...")
                announced = True
            time.sleep(0.1)
    lock.close()
    return None


def locked_command(path, command):
    lock = acquire_lock(path)
    if lock is None:
        return 130
    # Exec keeps the command in the caller's process group. Inherit the lock so
    # an unexpected wrapper death cannot unlock a still-running compiler.
    os.set_inheritable(lock.fileno(), True)
    os.execvp(command[0], command)


def snapshot(paths):
    result = {}
    for root in paths:
        root = Path(root)
        candidates = root.rglob("*") if root.is_dir() else (root,)
        for path in candidates:
            try:
                if path.is_file():
                    stat = path.stat()
                    result[str(path)] = (stat.st_mtime_ns, stat.st_size)
            except FileNotFoundError:
                # Editors and bundle writers may replace files during a scan.
                continue
    return result


def watch(args):
    lock = acquire_lock(Path(args.build_dir) / ".hydra-watch.lock", wait=False)
    if lock is None:
        return 130
    builder = None
    server = None
    built_sources = None
    sources = snapshot(args.source)
    runtime = snapshot(args.runtime)
    changed_at = time.monotonic()
    needs_build = True
    needs_restart = False
    build_succeeded = False
    env = {**os.environ, "HYDRA_PROJECT_ROOT": args.root,
           "HYDRA_BUILD_DIR": args.build_dir, "HYDRA_CONFIG_PATH": args.config,
           "HYDRA_DEV_SUPERVISOR": str(Path(__file__).resolve())}
    try:
        while not STOP:
            next_sources = snapshot(args.source)
            next_runtime = snapshot(args.runtime)
            if next_sources != sources:
                sources = next_sources
                needs_build = True
                changed_at = time.monotonic()
            if next_runtime != runtime:
                runtime = next_runtime
                needs_restart = True
                changed_at = time.monotonic()

            if builder is not None and builder.poll() is not None:
                succeeded = builder.returncode == 0
                build_succeeded = succeeded
                stop_process(builder)
                builder = None
                # Source edits during compilation queue one more build. Asset
                # edits never interrupt the compiler or request a C++ build.
                needs_build = sources != built_sources
                needs_restart = succeeded and not needs_build
                if not succeeded:
                    log("Build failed; waiting for a source change before retrying.")

            if builder is None and time.monotonic() - changed_at >= 0.3:
                if needs_build:
                    stop_process(server)
                    server = None
                    built_sources = sources.copy()
                    needs_build = False
                    needs_restart = False
                    log("Building C++ sources...")
                    builder = subprocess.Popen(
                        ["bash", args.run_script, "--build-only"], env=env,
                        start_new_session=True)
                elif needs_restart and build_succeeded:
                    stop_process(server)
                    server = None
                    if not STOP:
                        log("Starting app (no C++ rebuild for asset/config changes)...")
                        server = subprocess.Popen(
                            [str(Path(args.build_dir) / args.target)],
                            env={**env, "HYDRA_CONFIG": args.config},
                            start_new_session=True)
                    needs_restart = False

            if server is not None and server.poll() is not None:
                log(f"App exited ({server.returncode}); waiting for a change.")
                stop_process(server)
                server = None
            time.sleep(0.5)
    finally:
        stop_process(builder)
        stop_process(server)
        lock.close()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    locked = modes.add_parser("lock")
    locked.add_argument("path")
    locked.add_argument("command", nargs=argparse.REMAINDER)
    watcher = modes.add_parser("watch")
    for name in ("root", "build-dir", "config", "run-script"):
        watcher.add_argument(f"--{name}", required=True)
    watcher.add_argument("--target", default="hydra_app")
    watcher.add_argument("--source", action="append", default=[])
    watcher.add_argument("--runtime", action="append", default=[])
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    if args.mode == "lock":
        if not args.command:
            parser.error("lock requires a command")
        return locked_command(args.path, args.command)
    return watch(args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        log(str(error))
        sys.exit(1)
