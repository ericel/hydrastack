#pragma once

#include "hydra/Config.h"
#include "hydra/V8IsolatePool.h"

#include <drogon/HttpRequest.h>
#include <drogon/plugins/Plugin.h>
#include <json/value.h>

#include <cstddef>
#include <cstdint>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hydra {

struct RenderOptions {
    std::string urlOverride;
};

struct ApiBridgeRequest {
    std::string method = "GET";
    std::string path;
    std::string query;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
};

struct ApiBridgeResponse {
    int status = 200;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
};

using ApiBridgeHandler = std::function<ApiBridgeResponse(const ApiBridgeRequest &)>;

struct HydraMetricsSnapshot {
    std::uint64_t requestsOk = 0;
    std::uint64_t requestsFail = 0;
    std::uint64_t renderErrors = 0;
    std::uint64_t poolTimeouts = 0;
    std::uint64_t renderTimeouts = 0;
    std::uint64_t runtimeRecycles = 0;
    std::uint64_t totalAcquireWaitUs = 0;
    std::uint64_t totalRenderUs = 0;
    std::uint64_t totalWrapUs = 0;
    std::uint64_t totalRequestUs = 0;
    std::uint64_t totalAcquireWaitMs = 0;
    std::uint64_t totalRenderMs = 0;
    std::uint64_t totalWrapMs = 0;
    std::uint64_t totalRequestMs = 0;
};

struct SsrRenderResult {
    std::string html;
    int status = 200;
    std::unordered_map<std::string, std::string> headers;
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
    [[nodiscard]] SsrRenderResult renderResult(const drogon::HttpRequestPtr &req,
                                               const Json::Value &props,
                                               const RenderOptions &options = {}) const;
    [[nodiscard]] SsrRenderResult renderResult(const drogon::HttpRequestPtr &req,
                                               const std::string &propsJson,
                                               const RenderOptions &options = {}) const;

    [[nodiscard]] HydraMetricsSnapshot metricsSnapshot() const;
    [[nodiscard]] std::string metricsPrometheus() const;

    void setApiBridgeHandler(ApiBridgeHandler handler);

  private:
    [[nodiscard]] std::string buildRouteUrl(const drogon::HttpRequestPtr &req,
                                            const RenderOptions &options) const;
    [[nodiscard]] Json::Value buildRequestContext(const drogon::HttpRequestPtr &req,
                                                  const std::string &routeUrl,
                                                  const std::string &requestId) const;
    [[nodiscard]] std::string resolveRequestId(const drogon::HttpRequestPtr &req) const;
    void observeAcquireWait(double valueMs) const;
    void observeRenderLatency(double valueMs) const;
    void observeRequestLatency(double valueMs) const;
    void observeRequestCode(int statusCode) const;
    [[nodiscard]] V8SsrRuntime::BridgeResponse dispatchApiBridge(
        const V8SsrRuntime::BridgeRequest &request) const;
    void registerDevProxyRoutes();

    std::string ssrBundlePath_;
    std::string cssPath_;
    std::string clientJsPath_;
    std::string assetManifestPath_ = "./public/assets/manifest.json";
    std::string assetPublicPrefix_ = "/assets";
    std::string clientManifestEntry_ = "src/entry-client.tsx";
    std::size_t isolatePoolSize_ = 0;
    std::uint64_t isolateAcquireTimeoutMs_ = 0;
    std::uint64_t renderTimeoutMs_ = 50;
    bool wrapFragment_ = true;
    bool clientJsModule_ = false;
    std::string hmrClientPath_;
    bool devModeEnabled_ = false;
    bool devProxyAssetsEnabled_ = false;
    bool devInjectHmrClient_ = false;
    std::string devProxyOrigin_ = "http://127.0.0.1:5174";
    std::string devClientEntryPath_ = "/src/entry-client.tsx";
    std::string devHmrClientPath_ = "/@vite/client";
    std::string devCssPath_ = "/src/styles.css";
    double devProxyTimeoutSec_ = 10.0;
    bool devAutoReloadEnabled_ = false;
    std::string devReloadProbePath_ = "/__hydra/test";
    std::uint64_t devReloadIntervalMs_ = 1000;
    bool apiBridgeEnabled_ = true;
    std::string i18nDefaultLocale_ = "en";
    std::string i18nQueryParam_ = "lang";
    std::string i18nCookieName_ = "hydra_lang";
    bool i18nIncludeLocaleCandidates_ = false;
    std::unordered_set<std::string> i18nSupportedLocales_;
    std::vector<std::string> i18nSupportedLocaleOrder_;
    std::string themeDefault_ = "ocean";
    std::string themeQueryParam_ = "theme";
    std::string themeCookieName_ = "hydra_theme";
    bool themeIncludeThemeCandidates_ = false;
    std::unordered_set<std::string> themeSupportedThemes_;
    std::vector<std::string> themeSupportedThemeOrder_;
    bool requestContextIncludeCookies_ = false;
    bool requestContextIncludeCookieMap_ = false;
    std::unordered_set<std::string> requestContextAllowedCookies_;
    std::unordered_set<std::string> requestContextHeaderAllowlist_;
    std::unordered_set<std::string> requestContextHeaderBlocklist_ = {
        "authorization",
        "proxy-authorization",
        "cookie",
        "set-cookie",
        "x-api-key",
    };
    bool logRequestRoutes_ = false;
    bool logRenderMetrics_ = true;
    HydraSsrPluginConfig normalizedConfig_;
    mutable std::atomic<std::uint64_t> renderCount_{0};
    mutable std::atomic<std::uint64_t> poolTimeoutCount_{0};
    mutable std::atomic<std::uint64_t> renderTimeoutCount_{0};
    mutable std::atomic<std::uint64_t> runtimeRecycleCount_{0};
    mutable std::atomic<std::uint64_t> renderErrorCount_{0};
    mutable std::atomic<std::uint64_t> requestOkCount_{0};
    mutable std::atomic<std::uint64_t> requestFailCount_{0};
    mutable std::atomic<std::uint64_t> totalAcquireWaitUs_{0};
    mutable std::atomic<std::uint64_t> totalRenderUs_{0};
    mutable std::atomic<std::uint64_t> totalWrapUs_{0};
    mutable std::atomic<std::uint64_t> totalRequestUs_{0};
    mutable std::atomic<std::uint64_t> requestIdCounter_{0};
    mutable std::atomic<bool> warnedUnwrappedFragment_{false};
    static constexpr std::size_t kLatencyHistogramBucketCount = 13;
    mutable std::array<std::atomic<std::uint64_t>, kLatencyHistogramBucketCount>
        acquireWaitHistogram_{};
    mutable std::array<std::atomic<std::uint64_t>, kLatencyHistogramBucketCount>
        renderLatencyHistogram_{};
    mutable std::array<std::atomic<std::uint64_t>, kLatencyHistogramBucketCount>
        requestLatencyHistogram_{};
    static constexpr std::size_t kHttpStatusCodeMax = 599;
    mutable std::array<std::atomic<std::uint64_t>, kHttpStatusCodeMax + 1>
        requestCodeCounts_{};
    std::unordered_set<std::string> apiBridgeAllowedMethods_ = {"GET", "POST"};
    std::vector<std::string> apiBridgeAllowedPathPrefixes_ = {"/hydra/internal/"};
    std::size_t apiBridgeMaxBodyBytes_ = 64 * 1024;
    mutable std::mutex apiBridgeMutex_;
    ApiBridgeHandler apiBridgeHandler_;

    std::unique_ptr<V8IsolatePool> isolatePool_;
};

}  // namespace hydra
