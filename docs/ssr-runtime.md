# SSR runtime globals

HydraStack renders pages in an embedded V8 isolate. Plain V8 has no browser
APIs, so the engine adds the ones below before the SSR bundle runs
(`engine/src/V8RuntimeGlobals.cc`).

The rule: **a global either behaves as it does in a browser, or it is absent.**
A substitute that quietly behaves differently is worse than a missing global.
Feature checks pass, the server renders different HTML from the browser, and
React reports a hydration mismatch (#418/#423) far from the cause.

## Provided

| Global | Behaviour |
| --- | --- |
| `URL`, `URLSearchParams` | WHATWG URL Standard, parsed by [ada](../engine/third_party/ada/README.md), the parser Node.js uses. Includes IDNA hostnames, IPv4/IPv6, setters, live `searchParams`, `URL.canParse` and `URL.parse`. `createObjectURL` is absent. |
| `TextEncoder`, `TextDecoder` | WHATWG Encoding, UTF-8 only. Invalid bytes decode to U+FFFD, and `fatal`, `ignoreBOM` and `{ stream: true }` are supported. Any other label throws `RangeError`. |
| `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval` | Accepted and **never fire**. A render is synchronous, so in a browser no timer would have run before the HTML was produced. |
| `queueMicrotask` | Runs after the current render call returns. It cannot affect the HTML. |
| `fetch`, `Headers`, `Response` | Browser-shaped. `fetch("/api/...", init)` goes through the synchronous HydraStack bridge and resolves with a `Response`. Other origins reject with `TypeError`. `Response.body` is `null` (no streams), so use `text()`, `json()` or `arrayBuffer()`. The response resolves only after render returns, so it cannot change the HTML. Use `hydra.fetch` for data that the render needs. |
| `hydra.fetch(request)` | Synchronous bridge: `{ method, path, query, headers, body }` returns `{ status, body, headers }`. |
| `globalThis.global`, `self`, `process.env.NODE_ENV` | Aliases for bundled libraries. |
| `Intl` | Built into V8, with ICU. Cache formatters at module level: each `new Intl.*Format` holds native memory that V8's heap limit does not cover. |

## Absent

The following are absent: `window` and `document`, `atob`/`btoa`, `structuredClone`, `AbortController`, `crypto`, `requestAnimationFrame`, `Blob`, streams, and `localStorage`. Code that needs them must run in effects, or behind a check whose server branch renders the same HTML as the client's first render.

## Hydration diagnostics

The client entry passes `createHydrationErrorReporter(route)`
(`ui/src/hydrationDiagnostics.ts`) as `hydrateRoot`'s `onRecoverableError`.
Each report records the route, message and component stack on
`window.__HYDRA_HYDRATION_ERRORS__` (the last 20 reports). In development it
also logs a readable message. The error is then passed to the browser's
normal error reporting.

## Tests

`hydra_ssr_runtime_globals` (ctest) runs `engine/test/ssr_runtime_globals.js`
inside the runtime. Run the same file under Node
(`node engine/test/ssr_runtime_globals.js`) to confirm that the expected values
match a browser-grade implementation.
