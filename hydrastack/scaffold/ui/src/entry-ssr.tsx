import { renderToString } from "react-dom/server";

import App from "./App";
import { attachRouteContract, ensureString, resolveHydraRoute, type JsonObject } from "./routeContract";

declare global {
  interface Window {
    render?: (url: string, propsJson: string, requestContextJson?: string) => string;
  }

  interface HydraBridgeResponse {
    status?: number;
    body?: string;
    headers?: Record<string, string>;
  }

  interface HydraGlobalApi {
    fetch?: (request: Record<string, unknown> | string) => HydraBridgeResponse;
  }

  interface GlobalThis {
    render?: (url: string, propsJson: string, requestContextJson?: string) => string;
    hydra?: HydraGlobalApi;
  }
}

function parseProps(propsJson: string): Record<string, unknown> {
  if (!propsJson) {
    return {};
  }

  try {
    return JSON.parse(propsJson) as Record<string, unknown>;
  } catch {
    return {};
  }
}

function parseRequestContext(requestContextJson: string | undefined): Record<string, unknown> {
  if (!requestContextJson) {
    return {};
  }

  try {
    return JSON.parse(requestContextJson) as Record<string, unknown>;
  } catch {
    return {};
  }
}

function asFiniteNumber(value: unknown): number | null {
  if (typeof value === "number" && Number.isFinite(value)) {
    return value;
  }

  if (typeof value === "string" && value.trim() !== "") {
    const parsed = Number(value);
    if (Number.isFinite(parsed)) {
      return parsed;
    }
  }

  return null;
}

function asBoolean(value: unknown): boolean {
  if (typeof value === "boolean") {
    return value;
  }
  if (typeof value === "number") {
    return value !== 0;
  }
  if (typeof value === "string") {
    return value === "1" || value.toLowerCase() === "true";
  }
  return false;
}

function burnCpu(ms: number): void {
  // Deterministic CPU pressure for SSR contention tests.
  const iterations = Math.max(0, Math.floor(ms)) * 200_000;
  let sink = 0;
  for (let i = 0; i < iterations; i += 1) {
    sink = (sink + (i & 7)) & 0xffff;
  }
  (globalThis as Record<string, unknown>).__HYDRA_BURN_SINK__ = sink;
}

function nextIsolateCounter(): number {
  const key = "__HYDRA_INTERNAL_ISOLATE_COUNTER__";
  const current = asFiniteNumber((globalThis as Record<string, unknown>)[key]) ?? 0;
  const next = current + 1;
  (globalThis as Record<string, unknown>)[key] = next;
  return next;
}

function applySsrTestHooks(
  inputProps: JsonObject
): JsonObject {
  const props = { ...inputProps };
  const testConfig =
    typeof props.__hydra_test === "object" && props.__hydra_test !== null
      ? (props.__hydra_test as Record<string, unknown>)
      : {};

  const burnMsRaw = testConfig.burnMs ?? testConfig.burn_ms;
  const burnMs = asFiniteNumber(burnMsRaw);
  if (burnMs !== null && burnMs > 0) {
    burnCpu(Math.floor(burnMs));
    props.__hydra_burn_ms = Math.floor(burnMs);
  }

  const exposeCounter = asBoolean(
    testConfig.counter ?? testConfig.exposeCounter ?? testConfig.expose_counter
  );
  if (exposeCounter) {
    props.__hydra_isolate_counter = nextIsolateCounter();
  }

  const bridgePathRaw =
    testConfig.bridgePath ?? testConfig.bridge_path ?? testConfig.apiBridgePath;
  const bridgePath = typeof bridgePathRaw === "string" ? bridgePathRaw : "";
  if (bridgePath && globalThis.hydra?.fetch) {
    const bridgeResult = globalThis.hydra.fetch({
      method: "GET",
      path: bridgePath
    });
    props.__hydra_bridge_status =
      typeof bridgeResult.status === "number" ? bridgeResult.status : null;
    props.__hydra_bridge_body =
      typeof bridgeResult.body === "string" ? bridgeResult.body : "";
  }

  return props;
}

// SSR contract: globalThis.render(url, propsJson, requestContextJson) -> app HTML fragment.
globalThis.render = (
  url: string,
  propsJson: string,
  requestContextJson?: string
): string => {
  const parsedProps = parseProps(propsJson);
  const requestContext = parseRequestContext(requestContextJson);
  const propsWithContext: JsonObject = {
    ...parsedProps,
    __hydra_request: requestContext
  };
  const route = resolveHydraRoute(url || "/", propsWithContext, requestContext);
  let pageProps = attachRouteContract(propsWithContext, route);

  // Router contract: choose page by pageId from C++ (__hydra_route.pageId).
  switch (route.pageId) {
    case "post_detail": {
      const postId = route.params.postId ?? ensureString(pageProps.postId, "");
      const postDetailMessageKey = ensureString(
        pageProps.messageKey,
        ensureString(pageProps.message_key, "post_detail_title")
      );
      pageProps = {
        ...pageProps,
        page: "post_detail",
        postId,
        messageKey: postDetailMessageKey
      };
      break;
    }
    case "home":
    default:
      const homeMessageKey = ensureString(
        pageProps.messageKey,
        ensureString(pageProps.message_key, "hello_from_hydrastack")
      );
      pageProps = {
        ...pageProps,
        page: "home",
        messageKey: homeMessageKey
      };
      break;
  }

  const hydratedProps = applySsrTestHooks(pageProps);
  const appHtml = renderToString(<App url={route.routeUrl} initialProps={hydratedProps} />);
  return appHtml;
};
