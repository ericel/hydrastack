#pragma once

#include "hydra/V8IsolatePool.h"

#include <drogon/HttpRequest.h>
#include <drogon/plugins/Plugin.h>
#include <json/value.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace hydra {

struct RenderOptions {
    std::string urlOverride;
};

class HydraSsrPlugin : public drogon::Plugin<HydraSsrPlugin> {
  public:
    ~HydraSsrPlugin();

    void initAndStart(const Json::Value &config) override;
    void shutdown() override;

    [[nodiscard]] std::string render(const drogon::HttpRequestPtr &req,
                                     const Json::Value &props,
                                     const RenderOptions &options = {}) const;

    [[nodiscard]] std::string render(const drogon::HttpRequestPtr &req,
                                     const std::string &propsJson,
                                     const RenderOptions &options = {}) const;

  private:
    std::string ssrBundlePath_;
    std::string cssPath_ = "/assets/app.css";
    std::string clientJsPath_ = "/assets/client.js";
    std::size_t isolatePoolSize_ = 0;
    std::uint64_t isolateAcquireTimeoutMs_ = 0;
    std::uint64_t renderTimeoutMs_ = 50;
    bool wrapFragment_ = false;

    std::unique_ptr<V8IsolatePool> isolatePool_;
};

}  // namespace hydra
