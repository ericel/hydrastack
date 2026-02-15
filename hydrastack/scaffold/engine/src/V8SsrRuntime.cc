#include "hydra/V8SsrRuntime.h"

#include <v8.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <json/reader.h>
#include <json/writer.h>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace hydra {
namespace {

class IsolateCleanup {
  public:
    v8::Isolate *isolate = nullptr;

    ~IsolateCleanup() {
        if (isolate != nullptr) {
            isolate->Dispose();
        }
    }
};

class RenderWatchdog {
  public:
    RenderWatchdog(v8::Isolate *isolate, std::uint64_t timeoutMs)
        : isolate_(isolate), timeoutMs_(timeoutMs) {
        if (timeoutMs_ == 0) {
            return;
        }

        thread_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto stopped = cv_.wait_for(lock,
                                              std::chrono::milliseconds(timeoutMs_),
                                              [this] { return done_; });
            if (!stopped) {
                isolate_->TerminateExecution();
            }
        });
    }

    ~RenderWatchdog() {
        stop();
    }

    RenderWatchdog(const RenderWatchdog &) = delete;
    RenderWatchdog &operator=(const RenderWatchdog &) = delete;

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

  private:
    v8::Isolate *isolate_ = nullptr;
    std::uint64_t timeoutMs_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
};

std::string readFile(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open SSR bundle: " + path);
    }

    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string formatException(v8::Isolate *isolate, v8::TryCatch &tryCatch) {
    v8::HandleScope handleScope(isolate);
    std::ostringstream stream;

    v8::String::Utf8Value exceptionUtf8(isolate, tryCatch.Exception());
    stream << (*exceptionUtf8 ? *exceptionUtf8 : "Unknown V8 exception");

    auto context = isolate->GetCurrentContext();
    auto message = tryCatch.Message();
    if (!message.IsEmpty()) {
        const int line = message->GetLineNumber(context).FromMaybe(0);
        v8::String::Utf8Value scriptName(isolate, message->GetScriptResourceName());
        stream << " (" << (*scriptName ? *scriptName : "<script>") << ":" << line
               << ")";
    }

    return stream.str();
}

v8::Local<v8::String> toV8String(v8::Isolate *isolate, const std::string &value) {
    v8::Local<v8::String> out;
    if (!v8::String::NewFromUtf8(
             isolate,
             value.c_str(),
             v8::NewStringType::kNormal,
             static_cast<int>(value.size()))
             .ToLocal(&out)) {
        throw std::runtime_error("Unable to allocate V8 string");
    }

    return out;
}

bool parseJsonString(const std::string &json, Json::Value *out) {
    if (out == nullptr) {
        return false;
    }

    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(json);
    return Json::parseFromStream(builder, stream, out, &errors);
}

std::string toCompactJsonString(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    return Json::writeString(builder, value);
}

}  // namespace

V8SsrRuntime::V8SsrRuntime(std::string bundlePath, FetchBridge fetchBridge)
    : bundlePath_(std::move(bundlePath)),
      fetchBridge_(std::move(fetchBridge)),
      allocator_(v8::ArrayBuffer::Allocator::NewDefaultAllocator()) {
    v8::Isolate::CreateParams createParams;
    createParams.array_buffer_allocator = allocator_.get();
    IsolateCleanup cleanup;
    cleanup.isolate = v8::Isolate::New(createParams);
    isolate_ = cleanup.isolate;

    if (isolate_ == nullptr) {
        throw std::runtime_error("Failed to create V8 isolate");
    }

    try {
        {
            v8::Locker locker(isolate_);
            v8::Isolate::Scope isolateScope(isolate_);
            v8::HandleScope handleScope(isolate_);
            auto context = v8::Context::New(isolate_);
            context_.Reset(isolate_, context);
            isolate_->SetData(0, this);
            loadBundle();
        }

        cleanup.isolate = nullptr;
    } catch (...) {
        // Ensure the global context handle is released before isolate cleanup.
        context_.Reset();
        throw;
    }
}

V8SsrRuntime::~V8SsrRuntime() {
    context_.Reset();
    if (isolate_ != nullptr) {
        isolate_->Dispose();
        isolate_ = nullptr;
    }
}

void V8SsrRuntime::hydraFetchCallback(
    const v8::FunctionCallbackInfo<v8::Value> &info) {
    auto *isolate = info.GetIsolate();
    auto *runtime = static_cast<V8SsrRuntime *>(isolate->GetData(0));
    if (runtime == nullptr) {
        info.GetReturnValue().Set(toV8String(
            isolate,
            R"({"status":500,"body":"Hydra runtime unavailable"})"));
        return;
    }

    std::string requestJson = "{}";
    if (info.Length() > 0) {
        v8::String::Utf8Value requestUtf8(isolate, info[0]);
        if (*requestUtf8) {
            requestJson = *requestUtf8;
        }
    }

    BridgeRequest request;
    Json::Value parsedRequest;
    if (parseJsonString(requestJson, &parsedRequest) && parsedRequest.isObject()) {
        request.method = parsedRequest.get("method", "GET").asString();
        request.path = parsedRequest.get("path", "").asString();
        request.query = parsedRequest.get("query", "").asString();
        if (parsedRequest.isMember("body")) {
            if (parsedRequest["body"].isString()) {
                request.body = parsedRequest["body"].asString();
            } else {
                request.body = toCompactJsonString(parsedRequest["body"]);
            }
        }
        if (parsedRequest.isMember("headers") && parsedRequest["headers"].isObject()) {
            for (const auto &headerName : parsedRequest["headers"].getMemberNames()) {
                request.headers.emplace(headerName,
                                        parsedRequest["headers"][headerName].asString());
            }
        }
    }

    BridgeResponse response;
    try {
        if (runtime->fetchBridge_) {
            response = runtime->fetchBridge_(request);
        } else {
            response.status = 501;
            response.body = "Hydra API bridge is not configured";
        }
    } catch (const std::exception &ex) {
        response.status = 500;
        response.body = ex.what();
    } catch (...) {
        response.status = 500;
        response.body = "Unknown Hydra API bridge error";
    }

    Json::Value responseJson(Json::objectValue);
    responseJson["status"] = response.status;
    responseJson["body"] = response.body;
    Json::Value responseHeaders(Json::objectValue);
    for (const auto &[headerName, headerValue] : response.headers) {
        responseHeaders[headerName] = headerValue;
    }
    responseJson["headers"] = std::move(responseHeaders);

    info.GetReturnValue().Set(toV8String(isolate, toCompactJsonString(responseJson)));
}

void V8SsrRuntime::loadBundle() {
    const std::string bundleSource = readFile(bundlePath_);

    v8::Locker locker(isolate_);
    v8::Isolate::Scope isolateScope(isolate_);
    v8::HandleScope handleScope(isolate_);
    auto context = context_.Get(isolate_);
    v8::Context::Scope contextScope(context);
    v8::TryCatch tryCatch(isolate_);

    auto fetchFunction = v8::Function::New(context, &V8SsrRuntime::hydraFetchCallback);
    if (fetchFunction.IsEmpty() ||
        !context->Global()
             ->Set(context, toV8String(isolate_, "__hydraFetch"), fetchFunction.ToLocalChecked())
             .FromMaybe(false)) {
        throw std::runtime_error("Failed to install Hydra API bridge function");
    }

    const char *bootstrapSource = R"(
if (typeof globalThis.global === "undefined") globalThis.global = globalThis;
if (typeof globalThis.self === "undefined") globalThis.self = globalThis;
if (typeof globalThis.process === "undefined") {
  globalThis.process = { env: { NODE_ENV: "production" } };
} else if (!globalThis.process.env) {
  globalThis.process.env = { NODE_ENV: "production" };
} else if (!globalThis.process.env.NODE_ENV) {
  globalThis.process.env.NODE_ENV = "production";
}
if (typeof globalThis.TextEncoder === "undefined") {
  globalThis.TextEncoder = class TextEncoder {
    encode(input = "") {
      const normalized = String(input);
      const encoded = unescape(encodeURIComponent(normalized));
      const bytes = new Uint8Array(encoded.length);
      for (let i = 0; i < encoded.length; ++i) {
        bytes[i] = encoded.charCodeAt(i);
      }
      return bytes;
    }
  };
}
if (typeof globalThis.TextDecoder === "undefined") {
  globalThis.TextDecoder = class TextDecoder {
    decode(input = new Uint8Array()) {
      let raw = "";
      for (let i = 0; i < input.length; ++i) {
        raw += String.fromCharCode(input[i]);
      }
      return decodeURIComponent(escape(raw));
    }
  };
}
if (typeof globalThis.queueMicrotask === "undefined") {
  globalThis.queueMicrotask = (fn) => Promise.resolve().then(fn);
}
if (typeof globalThis.setTimeout === "undefined") {
  globalThis.setTimeout = (fn) => {
    if (typeof fn === "function") fn();
    return 0;
  };
}
if (typeof globalThis.clearTimeout === "undefined") {
  globalThis.clearTimeout = () => {};
}
if (typeof globalThis.hydra === "undefined") {
  globalThis.hydra = {};
}
if (typeof globalThis.hydra.fetch !== "function") {
  globalThis.hydra.fetch = (request = {}) => {
    const payload = typeof request === "string" ? request : JSON.stringify(request);
    const raw = globalThis.__hydraFetch(payload);
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
if (typeof globalThis.fetch !== "function") {
  globalThis.fetch = (request = {}) => Promise.resolve(globalThis.hydra.fetch(request));
}
)";
    v8::Local<v8::Script> bootstrapScript;
    if (!v8::Script::Compile(context, toV8String(isolate_, bootstrapSource))
             .ToLocal(&bootstrapScript) ||
        bootstrapScript->Run(context).IsEmpty()) {
        throw std::runtime_error("Failed to run V8 bootstrap script: " +
                                 formatException(isolate_, tryCatch));
    }

    auto source = toV8String(isolate_, bundleSource);
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(context, source).ToLocal(&script)) {
        throw std::runtime_error("Failed to compile SSR bundle: " +
                                 formatException(isolate_, tryCatch));
    }

    if (script->Run(context).IsEmpty()) {
        throw std::runtime_error("Failed to run SSR bundle: " +
                                 formatException(isolate_, tryCatch));
    }
}

std::string V8SsrRuntime::render(const std::string &url,
                                 const std::string &propsJson,
                                 const std::string &requestContextJson,
                                 std::uint64_t timeoutMs) {
    v8::Locker locker(isolate_);
    v8::Isolate::Scope isolateScope(isolate_);
    v8::HandleScope handleScope(isolate_);
    auto context = context_.Get(isolate_);
    v8::Context::Scope contextScope(context);
    v8::TryCatch tryCatch(isolate_);
    RenderWatchdog watchdog(isolate_, timeoutMs);

    auto renderName = toV8String(isolate_, "render");
    v8::Local<v8::Value> renderValue;
    if (!context->Global()->Get(context, renderName).ToLocal(&renderValue) ||
        !renderValue->IsFunction()) {
        throw std::runtime_error(
            "SSR bundle missing globalThis.render(url, propsJson, requestContextJson)");
    }

    auto renderFunc = v8::Local<v8::Function>::Cast(renderValue);
    v8::Local<v8::Value> args[3] = {
        toV8String(isolate_, url),
        toV8String(isolate_, propsJson),
        toV8String(isolate_, requestContextJson),
    };

    v8::Local<v8::Value> result;
    if (!renderFunc
             ->Call(context, context->Global(), static_cast<int>(std::size(args)), args)
             .ToLocal(&result)) {
        if (tryCatch.HasTerminated()) {
            isolate_->CancelTerminateExecution();
            throw std::runtime_error("SSR render exceeded timeout of " +
                                     std::to_string(timeoutMs) + "ms");
        }
        throw std::runtime_error("SSR render threw exception: " +
                                 formatException(isolate_, tryCatch));
    }

    v8::Local<v8::String> resultString;
    if (!result->ToString(context).ToLocal(&resultString)) {
        throw std::runtime_error("SSR render did not return a string");
    }

    v8::String::Utf8Value utf8(isolate_, resultString);
    return *utf8 ? *utf8 : std::string();
}

}  // namespace hydra
