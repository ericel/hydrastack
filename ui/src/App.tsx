import React from "react";

import { createGettext } from "./i18n/gettext";

type AppProps = {
  url: string;
  initialProps: Record<string, unknown>;
};

const SUPPORTED_THEMES = ["ocean", "sunset", "forest"] as const;
type ThemeName = (typeof SUPPORTED_THEMES)[number];

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

function normalizeTheme(value: string): ThemeName {
  const normalized = value.toLowerCase();
  if (SUPPORTED_THEMES.includes(normalized as ThemeName)) {
    return normalized as ThemeName;
  }
  return "ocean";
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
  const errorStatusCode = asNumber(initialProps.errorStatusCode ?? initialProps.error_status_code);
  const errorReason = asString(initialProps.errorReason, asString(initialProps.error_reason, ""));
  const routeQuery = asStringRecord(routeContract.query);
  const postId = asString(routeParams.postId, asString(initialProps.postId, ""));
  const querySummary = Object.entries(routeQuery)
    .map(([key, value]) => `${key}=${value}`)
    .join(", ");
  const isPostDetailPage = pageId === "post_detail";
  const requestLocale = asString(requestContext.locale, "en").toLowerCase();
  const requestTheme = normalizeTheme(asString(requestContext.theme, "ocean"));
  const [activeTheme, setActiveTheme] = React.useState<ThemeName>(requestTheme);
  const i18n = React.useMemo(() => createGettext(requestLocale), [requestLocale]);
  const gettext = i18n.gettext;
  const _ = i18n._;
  const locale = i18n.locale;
  const debugLocaleCandidates = asStringArray(requestContext.localeCandidates);
  const localeCandidates =
    debugLocaleCandidates.length > 0 ? debugLocaleCandidates : i18n.localeCandidates;
  const messageKey = asString(initialProps.messageKey, asString(initialProps.message_key, ""));
  const messageFallback = asString(initialProps.message, "");
  const message =
    pageId === "error_http"
      ? (errorReason || (errorStatusCode !== null ? `HTTP ${errorStatusCode}` : "Request failed"))
      : pageId === "not_found"
      ? "Page not found"
      : messageKey
      ? gettext(messageKey)
      : messageFallback || _("hello_from_hydrastack");
  const requestUrl = asString(requestContext.url, "");
  const bridgeStatus = asNumber(initialProps.__hydra_bridge_status);
  const bridgeBody = asString(initialProps.__hydra_bridge_body, "");
  const toggleTheme = React.useCallback(() => {
    setActiveTheme((current) => {
      const index = SUPPORTED_THEMES.indexOf(current);
      const next = SUPPORTED_THEMES[(index + 1) % SUPPORTED_THEMES.length];
      if (typeof document !== "undefined") {
        document.cookie = `hydra_theme=${next}; path=/; max-age=31536000`;
      }
      return next;
    });
  }, []);

  return (
    <main data-theme={activeTheme} className="min-h-screen hydra-theme-bg">
      <section className="mx-auto max-w-3xl px-6 py-16">
        <p className="text-sm uppercase tracking-[0.2em] hydra-text-accent">HydraStack</p>
        <h1 className="mt-4 text-4xl font-semibold">{message}</h1>
        <p className="mt-3 hydra-text-muted">
          {gettext("route")}: {url}
        </p>
        <p className="mt-2 text-xs hydra-text-muted">{gettext("page_id")}: {pageId}</p>
        {errorStatusCode !== null ? (
          <p className="mt-1 text-xs hydra-text-muted">
            HTTP status: {errorStatusCode}
            {errorReason ? ` (${errorReason})` : ""}
          </p>
        ) : null}
        {postId ? <p className="mt-1 text-xs hydra-text-muted">{gettext("post_id")}: {postId}</p> : null}
        {querySummary ? (
          <p className="mt-1 text-xs hydra-text-muted">{gettext("query_params")}: {querySummary}</p>
        ) : null}
        <p className="mt-2 text-xs hydra-text-muted">
          {_("locale")}: {locale}
        </p>
        <p className="mt-1 text-xs hydra-text-muted">
          {gettext("theme")}: {activeTheme}
        </p>
        <button
          className="mt-3 rounded-lg px-4 py-1 text-xs font-medium transition hydra-theme-button"
          onClick={toggleTheme}
          type="button"
        >
          {gettext("toggle_theme")}
        </button>
        <div className="mt-4 flex flex-wrap gap-2">
          {isPostDetailPage ? (
            <a
              className="rounded-lg px-4 py-1 text-xs font-medium transition hydra-theme-button"
              href="/"
            >
              Back to home
            </a>
          ) : (
            <a
              className="rounded-lg px-4 py-1 text-xs font-medium transition hydra-theme-button"
              href="/posts/123"
            >
              Open post 123
            </a>
          )}
          <a
            className="rounded-lg px-4 py-1 text-xs font-medium transition hydra-theme-button"
            href="/go-home"
          >
            Test redirect
          </a>
          <a
            className="rounded-lg px-4 py-1 text-xs font-medium transition hydra-theme-button"
            href="/not-found"
          >
            Test 404
          </a>
        </div>
        {localeCandidates.length > 0 ? (
          <p className="mt-1 text-xs hydra-text-muted">
            {gettext("locale_candidates")}: {localeCandidates.join(", ")}
          </p>
        ) : null}
        {requestUrl ? (
          <p className="mt-2 text-xs hydra-text-muted">
            {gettext("full_url")}: {requestUrl}
          </p>
        ) : null}
        {burnMs !== null ? (
          <p className="mt-2 text-xs hydra-text-muted">
            {gettext("ssr_burn")}: {burnMs}ms
          </p>
        ) : null}
        {isolateCounter !== null ? (
          <p className="mt-1 text-xs hydra-text-muted">
            {gettext("isolate_counter")}: {isolateCounter}
          </p>
        ) : null}
        {bridgeStatus !== null ? (
          <p className="mt-1 text-xs hydra-text-muted">
            {gettext("bridge_status")}: {bridgeStatus} {bridgeBody ? `(${bridgeBody})` : ""}
          </p>
        ) : null}

        <button
          className="mt-8 rounded-lg px-5 py-2 font-medium transition hydra-theme-button"
          onClick={() => setCount((value) => value + 1)}
          type="button"
        >
          {gettext("hydrated_clicks")}: {count}
        </button>
      </section>
    </main>
  );
}
