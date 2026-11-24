#include "V8RuntimeGlobals.h"

#include "ada.h"

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hydra::runtime_globals {
namespace {

// The bootstrap converts every string to a well-formed (USV) string before it
// crosses into C++, so Utf8Value always yields valid UTF-8 here.
std::string toUtf8(v8::Isolate *isolate, v8::Local<v8::Value> value) {
    v8::String::Utf8Value utf8(isolate, value);
    return *utf8 ? std::string(*utf8, static_cast<std::size_t>(utf8.length())) : std::string();
}

v8::Local<v8::String> toV8String(v8::Isolate *isolate, std::string_view value) {
    v8::Local<v8::String> out;
    if (!v8::String::NewFromUtf8(isolate,
                                 value.data(),
                                 v8::NewStringType::kNormal,
                                 static_cast<int>(value.size()))
             .ToLocal(&out)) {
        return v8::String::Empty(isolate);
    }
    return out;
}

// Component order shared with the bootstrap's URL class.
v8::Local<v8::Array> components(v8::Isolate *isolate, const ada::url_aggregator &url) {
    std::array<v8::Local<v8::Value>, 11> values = {
        toV8String(isolate, url.get_href()),
        toV8String(isolate, url.get_origin()),
        toV8String(isolate, url.get_protocol()),
        toV8String(isolate, url.get_username()),
        toV8String(isolate, url.get_password()),
        toV8String(isolate, url.get_host()),
        toV8String(isolate, url.get_hostname()),
        toV8String(isolate, url.get_port()),
        toV8String(isolate, url.get_pathname()),
        toV8String(isolate, url.get_search()),
        toV8String(isolate, url.get_hash()),
    };
    return v8::Array::New(isolate, values.data(), values.size());
}

// urlParse(input, base?) -> component array, or null when parsing fails.
void urlParse(const v8::FunctionCallbackInfo<v8::Value> &info) {
    auto *isolate = info.GetIsolate();
    v8::HandleScope scope(isolate);
    const std::string input = info.Length() > 0 ? toUtf8(isolate, info[0]) : std::string();
    ada::result<ada::url_aggregator> url;
    if (info.Length() > 1 && !info[1]->IsUndefined()) {
        const auto base = ada::parse<ada::url_aggregator>(toUtf8(isolate, info[1]));
        if (!base) {
            info.GetReturnValue().SetNull();
            return;
        }
        url = ada::parse<ada::url_aggregator>(input, &*base);
    } else {
        url = ada::parse<ada::url_aggregator>(input);
    }
    if (!url) {
        info.GetReturnValue().SetNull();
        return;
    }
    info.GetReturnValue().Set(components(isolate, *url));
}

// urlSet(href, field, value) -> component array after the setter. Setters that
// reject their input leave the URL unchanged, as in browsers. Only an invalid
// `href` returns null, so the caller can throw the spec's TypeError.
void urlSet(const v8::FunctionCallbackInfo<v8::Value> &info) {
    auto *isolate = info.GetIsolate();
    v8::HandleScope scope(isolate);
    if (info.Length() < 3) {
        info.GetReturnValue().SetNull();
        return;
    }
    auto url = ada::parse<ada::url_aggregator>(toUtf8(isolate, info[0]));
    if (!url) {
        info.GetReturnValue().SetNull();
        return;
    }
    const std::string field = toUtf8(isolate, info[1]);
    const std::string value = toUtf8(isolate, info[2]);
    if (field == "href") {
        if (!url->set_href(value)) {
            info.GetReturnValue().SetNull();
            return;
        }
    } else if (field == "protocol") {
        url->set_protocol(value);
    } else if (field == "username") {
        url->set_username(value);
    } else if (field == "password") {
        url->set_password(value);
    } else if (field == "host") {
        url->set_host(value);
    } else if (field == "hostname") {
        url->set_hostname(value);
    } else if (field == "port") {
        url->set_port(value);
    } else if (field == "pathname") {
        url->set_pathname(value);
    } else if (field == "search") {
        url->set_search(value);
    } else if (field == "hash") {
        url->set_hash(value);
    }
    info.GetReturnValue().Set(components(isolate, *url));
}

// formParse(query) -> [name0, value0, name1, value1, ...] using the
// application/x-www-form-urlencoded parser (one leading "?" is stripped).
void formParse(const v8::FunctionCallbackInfo<v8::Value> &info) {
    auto *isolate = info.GetIsolate();
    v8::HandleScope scope(isolate);
    ada::url_search_params params(info.Length() > 0 ? toUtf8(isolate, info[0]) : std::string());
    auto context = isolate->GetCurrentContext();
    auto out = v8::Array::New(isolate, static_cast<int>(params.size() * 2));
    uint32_t index = 0;
    auto entries = params.get_entries();
    while (entries.has_next()) {
        const auto entry = entries.next();
        if (!entry) {
            break;
        }
        if (out->Set(context, index++, toV8String(isolate, entry->first)).IsNothing() ||
            out->Set(context, index++, toV8String(isolate, entry->second)).IsNothing()) {
            return;
        }
    }
    info.GetReturnValue().Set(out);
}

// formSerialize([name0, value0, ...]) -> application/x-www-form-urlencoded string.
void formSerialize(const v8::FunctionCallbackInfo<v8::Value> &info) {
    auto *isolate = info.GetIsolate();
    v8::HandleScope scope(isolate);
    ada::url_search_params params;
    if (info.Length() > 0 && info[0]->IsArray()) {
        auto context = isolate->GetCurrentContext();
        auto flat = info[0].As<v8::Array>();
        const uint32_t length = flat->Length();
        for (uint32_t i = 0; i + 1 < length; i += 2) {
            v8::Local<v8::Value> name;
            v8::Local<v8::Value> value;
            if (!flat->Get(context, i).ToLocal(&name) ||
                !flat->Get(context, i + 1).ToLocal(&value)) {
                return;
            }
            params.append(toUtf8(isolate, name), toUtf8(isolate, value));
        }
    }
    info.GetReturnValue().Set(toV8String(isolate, params.to_string()));
}

void setFunction(v8::Isolate *isolate,
                 v8::Local<v8::Context> context,
                 v8::Local<v8::Object> target,
                 const char *name,
                 v8::FunctionCallback callback) {
    v8::Local<v8::Function> function;
    if (!v8::Function::New(context, callback).ToLocal(&function) ||
        !target->Set(context, toV8String(isolate, name), function).FromMaybe(false)) {
        throw std::runtime_error(std::string("Failed to install SSR runtime native: ") + name);
    }
}

}  // namespace

void installNatives(v8::Isolate *isolate, v8::Local<v8::Context> context) {
    auto natives = v8::Object::New(isolate);
    setFunction(isolate, context, natives, "urlParse", &urlParse);
    setFunction(isolate, context, natives, "urlSet", &urlSet);
    setFunction(isolate, context, natives, "formParse", &formParse);
    setFunction(isolate, context, natives, "formSerialize", &formSerialize);
    if (!context->Global()
             ->Set(context, toV8String(isolate, "__hydraNative"), natives)
             .FromMaybe(false)) {
        throw std::runtime_error("Failed to install SSR runtime natives");
    }
}

// Browser globals for server renders. Each one either matches browser
// behaviour or is absent; a substitute that quietly behaves differently is
// worse than a missing global, because feature checks pass and the server
// then renders different HTML from the browser (a hydration mismatch).
// docs/ssr-runtime.md lists what is provided.
const char *bootstrapSource() {
    return R"JS(
(function (native) {
"use strict";
const G = globalThis;
const define = (name, value) => {
  if (typeof G[name] === "undefined") {
    Object.defineProperty(G, name, { value, writable: true, configurable: true, enumerable: false });
  }
};
// WebIDL USVString conversion: ToString (throws on symbols), then lone surrogates -> U+FFFD.
const usv = (value) => `${value}`.toWellFormed();
const requireArgs = (count, actual, where) => {
  if (actual < count) {
    throw new TypeError(`Failed to execute ${where}: ${count} argument${count === 1 ? "" : "s"} required, but only ${actual} present.`);
  }
};

if (typeof G.global === "undefined") G.global = G;
if (typeof G.self === "undefined") G.self = G;
if (typeof G.process === "undefined") {
  G.process = { env: { NODE_ENV: "production" } };
} else if (!G.process.env) {
  G.process.env = { NODE_ENV: "production" };
} else if (!G.process.env.NODE_ENV) {
  G.process.env.NODE_ENV = "production";
}

// ---- Encoding (UTF-8 only) ----------------------------------------------
function utf8Encode(str) {
  const out = new Uint8Array(str.length * 3);
  let n = 0;
  for (let i = 0; i < str.length; i++) {
    const c = str.charCodeAt(i);
    if (c < 0x80) {
      out[n++] = c;
    } else if (c < 0x800) {
      out[n++] = 0xc0 | (c >> 6);
      out[n++] = 0x80 | (c & 0x3f);
    } else if (c >= 0xd800 && c <= 0xdbff) {
      const cp = 0x10000 + ((c - 0xd800) << 10) + (str.charCodeAt(++i) - 0xdc00);
      out[n++] = 0xf0 | (cp >> 18);
      out[n++] = 0x80 | ((cp >> 12) & 0x3f);
      out[n++] = 0x80 | ((cp >> 6) & 0x3f);
      out[n++] = 0x80 | (cp & 0x3f);
    } else {
      out[n++] = 0xe0 | (c >> 12);
      out[n++] = 0x80 | ((c >> 6) & 0x3f);
      out[n++] = 0x80 | (c & 0x3f);
    }
  }
  return out.slice(0, n);
}

class TextEncoder {
  get encoding() { return "utf-8"; }
  encode(input = "") {
    return utf8Encode(usv(input));
  }
  encodeInto(source, destination) {
    requireArgs(2, arguments.length, "'encodeInto' on 'TextEncoder'");
    if (!(destination instanceof Uint8Array)) {
      throw new TypeError("Failed to execute 'encodeInto' on 'TextEncoder': parameter 2 is not of type 'Uint8Array'.");
    }
    const str = usv(source);
    let read = 0;
    let written = 0;
    while (read < str.length) {
      const c = str.charCodeAt(read);
      const units = c >= 0xd800 && c <= 0xdbff ? 2 : 1;
      const bytes = utf8Encode(str.slice(read, read + units));
      if (written + bytes.length > destination.length) break;
      destination.set(bytes, written);
      written += bytes.length;
      read += units;
    }
    return { read, written };
  }
}
Object.defineProperty(TextEncoder.prototype, Symbol.toStringTag, { value: "TextEncoder", configurable: true });

const UTF8_LABELS = new Set(["unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "utf-8", "utf8", "x-unicode20utf8"]);
const toBytes = (input, where) => {
  if (input === undefined) return new Uint8Array(0);
  if (input instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && input instanceof SharedArrayBuffer)) {
    return new Uint8Array(input);
  }
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  throw new TypeError(`Failed to execute ${where}: The provided value is not of type '(ArrayBuffer or ArrayBufferView)'.`);
};

// WHATWG Encoding "UTF-8 decoder", with streaming and BOM handling.
class TextDecoder {
  #fatal; #ignoreBOM; #bomSeen = false; #doNotFlush = false;
  #codePoint = 0; #bytesSeen = 0; #bytesNeeded = 0; #lower = 0x80; #upper = 0xbf;
  constructor(label = "utf-8", options = undefined) {
    const normalized = usv(label).replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, "").toLowerCase();
    if (!UTF8_LABELS.has(normalized)) {
      throw new RangeError(`Failed to construct 'TextDecoder': The encoding label provided ('${label}') is not supported by the HydraStack SSR runtime, which decodes UTF-8 only.`);
    }
    if (options !== undefined && options !== null && typeof options !== "object" && typeof options !== "function") {
      throw new TypeError("Failed to construct 'TextDecoder': The provided value is not of type 'TextDecoderOptions'.");
    }
    this.#fatal = Boolean(options?.fatal);
    this.#ignoreBOM = Boolean(options?.ignoreBOM);
  }
  get encoding() { return "utf-8"; }
  get fatal() { return this.#fatal; }
  get ignoreBOM() { return this.#ignoreBOM; }
  #reset() {
    this.#codePoint = 0; this.#bytesSeen = 0; this.#bytesNeeded = 0; this.#lower = 0x80; this.#upper = 0xbf;
  }
  decode(input = undefined, options = undefined) {
    const bytes = toBytes(input, "'decode' on 'TextDecoder'");
    if (!this.#doNotFlush) {
      this.#reset();
      this.#bomSeen = false;
    }
    this.#doNotFlush = Boolean(options?.stream);
    const units = [];
    let out = "";
    const emit = (cp) => {
      if (!this.#bomSeen) {
        this.#bomSeen = true;
        if (cp === 0xfeff && !this.#ignoreBOM) return;
      }
      if (cp > 0xffff) {
        cp -= 0x10000;
        units.push(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
      } else {
        units.push(cp);
      }
      if (units.length >= 8192) {
        out += String.fromCharCode.apply(null, units);
        units.length = 0;
      }
    };
    const fail = () => {
      if (this.#fatal) {
        this.#reset();
        this.#doNotFlush = false;
        throw new TypeError("Failed to execute 'decode' on 'TextDecoder': The encoded data was not valid for encoding utf-8");
      }
      emit(0xfffd);
    };
    for (let i = 0; i < bytes.length; i++) {
      const b = bytes[i];
      if (this.#bytesNeeded === 0) {
        if (b <= 0x7f) {
          emit(b);
        } else if (b >= 0xc2 && b <= 0xdf) {
          this.#bytesNeeded = 1; this.#codePoint = b & 0x1f;
        } else if (b >= 0xe0 && b <= 0xef) {
          if (b === 0xe0) this.#lower = 0xa0;
          if (b === 0xed) this.#upper = 0x9f;
          this.#bytesNeeded = 2; this.#codePoint = b & 0xf;
        } else if (b >= 0xf0 && b <= 0xf4) {
          if (b === 0xf0) this.#lower = 0x90;
          if (b === 0xf4) this.#upper = 0x8f;
          this.#bytesNeeded = 3; this.#codePoint = b & 0x7;
        } else {
          fail();
        }
        continue;
      }
      if (b < this.#lower || b > this.#upper) {
        this.#reset();
        i--; // reprocess this byte as the start of a new sequence
        fail();
        continue;
      }
      this.#lower = 0x80; this.#upper = 0xbf;
      this.#codePoint = (this.#codePoint << 6) | (b & 0x3f);
      if (++this.#bytesSeen === this.#bytesNeeded) {
        const cp = this.#codePoint;
        this.#reset();
        emit(cp);
      }
    }
    if (!this.#doNotFlush && this.#bytesNeeded !== 0) {
      this.#reset();
      fail();
    }
    return out + String.fromCharCode.apply(null, units);
  }
}
Object.defineProperty(TextDecoder.prototype, Symbol.toStringTag, { value: "TextDecoder", configurable: true });
define("TextEncoder", TextEncoder);
define("TextDecoder", TextDecoder);

// ---- Microtasks and timers ------------------------------------------------
define("queueMicrotask", function queueMicrotask(callback) {
  if (typeof callback !== "function") {
    throw new TypeError("Failed to execute 'queueMicrotask': parameter 1 is not of type 'Function'.");
  }
  Promise.resolve().then(() => callback());
});
// A server render is synchronous, so in a browser no timer would have fired
// before the HTML was produced. Timers are accepted and never fire; running
// them immediately (the old shim) executed delayed work in the middle of render.
let lastTimerId = 0;
define("setTimeout", function setTimeout() { return ++lastTimerId; });
define("setInterval", function setInterval() { return ++lastTimerId; });
define("clearTimeout", function clearTimeout() {});
define("clearInterval", function clearInterval() {});

// ---- URL and URLSearchParams (WHATWG, parsed natively by ada) -------------
const FIELDS = ["href", "origin", "protocol", "username", "password", "host", "hostname", "port", "pathname", "search", "hash"];
const SEARCH = FIELDS.indexOf("search");
const urlState = new WeakMap();
const paramsState = new WeakMap();
const brand = (map, self, name) => {
  const state = map.get(self);
  if (state === undefined) throw new TypeError(`Illegal invocation: receiver is not a ${name}`);
  return state;
};
const parseUrl = (url, base) => native.urlParse(usv(url), base === undefined ? undefined : usv(base));
const parseForm = (query) => {
  const flat = native.formParse(query);
  const list = [];
  for (let i = 0; i < flat.length; i += 2) list.push([flat[i], flat[i + 1]]);
  return list;
};
const serializeForm = (list) => native.formSerialize(list.flat());

class URL {
  constructor(url, base = undefined) {
    requireArgs(1, arguments.length, "'URL' constructor");
    const components = parseUrl(url, base);
    if (components === null) throw new TypeError(`Failed to construct 'URL': Invalid URL`);
    urlState.set(this, { components, params: null });
  }
  static canParse(url, base = undefined) {
    requireArgs(1, arguments.length, "'canParse' on 'URL'");
    return parseUrl(url, base) !== null;
  }
  static parse(url, base = undefined) {
    requireArgs(1, arguments.length, "'parse' on 'URL'");
    return parseUrl(url, base) === null ? null : new URL(url, base);
  }
  get searchParams() {
    const state = brand(urlState, this, "URL");
    if (state.params === null) {
      state.params = new URLSearchParams(state.components[SEARCH]);
      paramsState.get(state.params).url = this;
    }
    return state.params;
  }
  toString() { return brand(urlState, this, "URL").components[0]; }
  toJSON() { return brand(urlState, this, "URL").components[0]; }
}
FIELDS.forEach((name, index) => {
  const descriptor = {
    get() { return brand(urlState, this, "URL").components[index]; },
    enumerable: true,
    configurable: true,
  };
  if (name !== "origin") {
    descriptor.set = function (value) {
      const state = brand(urlState, this, "URL");
      const next = native.urlSet(state.components[0], name, usv(value));
      if (next === null) throw new TypeError(`Failed to set the 'href' property on 'URL': Invalid URL`);
      state.components = next;
      if (state.params !== null && (name === "href" || name === "search")) {
        paramsState.get(state.params).list = parseForm(next[SEARCH]);
      }
    };
  }
  Object.defineProperty(URL.prototype, name, descriptor);
});
Object.defineProperty(URL.prototype, "searchParams", { enumerable: true });
Object.defineProperty(URL.prototype, Symbol.toStringTag, { value: "URL", configurable: true });

class URLSearchParams {
  constructor(init = "") {
    const state = { list: [], url: null };
    paramsState.set(this, state);
    if (init === null) return; // WebIDL: null converts to the (empty) record branch
    if (typeof init === "object" || typeof init === "function") {
      const iterator = init[Symbol.iterator];
      if (iterator !== undefined) {
        if (typeof iterator !== "function") throw new TypeError("Failed to construct 'URLSearchParams': The object must have a callable @@iterator property.");
        for (const pair of init) {
          const entry = Array.from(pair);
          if (entry.length !== 2) throw new TypeError("Failed to construct 'URLSearchParams': Sequence initializer must only contain pair elements");
          state.list.push([usv(entry[0]), usv(entry[1])]);
        }
      } else {
        for (const key of Reflect.ownKeys(init)) {
          const descriptor = Reflect.getOwnPropertyDescriptor(init, key);
          if (descriptor === undefined || !descriptor.enumerable) continue;
          const name = usv(key);
          const value = usv(init[key]);
          const existing = state.list.findIndex(([k]) => k === name);
          if (existing >= 0) state.list[existing][1] = value;
          else state.list.push([name, value]);
        }
      }
    } else {
      state.list = parseForm(usv(init));
    }
  }
  #update() {
    const state = paramsState.get(this);
    if (state.url === null) return;
    const owner = urlState.get(state.url);
    owner.components = native.urlSet(owner.components[0], "search", serializeForm(state.list));
  }
  get size() { return brand(paramsState, this, "URLSearchParams").list.length; }
  append(name, value) {
    requireArgs(2, arguments.length, "'append' on 'URLSearchParams'");
    brand(paramsState, this, "URLSearchParams").list.push([usv(name), usv(value)]);
    this.#update();
  }
  delete(name, value = undefined) {
    requireArgs(1, arguments.length, "'delete' on 'URLSearchParams'");
    const state = brand(paramsState, this, "URLSearchParams");
    const key = usv(name);
    const match = value === undefined ? null : usv(value);
    state.list = state.list.filter(([k, v]) => k !== key || (match !== null && v !== match));
    this.#update();
  }
  get(name) {
    requireArgs(1, arguments.length, "'get' on 'URLSearchParams'");
    const key = usv(name);
    const found = brand(paramsState, this, "URLSearchParams").list.find(([k]) => k === key);
    return found === undefined ? null : found[1];
  }
  getAll(name) {
    requireArgs(1, arguments.length, "'getAll' on 'URLSearchParams'");
    const key = usv(name);
    return brand(paramsState, this, "URLSearchParams").list.filter(([k]) => k === key).map(([, v]) => v);
  }
  has(name, value = undefined) {
    requireArgs(1, arguments.length, "'has' on 'URLSearchParams'");
    const key = usv(name);
    const match = value === undefined ? null : usv(value);
    return brand(paramsState, this, "URLSearchParams").list.some(([k, v]) => k === key && (match === null || v === match));
  }
  set(name, value) {
    requireArgs(2, arguments.length, "'set' on 'URLSearchParams'");
    const state = brand(paramsState, this, "URLSearchParams");
    const key = usv(name);
    const text = usv(value);
    const first = state.list.findIndex(([k]) => k === key);
    if (first < 0) {
      state.list.push([key, text]);
    } else {
      state.list[first][1] = text;
      state.list = state.list.filter(([k], i) => i <= first || k !== key);
    }
    this.#update();
  }
  sort() {
    // Array.prototype.sort is stable; string comparison is by UTF-16 code units, as the spec requires.
    brand(paramsState, this, "URLSearchParams").list.sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0));
    this.#update();
  }
  forEach(callback, thisArg = undefined) {
    requireArgs(1, arguments.length, "'forEach' on 'URLSearchParams'");
    if (typeof callback !== "function") throw new TypeError("Failed to execute 'forEach' on 'URLSearchParams': The callback provided as parameter 1 is not a function.");
    const state = brand(paramsState, this, "URLSearchParams");
    for (let i = 0; i < state.list.length; i++) callback.call(thisArg, state.list[i][1], state.list[i][0], this);
  }
  *entries() {
    const state = brand(paramsState, this, "URLSearchParams");
    for (let i = 0; i < state.list.length; i++) yield [state.list[i][0], state.list[i][1]];
  }
  *keys() {
    for (const [k] of this.entries()) yield k;
  }
  *values() {
    for (const [, v] of this.entries()) yield v;
  }
  toString() { return serializeForm(brand(paramsState, this, "URLSearchParams").list); }
}
URLSearchParams.prototype[Symbol.iterator] = URLSearchParams.prototype.entries;
Object.defineProperty(URLSearchParams.prototype, Symbol.toStringTag, { value: "URLSearchParams", configurable: true });
define("URL", URL);
define("URLSearchParams", URLSearchParams);

// ---- fetch, Headers, Response ---------------------------------------------
const TOKEN = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
const headerName = (name) => {
  const text = `${name}`;
  if (!TOKEN.test(text)) throw new TypeError(`Invalid header name: '${text}'`);
  return text.toLowerCase();
};
const headerValue = (value) => {
  const text = `${value}`.replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, "");
  if (/[\0\r\n]/.test(text)) throw new TypeError(`Invalid header value for header`);
  return text;
};
class Headers {
  #list = [];
  constructor(init = undefined) {
    if (init === undefined || init === null) return;
    if (typeof init !== "object" && typeof init !== "function") throw new TypeError("Failed to construct 'Headers': The provided value is not of type 'HeadersInit'.");
    if (typeof init[Symbol.iterator] === "function") {
      for (const pair of init) {
        const entry = Array.from(pair);
        if (entry.length !== 2) throw new TypeError("Failed to construct 'Headers': Invalid value");
        this.append(entry[0], entry[1]);
      }
    } else {
      for (const key of Object.keys(init)) this.append(key, init[key]);
    }
  }
  append(name, value) { this.#list.push([headerName(name), headerValue(value)]); }
  delete(name) { const key = headerName(name); this.#list = this.#list.filter(([k]) => k !== key); }
  get(name) {
    const key = headerName(name);
    const values = this.#list.filter(([k]) => k === key).map(([, v]) => v);
    return values.length === 0 ? null : values.join(", ");
  }
  getSetCookie() { return this.#list.filter(([k]) => k === "set-cookie").map(([, v]) => v); }
  has(name) { const key = headerName(name); return this.#list.some(([k]) => k === key); }
  set(name, value) {
    const key = headerName(name);
    const text = headerValue(value);
    const first = this.#list.findIndex(([k]) => k === key);
    if (first < 0) { this.#list.push([key, text]); return; }
    this.#list[first][1] = text;
    this.#list = this.#list.filter(([k], i) => i <= first || k !== key);
  }
  *entries() {
    const names = [...new Set(this.#list.map(([k]) => k))].sort();
    for (const name of names) {
      if (name === "set-cookie") {
        for (const value of this.getSetCookie()) yield [name, value];
      } else {
        yield [name, this.get(name)];
      }
    }
  }
  *keys() { for (const [k] of this.entries()) yield k; }
  *values() { for (const [, v] of this.entries()) yield v; }
  forEach(callback, thisArg = undefined) {
    for (const [k, v] of this.entries()) callback.call(thisArg, v, k, this);
  }
}
Headers.prototype[Symbol.iterator] = Headers.prototype.entries;
Object.defineProperty(Headers.prototype, Symbol.toStringTag, { value: "Headers", configurable: true });

const STATUS_TEXT = {
  200: "OK", 201: "Created", 202: "Accepted", 204: "No Content", 301: "Moved Permanently", 302: "Found",
  303: "See Other", 304: "Not Modified", 307: "Temporary Redirect", 308: "Permanent Redirect",
  400: "Bad Request", 401: "Unauthorized", 403: "Forbidden", 404: "Not Found", 405: "Method Not Allowed",
  409: "Conflict", 410: "Gone", 422: "Unprocessable Content", 429: "Too Many Requests",
  500: "Internal Server Error", 501: "Not Implemented", 502: "Bad Gateway", 503: "Service Unavailable", 504: "Gateway Timeout",
};
const NULL_BODY_STATUS = new Set([101, 103, 204, 205, 304]);
const bodyText = (body) => {
  if (body === undefined || body === null) return null;
  if (typeof body === "string") return body;
  if (body instanceof URLSearchParams) return body.toString();
  if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) return new TextDecoder().decode(body);
  return `${body}`;
};
let bridgeResponse;
class Response {
  #body; #used = false; #status; #statusText; #headers; #url = ""; #type = "default";
  constructor(body = null, init = undefined) {
    const status = init?.status === undefined ? 200 : Number(init.status);
    if (!Number.isInteger(status) || status < 200 || status > 599) {
      throw new RangeError(`Failed to construct 'Response': The status provided (${init?.status}) is outside the range [200, 599].`);
    }
    this.#status = status;
    this.#statusText = init?.statusText === undefined ? "" : `${init.statusText}`;
    this.#headers = new Headers(init?.headers);
    this.#body = bodyText(body);
    if (this.#body !== null && body instanceof URLSearchParams && !this.#headers.has("content-type")) {
      this.#headers.set("content-type", "application/x-www-form-urlencoded;charset=UTF-8");
    } else if (typeof body === "string" && !this.#headers.has("content-type")) {
      this.#headers.set("content-type", "text/plain;charset=UTF-8");
    }
  }
  static {
    bridgeResponse = (raw, url) => {
      const response = new Response(null);
      const status = Number(raw?.status);
      response.#status = Number.isInteger(status) && status >= 100 && status <= 599 ? status : 500;
      response.#statusText = STATUS_TEXT[response.#status] ?? "";
      response.#headers = new Headers(raw?.headers ?? {});
      response.#body = NULL_BODY_STATUS.has(response.#status) ? null : bodyText(raw?.body ?? "");
      response.#url = url;
      response.#type = "basic";
      return response;
    };
  }
  static json(data, init = undefined) {
    const headers = new Headers(init?.headers);
    if (!headers.has("content-type")) headers.set("content-type", "application/json");
    return new Response(JSON.stringify(data), { ...init, headers });
  }
  get status() { return this.#status; }
  get ok() { return this.#status >= 200 && this.#status <= 299; }
  get statusText() { return this.#statusText; }
  get headers() { return this.#headers; }
  get url() { return this.#url; }
  get redirected() { return false; }
  get type() { return this.#type; }
  get bodyUsed() { return this.#used; }
  // Streams are not available during SSR; read the body with text(), json() or arrayBuffer().
  get body() { return null; }
  #consume() {
    if (this.#used) return Promise.reject(new TypeError("Failed to execute on 'Response': body stream already read"));
    this.#used = true;
    return Promise.resolve(this.#body ?? "");
  }
  text() { return this.#consume(); }
  json() { return this.#consume().then((text) => JSON.parse(text)); }
  arrayBuffer() { return this.#consume().then((text) => utf8Encode(text.toWellFormed()).buffer); }
  clone() {
    if (this.#used) throw new TypeError("Failed to execute 'clone' on 'Response': Response body is already used");
    const copy = bridgeResponse({ status: this.#status }, this.#url);
    copy.#headers = new Headers(this.#headers);
    copy.#statusText = this.#statusText;
    copy.#type = this.#type;
    copy.#body = this.#body;
    return copy;
  }
}
Object.defineProperty(Response.prototype, Symbol.toStringTag, { value: "Response", configurable: true });
define("Headers", Headers);
define("Response", Response);

if (typeof G.hydra === "undefined") G.hydra = {};
if (typeof G.hydra.fetch !== "function") {
  // Synchronous HydraStack bridge: hydra.fetch({ method, path, query, headers, body }) -> { status, body, headers }.
  G.hydra.fetch = (request = {}) => {
    const payload = typeof request === "string" ? request : JSON.stringify(request);
    const raw = G.__hydraFetch(payload);
    if (typeof raw === "string") {
      try {
        return JSON.parse(raw);
      } catch {
        return { status: 500, body: "Invalid bridge response", headers: {} };
      }
    }
    return raw;
  };
}
const METHODS = new Set(["DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT"]);
// Browser-shaped fetch over the bridge. It reaches this app's own paths only
// ("/api/..."); a browser would also reach other origins, so those reject
// with the same TypeError a network failure produces. The HydraStack object
// form fetch({ path, ... }) is still accepted.
define("fetch", function fetch(input, init = undefined) {
  try {
    requireArgs(1, arguments.length, "'fetch'");
    let request;
    let target;
    if (input !== null && typeof input === "object" && typeof input.path === "string" && !(input instanceof URL)) {
      request = input;
      target = input.query ? `${input.path}?${input.query}` : input.path;
    } else {
      target = typeof input === "object" && input !== null && typeof input.url === "string" ? input.url : usv(input);
      if (!target.startsWith("/") || target.startsWith("//")) {
        throw new TypeError(`Failed to fetch: the HydraStack SSR runtime only fetches this app's own paths (for example "/api/..."), not '${target}'.`);
      }
      const withoutHash = target.split("#")[0];
      const queryAt = withoutHash.indexOf("?");
      const method = `${init?.method ?? input?.method ?? "GET"}`;
      request = {
        method: METHODS.has(method.toUpperCase()) ? method.toUpperCase() : method,
        path: queryAt < 0 ? withoutHash : withoutHash.slice(0, queryAt),
        query: queryAt < 0 ? "" : withoutHash.slice(queryAt + 1),
        headers: Object.fromEntries(new Headers(init?.headers ?? input?.headers)),
      };
      const body = bodyText(init?.body);
      if (body !== null) {
        request.body = body;
        if (init.body instanceof URLSearchParams && request.headers["content-type"] === undefined) {
          request.headers["content-type"] = "application/x-www-form-urlencoded;charset=UTF-8";
        }
      }
    }
    return Promise.resolve(bridgeResponse(G.hydra.fetch(request), target));
  } catch (error) {
    return Promise.reject(error);
  }
});
})(globalThis.__hydraNative);
delete globalThis.__hydraNative;
)JS";
}

}  // namespace hydra::runtime_globals
