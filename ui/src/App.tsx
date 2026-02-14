import React from "react";

type AppProps = {
  url: string;
  initialProps: Record<string, unknown>;
};

function asString(value: unknown, fallback: string): string {
  return typeof value === "string" ? value : fallback;
}

function asNumber(value: unknown): number | null {
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

export default function App({ url, initialProps }: AppProps): JSX.Element {
  const [count, setCount] = React.useState(0);
  const message = asString(initialProps.message, "HydraStack SSR active");
  const burnMs = asNumber(initialProps.__hydra_burn_ms);
  const isolateCounter = asNumber(initialProps.__hydra_isolate_counter);

  return (
    <main className="min-h-screen bg-gradient-to-b from-deep-900 to-deep-700 text-slate-100">
      <section className="mx-auto max-w-3xl px-6 py-16">
        <p className="text-sm uppercase tracking-[0.2em] text-accent-500">HydraStack</p>
        <h1 className="mt-4 text-4xl font-semibold">{message}</h1>
        <p className="mt-3 text-slate-300">Route: {url}</p>
        {burnMs !== null ? <p className="mt-2 text-xs text-slate-400">SSR burn: {burnMs}ms</p> : null}
        {isolateCounter !== null ? (
          <p className="mt-1 text-xs text-slate-400">Isolate counter: {isolateCounter}</p>
        ) : null}

        <button
          className="mt-8 rounded-lg border border-accent-500 px-5 py-2 font-medium text-accent-500 transition hover:bg-accent-500 hover:text-deep-900"
          onClick={() => setCount((value) => value + 1)}
          type="button"
        >
          Hydrated clicks: {count}
        </button>
      </section>
    </main>
  );
}
