// SSR runtime web-platform conformance checks.
//
// Runs in two places:
//   - inside the HydraStack V8 runtime, driven by SsrRuntimeGlobalsTest.cc
//     (render("/sync") runs the synchronous checks and starts the async ones;
//     render("/async") returns the async results after microtasks have run);
//   - directly under Node (`node engine/test/ssr_runtime_globals.js`), which
//     checks that every expected value below is what a real browser-grade
//     implementation produces. Runtime-specific checks run only in HydraStack.
"use strict";

const isHydra = typeof globalThis.hydra === "object" && typeof globalThis.hydra.fetch === "function";
const failures = [];
let passed = 0;

const show = (value) => {
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
};
function eq(name, actual, expected) {
  if (show(actual) === show(expected)) {
    passed++;
  } else {
    failures.push(`${name}: expected ${show(expected)}, got ${show(actual)}`);
  }
}
function throws(name, fn, type) {
  try {
    fn();
    failures.push(`${name}: expected ${type.name}, nothing thrown`);
  } catch (error) {
    if (error instanceof type) passed++;
    else failures.push(`${name}: expected ${type.name}, got ${error && error.name}: ${error && error.message}`);
  }
}
const bytes = (u8) => Array.from(u8);

function syncChecks() {
  // ---- URL parsing -------------------------------------------------------
  eq("normalises case, default port, dot segments and encodes",
     new URL("HTTPS://User:Pa ss@EXAMPLE.com:443/a/../b/./c?q=1 2#h \u0101").href,
     "https://User:Pa%20ss@example.com/b/c?q=1%202#h%20%C4%81");
  const parts = new URL("https://u:p@sub.example.com:8443/p/a?x=1&y=2#frag");
  eq("components", [parts.protocol, parts.username, parts.password, parts.host, parts.hostname,
                    parts.port, parts.pathname, parts.search, parts.hash, parts.origin],
     ["https:", "u", "p", "sub.example.com:8443", "sub.example.com", "8443", "/p/a", "?x=1&y=2", "#frag",
      "https://sub.example.com:8443"]);
  eq("IDNA hostname", new URL("http://m\u00fcnchen.de/").hostname, "xn--mnchen-3ya.de");
  eq("relative against base", new URL("../x?y#z", "http://a.com/b/c/d").href, "http://a.com/b/x?y#z");
  eq("base as URL object", new URL("/p", new URL("https://h.test/a/b")).href, "https://h.test/p");
  eq("IPv4 numeric forms", new URL("http://0x7f.1/").host, "127.0.0.1");
  eq("IPv6 with port", new URL("http://[0:0::1]:8080/").host, "[::1]:8080");
  eq("file URL", new URL("file:///C:/x/../y").href, "file:///C:/y");
  eq("opaque path and null origin", [new URL("mailto:a@b.c").pathname, new URL("mailto:a@b.c").origin], ["a@b.c", "null"]);
  eq("lone surrogate becomes U+FFFD", new URL("http://a/\uD800").pathname, "/%EF%BF%BD");
  eq("leading/trailing C0 and space trimmed", new URL("  http://a/ \t").href, "http://a/");
  eq("backslash in special URL", new URL("http:\\\\a\\b").href, "http://a/b");
  throws("invalid URL throws TypeError", () => new URL("not a url"), TypeError);
  throws("relative without base throws TypeError", () => new URL("/path"), TypeError);
  throws("invalid base throws TypeError", () => new URL("/p", "nope"), TypeError);
  throws("no arguments throws TypeError", () => new URL(), TypeError);
  eq("canParse", [URL.canParse("/p", "http://a"), URL.canParse("/p"), URL.canParse("http://a")], [true, false, true]);
  eq("parse returns null on failure", URL.parse("bad"), null);
  eq("parse returns a URL", URL.parse("p", "http://a/b/") instanceof URL, true);
  eq("toString / toJSON / JSON.stringify",
     [String(new URL("http://a")), new URL("http://a").toJSON(), JSON.stringify({ u: new URL("http://a") })],
     ["http://a/", "http://a/", "{\"u\":\"http://a/\"}"]);
  eq("toStringTag", Object.prototype.toString.call(new URL("http://a")), "[object URL]");

  // ---- URL setters -------------------------------------------------------
  const u = new URL("https://a.com/p?x=1#f");
  u.port = "8080";
  eq("port setter", u.host, "a.com:8080");
  u.port = "443";
  eq("default port setter clears port", u.host, "a.com");
  u.port = "abc";
  eq("invalid port is ignored", u.port, "");
  u.protocol = "ftp";
  eq("special-to-special protocol", u.href, "ftp://a.com/p?x=1#f");
  u.protocol = "foo";
  eq("special-to-non-special protocol is ignored", u.protocol, "ftp:");
  u.hash = "";
  u.search = "?a=b c";
  u.pathname = "/\u00e4";
  eq("search/hash/pathname setters", u.href, "ftp://a.com/%C3%A4?a=b%20c");
  u.hostname = "";
  eq("empty hostname on special URL is ignored", u.hostname, "a.com");
  u.username = "me";
  u.password = "pw";
  eq("credential setters", u.href, "ftp://me:pw@a.com/%C3%A4?a=b%20c");
  throws("invalid href setter throws TypeError", () => { u.href = "bad"; }, TypeError);
  eq("URL unchanged after failed href", u.href, "ftp://me:pw@a.com/%C3%A4?a=b%20c");
  u.href = "http://b.test/z";
  eq("href setter", u.host, "b.test");

  // ---- searchParams linkage ---------------------------------------------
  const linked = new URL("http://a/?b=1&a=2");
  const params = linked.searchParams;
  eq("searchParams is the same object", linked.searchParams === params, true);
  params.append("c", "x y");
  eq("append updates URL", linked.href, "http://a/?b=1&a=2&c=x+y");
  params.sort();
  eq("sort updates URL", linked.search, "?a=2&b=1&c=x+y");
  linked.search = "?z=9";
  eq("search setter updates params", [params.get("z"), params.has("b")], ["9", false]);
  linked.href = "http://a/?k=v";
  eq("href setter updates params", params.get("k"), "v");
  params.delete("k");
  eq("emptying params removes the query", [linked.search, linked.href], ["", "http://a/"]);

  // ---- URLSearchParams ---------------------------------------------------
  const sp = new URLSearchParams("?a=1&b=%F0%9F%98%80&c=+&d&=e&a=3");
  eq("parse/serialise round trip", sp.toString(), "a=1&b=%F0%9F%98%80&c=+&d=&=e&a=3");
  eq("decoding", [sp.get("b"), sp.get("c"), sp.get("d"), sp.get(""), sp.getAll("a"), sp.size],
     ["\u{1F600}", " ", "", "e", ["1", "3"], 6]);
  eq("only one leading ? is stripped", [...new URLSearchParams("??a").keys()], ["?a"]);
  eq("record init", new URLSearchParams({ x: "1", y: "\u00e9" }).toString(), "x=1&y=%C3%A9");
  eq("sequence init", new URLSearchParams([["a", "b"], ["c", "d"]]).toString(), "a=b&c=d");
  eq("copy init", new URLSearchParams(new URLSearchParams("q=1")).toString(), "q=1");
  throws("bad pair throws TypeError", () => new URLSearchParams([["a"]]), TypeError);
  eq("form-urlencoded byte set", new URLSearchParams({ k: "*-._~!'()%&=+ /" }).toString(),
     "k=*-._%7E%21%27%28%29%25%26%3D%2B+%2F");
  eq("invalid percent sequences kept", new URLSearchParams("a=%zz%4").get("a"), "%zz%4");
  eq("invalid UTF-8 becomes U+FFFD", new URLSearchParams("a=%FF").get("a"), "\uFFFD");
  eq("null init is empty", new URLSearchParams(null).toString(), "");
  const edit = new URLSearchParams("a=1&b=2&a=3");
  edit.set("a", "9");
  eq("set replaces first and removes rest", edit.toString(), "a=9&b=2");
  edit.append("b", "4");
  eq("has(name, value)", [edit.has("b", "4"), edit.has("b", "5")], [true, false]);
  edit.delete("b", "2");
  eq("delete(name, value)", edit.toString(), "a=9&b=4");
  const sorted = new URLSearchParams("z=1&a=2&z=0&a=1");
  sorted.sort();
  eq("sort is stable", sorted.toString(), "a=2&a=1&z=1&z=0");
  const units = new URLSearchParams("\uFB03&\u{1F308}");
  units.sort();
  eq("sort compares UTF-16 code units", units.toString(), "%F0%9F%8C%88=&%EF%AC%83=");
  const seen = [];
  new URLSearchParams("x=1&y=2").forEach((value, key) => seen.push(`${key}:${value}`));
  eq("forEach order", seen, ["x:1", "y:2"]);
  eq("iteration", [...new URLSearchParams("x=1&y=2")], [["x", "1"], ["y", "2"]]);
  throws("get() without arguments throws TypeError", () => new URLSearchParams().get(), TypeError);
  eq("toStringTag", Object.prototype.toString.call(new URLSearchParams()), "[object URLSearchParams]");

  // ---- TextEncoder / TextDecoder ----------------------------------------
  const encoder = new TextEncoder();
  eq("encoding", [encoder.encoding, new TextDecoder().encoding], ["utf-8", "utf-8"]);
  eq("encode with lone surrogate", bytes(encoder.encode("\u20ac\u{1F600}\uD800")),
     [226, 130, 172, 240, 159, 152, 128, 239, 191, 189]);
  const target = new Uint8Array(3);
  eq("encodeInto stops before a split code point", encoder.encodeInto("a\u{1F600}", target), { read: 1, written: 1 });
  const decoder = new TextDecoder();
  eq("BOM is stripped", decoder.decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x68])), "h");
  eq("ignoreBOM keeps the BOM", new TextDecoder("utf-8", { ignoreBOM: true }).decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x68])), "\uFEFFh");
  eq("invalid lead byte", decoder.decode(new Uint8Array([0xff, 0x61])), "\uFFFDa");
  eq("truncated sequence", decoder.decode(new Uint8Array([0xe2, 0x82])), "\uFFFD");
  eq("maximal subpart replacement", decoder.decode(new Uint8Array([0xf0, 0x9f, 0x41])), "\uFFFDA");
  eq("encoded surrogate", decoder.decode(new Uint8Array([0xed, 0xa0, 0x80])), "\uFFFD\uFFFD\uFFFD");
  eq("overlong encoding", decoder.decode(new Uint8Array([0xc0, 0xaf])), "\uFFFD\uFFFD");
  eq("ArrayBuffer and DataView input",
     [decoder.decode(new Uint8Array([0x68, 0x69]).buffer), decoder.decode(new DataView(new Uint8Array([0, 0x6f, 0x6b]).buffer, 1))],
     ["hi", "ok"]);
  throws("fatal throws TypeError", () => new TextDecoder("utf-8", { fatal: true }).decode(new Uint8Array([0xff])), TypeError);
  const streaming = new TextDecoder();
  eq("streaming across chunks",
     [streaming.decode(new Uint8Array([0xe2, 0x82]), { stream: true }), streaming.decode(new Uint8Array([0xac]))],
     ["", "\u20ac"]);
  eq("label normalisation", new TextDecoder(" UTF8 ").encoding, "utf-8");
  eq("round trip of a long string", decoder.decode(encoder.encode("\u00e9\u{1F600}".repeat(5000))), "\u00e9\u{1F600}".repeat(5000));

  // ---- Timers and microtasks --------------------------------------------
  let ran = false;
  const id = setTimeout(() => { ran = true; }, 0);
  eq("setTimeout never runs during render", ran, false);
  clearTimeout(id);
  eq("setInterval and clearInterval exist", [typeof setInterval, typeof clearInterval], ["function", "function"]);
  throws("queueMicrotask requires a function", () => queueMicrotask(1), TypeError);

  if (isHydra) {
    eq("timer ids are increasing integers", (() => { const a = setTimeout(() => {}); const b = setInterval(() => {}); return b > a && Number.isInteger(a); })(), true);
    throws("non-UTF-8 labels are rejected explicitly", () => new TextDecoder("latin1"), RangeError);
    eq("native helpers are not exposed", typeof globalThis.__hydraNative, "undefined");
    eq("globals are non-enumerable", Object.keys(globalThis).filter((k) => ["URL", "URLSearchParams", "TextEncoder", "TextDecoder", "fetch", "setTimeout"].includes(k)), []);
  }

  // ---- Headers / Response -------------------------------------------------
  const headers = new Headers({ "X-B": "2", "x-a": " 1 " });
  headers.append("x-b", "3");
  headers.append("Set-Cookie", "a=1");
  headers.append("Set-Cookie", "b=2");
  eq("headers combine, trim and sort", [...headers], [["set-cookie", "a=1"], ["set-cookie", "b=2"], ["x-a", "1"], ["x-b", "2, 3"]]);
  eq("headers get is case-insensitive", headers.get("X-A"), "1");
  throws("invalid header name throws TypeError", () => headers.set("bad name", "v"), TypeError);
  throws("Response status out of range throws RangeError", () => new Response("", { status: 100 }), RangeError);
}

let asyncResults = null;
function asyncChecks() {
  const work = [];
  const check = (name, promise) => work.push(promise.then(() => {}, (error) => { failures.push(`${name}: ${error && error.message}`); }));
  check("Response body helpers", (async () => {
    const response = new Response("{\"a\":1}", { status: 201, statusText: "Created", headers: { "X-Y": "z" } });
    eq("constructed response", [response.status, response.ok, response.statusText, response.headers.get("x-y")], [201, true, "Created", "z"]);
    const copy = response.clone();
    eq("json()", await response.json(), { a: 1 });
    eq("bodyUsed", response.bodyUsed, true);
    eq("clone keeps its own body", await copy.text(), "{\"a\":1}");
    let rejected = false;
    await response.text().catch((error) => { rejected = error instanceof TypeError; });
    eq("second read rejects with TypeError", rejected, true);
    eq("Response.json", await Response.json({ b: 2 }).json(), { b: 2 });
  })());
  if (isHydra) {
    check("fetch over the bridge", (async () => {
      const response = await fetch("/api/echo?x=1#ignored", { method: "post", headers: { "X-A": "1" }, body: "hi" });
      eq("fetch resolves a Response", [response instanceof Response, response.ok, response.status, response.statusText, response.type, response.url],
         [true, true, 200, "OK", "basic", "/api/echo?x=1#ignored"]);
      const echoed = await response.json();
      eq("bridge received the request", [echoed.method, echoed.path, echoed.query, echoed.body, echoed.x_a],
         ["POST", "/api/echo", "x=1", "hi", "1"]);
      const missing = await fetch("/api/status/404");
      eq("HTTP errors resolve with ok=false", [missing.ok, missing.status, missing.statusText], [false, 404, "Not Found"]);
      const legacy = await fetch({ path: "/api/echo", query: "y=2" });
      eq("HydraStack object form still works", (await legacy.json()).query, "y=2");
      let rejected = null;
      await fetch("https://example.com/").catch((error) => { rejected = error; });
      eq("other origins reject with TypeError", rejected instanceof TypeError, true);
    })());
  }
  asyncResults = Promise.all(work);
}

const report = () => JSON.stringify({ passed, failures });

if (isHydra) {
  globalThis.render = (url) => {
    if (url === "/sync") {
      try {
        syncChecks();
      } catch (error) {
        failures.push(`sync checks aborted: ${error && error.stack}`);
      }
      asyncChecks();
      return report();
    }
    return report();
  };
} else {
  try {
    syncChecks();
  } catch (error) {
    failures.push(`sync checks aborted: ${error && error.stack}`);
  }
  asyncChecks();
  asyncResults.then(() => {
    console.log(report());
    if (failures.length > 0) process.exitCode = 1;
  });
}
