#pragma once

#include <v8.h>

#include <cstdint>
#include <memory>
#include <string>

namespace hydra {

class V8SsrRuntime {
  public:
    explicit V8SsrRuntime(std::string bundlePath);
    ~V8SsrRuntime();

    V8SsrRuntime(const V8SsrRuntime &) = delete;
    V8SsrRuntime &operator=(const V8SsrRuntime &) = delete;

    [[nodiscard]] std::string render(const std::string &url,
                                     const std::string &propsJson,
                                     std::uint64_t timeoutMs = 0);

  private:
    void loadBundle();

    std::string bundlePath_;
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator_;
    v8::Isolate *isolate_ = nullptr;
    v8::Global<v8::Context> context_;
};

}  // namespace hydra
