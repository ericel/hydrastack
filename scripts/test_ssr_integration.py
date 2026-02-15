#!/usr/bin/env python3
"""HydraStack SSR integration smoke tests.

Runs a local hydra_demo process, executes request-context and routing assertions,
and validates the metrics endpoint payload.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict, Mapping, Tuple


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):  # type: ignore[override]
        return None


def fetch_text(
    url: str,
    headers: Dict[str, str] | None = None,
    *,
    follow_redirects: bool = True,
) -> Tuple[int, str, Mapping[str, str]]:
    request = urllib.request.Request(url, headers=headers or {})
    opener = urllib.request.build_opener() if follow_redirects else urllib.request.build_opener(_NoRedirectHandler())
    try:
        with opener.open(request, timeout=3) as response:
            body = response.read().decode("utf-8", errors="replace")
            return int(response.status), body, dict(response.headers.items())
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        return int(exc.code), body, dict(exc.headers.items())


def parse_props(html: str) -> Dict[str, Any]:
    match = re.search(
        r'<script[^>]*id="__HYDRA_PROPS__"[^>]*>(.*?)</script>',
        html,
        flags=re.DOTALL,
    )
    if not match:
        raise AssertionError("missing __HYDRA_PROPS__ payload in HTML response")
    return json.loads(match.group(1))


def assert_equal(actual: Any, expected: Any, label: str) -> None:
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")


def assert_contains(haystack: str, needle: str, label: str) -> None:
    if needle not in haystack:
        raise AssertionError(f"{label}: missing {needle!r}")


def wait_until_ready(base_url: str, timeout_s: float = 20.0) -> None:
    deadline = time.time() + timeout_s
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            status, body, _ = fetch_text(f"{base_url}/__hydra/test")
            if status == 200:
                payload = json.loads(body)
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

    plugin_config: Dict[str, Any] | None = None
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

    fd, raw_path = tempfile.mkstemp(prefix="hydra-ssr-test-", suffix=".json")
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


def run_tests(base_url: str) -> None:
    status, html, _ = fetch_text(f"{base_url}/")
    assert_equal(status, 200, "GET / status")
    props = parse_props(html)
    route = props.get("__hydra_route", {})
    req = props.get("__hydra_request", {})
    assert_equal(route.get("pageId"), "home", "home pageId")
    assert_equal(req.get("routeUrl"), "/", "home routeUrl")
    assert_contains(str(req.get("requestId", "")), "hydra-", "generated requestId")

    status, html, _ = fetch_text(f"{base_url}/posts/123?x=1")
    assert_equal(status, 200, "GET /posts/123 status")
    props = parse_props(html)
    route = props.get("__hydra_route", {})
    assert_equal(route.get("pageId"), "post_detail", "post pageId")
    params = route.get("params", {})
    assert_equal(params.get("postId"), "123", "postId route param")
    query = route.get("query", {})
    assert_equal(query.get("x"), "1", "query passthrough")

    status, html, _ = fetch_text(f"{base_url}/", headers={"Accept-Language": "fr"})
    assert_equal(status, 200, "GET / (fr) status")
    props = parse_props(html)
    req = props.get("__hydra_request", {})
    assert_equal(req.get("locale"), "fr", "locale from accept-language")

    status, html, _ = fetch_text(
        f"{base_url}/",
        headers={"Accept-Language": "fr", "Cookie": "hydra_lang=ko"},
    )
    assert_equal(status, 200, "GET / (cookie locale) status")
    props = parse_props(html)
    req = props.get("__hydra_request", {})
    assert_equal(req.get("locale"), "ko", "locale from cookie")

    status, html, _ = fetch_text(f"{base_url}/", headers={"X-Request-Id": "test-req-42"})
    assert_equal(status, 200, "GET / (request id) status")
    props = parse_props(html)
    req = props.get("__hydra_request", {})
    assert_equal(req.get("requestId"), "test-req-42", "request id propagation")

    status, _, response_headers = fetch_text(f"{base_url}/go-home", follow_redirects=False)
    assert_equal(status, 302, "GET /go-home redirect status")
    location_header = response_headers.get("Location") or response_headers.get("location") or ""
    assert_equal(location_header, "/", "redirect location")

    status, html, _ = fetch_text(f"{base_url}/not-found")
    assert_equal(status, 404, "GET /not-found status")
    assert_contains(html, "Page not found", "not-found html")

    # Unmatched document routes should use global Hydra error UI with matching HTTP status.
    status, html, _ = fetch_text(f"{base_url}/m")
    assert_equal(status, 404, "GET /m status")
    props = parse_props(html)
    route = props.get("__hydra_route", {})
    assert_equal(route.get("pageId"), "error_http", "unmatched route pageId")
    assert_equal(props.get("errorStatusCode"), 404, "unmatched route status payload")
    assert_contains(html, "Not Found", "unmatched route headline")

    status, metrics, _ = fetch_text(f"{base_url}/__hydra/metrics")
    assert_equal(status, 200, "GET /__hydra/metrics status")
    assert_contains(metrics, "hydra_render_latency_ms_bucket", "render histogram")
    assert_contains(metrics, "hydra_acquire_wait_ms_bucket", "acquire histogram")
    assert_contains(metrics, "hydra_pool_in_use", "pool gauge")
    assert_contains(metrics, "hydra_render_timeouts_total", "render timeout counter")
    assert_contains(metrics, "hydra_recycles_total", "recycle counter")
    assert_contains(metrics, "hydra_render_errors_total", "render error counter")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run HydraStack SSR integration tests")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--binary", type=Path, default=None, help="hydra_demo binary path")
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        help="source config template path (default: app/config.dev.json)",
    )
    parser.add_argument("--port", type=int, default=int(os.environ.get("HYDRA_TEST_PORT", "18070")))
    args = parser.parse_args()

    root = args.root.resolve()
    binary = (args.binary or (root / "build" / "hydra_demo")).resolve()
    config_template = (args.config or (root / "app" / "config.dev.json")).resolve()

    if not binary.exists():
        print(f"[ssr-test] missing binary: {binary}", file=sys.stderr)
        return 2
    if not config_template.exists():
        print(f"[ssr-test] missing config template: {config_template}", file=sys.stderr)
        return 2

    test_config = make_test_config(root, config_template, args.port)
    log_fd, raw_log_path = tempfile.mkstemp(prefix="hydra-ssr-test-", suffix=".log")
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
        run_tests(base_url)
        print("[ssr-test] PASS")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"[ssr-test] FAIL: {exc}", file=sys.stderr)
        if log_path.exists():
            print(f"[ssr-test] server log: {log_path}", file=sys.stderr)
            print("[ssr-test] last log lines:", file=sys.stderr)
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
