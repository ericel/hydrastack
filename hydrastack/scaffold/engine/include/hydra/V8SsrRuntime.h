#pragma once

#include <v8.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace hydra {

class V8SsrRuntime {
  public:
    struct BridgeRequest {
        std::string method = "GET";
        std::string path;
        std::string query;
        std::string body;
        std::unordered_map<std::string, std::string> headers;
    };

    struct BridgeResponse {
        int status = 200;
        std::string body;
        std::unordered_map<std::string, std::string> headers;
    };

    using FetchBridge = std::function<BridgeResponse(const BridgeRequest &)>;

    explicit V8SsrRuntime(std::string bundlePath, FetchBridge fetchBridge = {});
    ~V8SsrRuntime();

    V8SsrRuntime(const V8SsrRuntime &) = delete;
    V8SsrRuntime &operator=(const V8SsrRuntime &) = delete;

    [[nodiscard]] std::string render(const std::string &url,
                                     const std::string &propsJson,
                                     const std::string &requestContextJson = "{}",
                                     std::uint64_t timeoutMs = 0);

  private:
    static void hydraFetchCallback(const v8::FunctionCallbackInfo<v8::Value> &info);
    void loadBundle();

    std::string bundlePath_;
    FetchBridge fetchBridge_;
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator_;
    v8::Isolate *isolate_ = nullptr;
    v8::Global<v8::Context> context_;
};

}  // namespace hydra
