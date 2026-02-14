import { renderToString } from "react-dom/server";

import App from "./App";

declare global {
  interface Window {
    render?: (url: string, propsJson: string) => string;
  }

  interface GlobalThis {
    render?: (url: string, propsJson: string) => string;
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
  inputProps: Record<string, unknown>
): Record<string, unknown> {
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

  return props;
}

// SSR contract: globalThis.render(url, propsJson) -> app HTML fragment.
globalThis.render = (url: string, propsJson: string): string => {
  const parsedProps = parseProps(propsJson);
  const hydratedProps = applySsrTestHooks(parsedProps);
  const appHtml = renderToString(<App url={url || "/"} initialProps={hydratedProps} />);
  return appHtml;
};
