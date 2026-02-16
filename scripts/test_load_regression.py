#!/usr/bin/env python3
"""HydraStack micro load regression guard.

Starts hydra_demo on an isolated port, runs apachebench against `/`, and fails
if throughput/error thresholds regress beyond configured limits.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path
from typing import Any


def wait_until_ready(base_url: str, timeout_s: float = 20.0) -> None:
    deadline = time.time() + timeout_s
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(f"{base_url}/__hydra/test", timeout=2) as response:
                payload = json.loads(response.read().decode("utf-8", errors="replace"))
                if payload.get("ok") is True:
                    return
        except Exception as exc:  # noqa: BLE001
            last_error = exc
        time.sleep(0.2)
    raise RuntimeError(f"server did not become ready: {last_error}")


def make_test_config(root: Path, source_config: Path, port: int) -> Path:
    config = json.loads(source_config.read_text(encoding="utf-8"))
    listeners = config.get("listeners")
    if not isinstance(listeners, list) or not listeners:
        raise RuntimeError("config missing listeners[0]")
    if not isinstance(listeners[0], dict):
        raise RuntimeError("config listeners[0] must be an object")

    listeners[0]["address"] = "127.0.0.1"
    listeners[0]["port"] = port
    listeners[0]["https"] = False

    plugins = config.get("plugins")
    if not isinstance(plugins, list):
        raise RuntimeError("config missing plugins list")

    plugin_config: dict[str, Any] | None = None
    for plugin in plugins:
        if isinstance(plugin, dict) and plugin.get("name") == "hydra::HydraSsrPlugin":
            maybe_cfg = plugin.get("config")
            if isinstance(maybe_cfg, dict):
                plugin_config = maybe_cfg
                break

    if plugin_config is None:
        raise RuntimeError("config missing hydra::HydraSsrPlugin block")

    ssr_bundle_path = str(plugin_config.get("ssr_bundle_path", "./public/assets/ssr-bundle.js"))
    if not (root / ssr_bundle_path).exists():
        raise RuntimeError(
            f"missing SSR bundle at {ssr_bundle_path}. Run `cd ui && npm run build:ssr` first."
        )

    plugin_config["asset_mode"] = "prod"
    plugin_config["log_render_metrics"] = False
    plugin_config["log_request_routes"] = False
    dev_mode = plugin_config.get("dev_mode")
    if isinstance(dev_mode, dict):
        dev_mode["enabled"] = False

    fd, raw_path = tempfile.mkstemp(prefix="hydra-load-test-", suffix=".json")
    os.close(fd)
    output_path = Path(raw_path)
    output_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
    return output_path


def terminate_process(process: subprocess.Popen[str], timeout_s: float = 5.0) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=timeout_s)


def parse_ab_metrics(output: str) -> tuple[int, float, int]:
    failed_match = re.search(r"Failed requests:\s+(\d+)", output)
    rps_match = re.search(r"Requests per second:\s+([0-9]+(?:\.[0-9]+)?)", output)
    p95_match = re.search(r"^\s*95%\s+(\d+)", output, flags=re.MULTILINE)

    if failed_match is None or rps_match is None:
        raise RuntimeError("unable to parse apachebench output")

    failed_requests = int(failed_match.group(1))
    requests_per_second = float(rps_match.group(1))
    p95_ms = int(p95_match.group(1)) if p95_match else 0
    return failed_requests, requests_per_second, p95_ms


def run_load_test(ab_binary: str, base_url: str, requests: int, concurrency: int) -> tuple[int, float, int, str]:
    cmd = [ab_binary, "-k", "-l", "-n", str(requests), "-c", str(concurrency), f"{base_url}/"]
    completed = subprocess.run(  # noqa: S603
        cmd,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"apachebench failed ({completed.returncode}): {completed.stderr.strip()}")

    output = completed.stdout
    failed, rps, p95_ms = parse_ab_metrics(output)
    return failed, rps, p95_ms, output


def main() -> int:
    parser = argparse.ArgumentParser(description="Run HydraStack micro load regression test")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--binary", type=Path, default=None, help="hydra_demo binary path")
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        help="source config template path (default: demo/config.dev.json)",
    )
    parser.add_argument("--port", type=int, default=int(os.environ.get("HYDRA_LOAD_TEST_PORT", "18071")))
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--concurrency", type=int, default=100)
    parser.add_argument("--min-rps", type=float, default=300.0)
    parser.add_argument("--max-failed", type=int, default=0)
    parser.add_argument("--max-p95-ms", type=int, default=250)
    parser.add_argument("--ab", type=str, default=None, help="apachebench binary path")
    args = parser.parse_args()

    root = args.root.resolve()
    binary = (args.binary or (root / "build" / "hydra_demo")).resolve()
    default_config = root / "demo" / "config.dev.json"
    if not default_config.exists():
        default_config = root / "app" / "config.dev.json"
    config_template = (args.config or default_config).resolve()

    ab_binary = args.ab or shutil.which("ab")
    if not ab_binary:
        print("[load-test] SKIP: apachebench (ab) not found", file=sys.stderr)
        return 0

    if not binary.exists():
        print(f"[load-test] missing binary: {binary}", file=sys.stderr)
        return 2
    if not config_template.exists():
        print(f"[load-test] missing config template: {config_template}", file=sys.stderr)
        return 2

    test_config = make_test_config(root, config_template, args.port)
    log_fd, raw_log_path = tempfile.mkstemp(prefix="hydra-load-test-", suffix=".log")
    os.close(log_fd)
    log_path = Path(raw_log_path)

    process: subprocess.Popen[str] | None = None
    try:
        with log_path.open("w", encoding="utf-8") as log_file:
            process = subprocess.Popen(  # noqa: S603
                [str(binary), str(test_config)],
                cwd=str(root),
                stdout=log_file,
                stderr=subprocess.STDOUT,
                text=True,
            )

        base_url = f"http://127.0.0.1:{args.port}"
        wait_until_ready(base_url)
        failed, rps, p95_ms, ab_output = run_load_test(
            ab_binary,
            base_url,
            args.requests,
            args.concurrency,
        )

        failures: list[str] = []
        if failed > args.max_failed:
            failures.append(
                f"failed requests {failed} exceeded max_failed {args.max_failed}"
            )
        if rps < args.min_rps:
            failures.append(f"requests/sec {rps:.2f} below min_rps {args.min_rps:.2f}")
        if args.max_p95_ms > 0 and p95_ms > args.max_p95_ms:
            failures.append(f"p95 {p95_ms}ms exceeded max_p95_ms {args.max_p95_ms}ms")

        print(
            "[load-test] stats "
            f"requests={args.requests} concurrency={args.concurrency} "
            f"failed={failed} rps={rps:.2f} p95_ms={p95_ms}"
        )

        if failures:
            print("[load-test] FAIL:", file=sys.stderr)
            for failure in failures:
                print(f"  - {failure}", file=sys.stderr)
            print("[load-test] apachebench output:", file=sys.stderr)
            print(ab_output, file=sys.stderr)
            return 1

        print("[load-test] PASS")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"[load-test] FAIL: {exc}", file=sys.stderr)
        if log_path.exists():
            print(f"[load-test] server log: {log_path}", file=sys.stderr)
            lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
            for line in lines[-40:]:
                print(line, file=sys.stderr)
        return 1
    finally:
        if process is not None:
            terminate_process(process)
        if test_config.exists():
            test_config.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
