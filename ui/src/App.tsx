import React from "react";

import { createGettext } from "./i18n/gettext";

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

function asStringArray(value: unknown): string[] {
  if (!Array.isArray(value)) {
    return [];
  }

  return value.filter((item): item is string => typeof item === "string");
}

function asStringRecord(value: unknown): Record<string, string> {
  if (typeof value !== "object" || value === null) {
    return {};
  }

  const out: Record<string, string> = {};
  for (const [key, raw] of Object.entries(value as Record<string, unknown>)) {
    if (typeof raw === "string") {
      out[key] = raw;
    } else if (typeof raw === "number" || typeof raw === "boolean") {
      out[key] = String(raw);
    }
  }
  return out;
}

export default function App({ url, initialProps }: AppProps): JSX.Element {
  const [count, setCount] = React.useState(0);
  const burnMs = asNumber(initialProps.__hydra_burn_ms);
  const isolateCounter = asNumber(initialProps.__hydra_isolate_counter);
  const requestContext =
    typeof initialProps.__hydra_request === "object" && initialProps.__hydra_request !== null
      ? (initialProps.__hydra_request as Record<string, unknown>)
      : {};
  const routeContract =
    typeof initialProps.__hydra_route === "object" && initialProps.__hydra_route !== null
      ? (initialProps.__hydra_route as Record<string, unknown>)
      : {};
  const pageId = asString(routeContract.pageId, asString(initialProps.page, "home"));
  const routeParams = asStringRecord(routeContract.params);
  const routeQuery = asStringRecord(routeContract.query);
  const postId = asString(routeParams.postId, asString(initialProps.postId, ""));
  const querySummary = Object.entries(routeQuery)
    .map(([key, value]) => `${key}=${value}`)
    .join(", ");
  const requestLocale = asString(requestContext.locale, "en").toLowerCase();
  const i18n = React.useMemo(() => createGettext(requestLocale), [requestLocale]);
  const gettext = i18n.gettext;
  const _ = i18n._;
  const locale = i18n.locale;
  const debugLocaleCandidates = asStringArray(requestContext.localeCandidates);
  const localeCandidates =
    debugLocaleCandidates.length > 0 ? debugLocaleCandidates : i18n.localeCandidates;
  const messageKey = asString(initialProps.messageKey, asString(initialProps.message_key, ""));
  const messageFallback = asString(initialProps.message, "");
  const message = messageKey ? gettext(messageKey) : messageFallback || _("hello_from_hydrastack");
  const requestUrl = asString(requestContext.url, "");
  const bridgeStatus = asNumber(initialProps.__hydra_bridge_status);
  const bridgeBody = asString(initialProps.__hydra_bridge_body, "");

  return (
    <main className="min-h-screen bg-gradient-to-b from-deep-900 to-deep-700 text-slate-100">
      <section className="mx-auto max-w-3xl px-6 py-16">
        <p className="text-sm uppercase tracking-[0.2em] text-accent-500">HydraStack</p>
        <h1 className="mt-4 text-4xl font-semibold">{message}</h1>
        <p className="mt-3 text-slate-300">
          {gettext("route")}: {url}
        </p>
        <p className="mt-2 text-xs text-slate-400">{gettext("page_id")}: {pageId}</p>
        {postId ? <p className="mt-1 text-xs text-slate-400">{gettext("post_id")}: {postId}</p> : null}
        {querySummary ? (
          <p className="mt-1 text-xs text-slate-400">{gettext("query_params")}: {querySummary}</p>
        ) : null}
        <p className="mt-2 text-xs text-slate-400">
          {_("locale")}: {locale}
        </p>
        {localeCandidates.length > 0 ? (
          <p className="mt-1 text-xs text-slate-400">
            {gettext("locale_candidates")}: {localeCandidates.join(", ")}
          </p>
        ) : null}
        {requestUrl ? (
          <p className="mt-2 text-xs text-slate-400">
            {gettext("full_url")}: {requestUrl}
          </p>
        ) : null}
        {burnMs !== null ? (
          <p className="mt-2 text-xs text-slate-400">
            {gettext("ssr_burn")}: {burnMs}ms
          </p>
        ) : null}
        {isolateCounter !== null ? (
          <p className="mt-1 text-xs text-slate-400">
            {gettext("isolate_counter")}: {isolateCounter}
          </p>
        ) : null}
        {bridgeStatus !== null ? (
          <p className="mt-1 text-xs text-slate-400">
            {gettext("bridge_status")}: {bridgeStatus} {bridgeBody ? `(${bridgeBody})` : ""}
          </p>
        ) : null}

        <button
          className="mt-8 rounded-lg border border-accent-500 px-5 py-2 font-medium text-accent-500 transition hover:bg-accent-500 hover:text-deep-900"
          onClick={() => setCount((value) => value + 1)}
          type="button"
        >
          {gettext("hydrated_clicks")}: {count}
        </button>
      </section>
    </main>
  );
}
