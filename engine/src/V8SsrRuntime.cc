#include "hydra/V8SsrRuntime.h"

#include <v8.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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

}  // namespace

V8SsrRuntime::V8SsrRuntime(std::string bundlePath)
    : bundlePath_(std::move(bundlePath)),
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

void V8SsrRuntime::loadBundle() {
    const std::string bundleSource = readFile(bundlePath_);

    v8::Locker locker(isolate_);
    v8::Isolate::Scope isolateScope(isolate_);
    v8::HandleScope handleScope(isolate_);
    auto context = context_.Get(isolate_);
    v8::Context::Scope contextScope(context);
    v8::TryCatch tryCatch(isolate_);

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
        throw std::runtime_error("SSR bundle missing globalThis.render(url, propsJson)");
    }

    auto renderFunc = v8::Local<v8::Function>::Cast(renderValue);
    v8::Local<v8::Value> args[2] = {
        toV8String(isolate_, url),
        toV8String(isolate_, propsJson),
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
