#pragma once

#include <json/value.h>

#include <cstdint>
#include <string>
#include <vector>

namespace hydra {

enum class HydraAssetMode {
    kAuto,
    kDev,
    kProd,
};

struct HydraSsrPluginConfig {
    std::string ssrBundlePath = "./public/assets/ssr-bundle.js";
    std::string cssPath;
    std::string clientJsPath;
    std::string assetManifestPath = "./public/assets/manifest.json";
    std::string assetPublicPrefix = "/assets";
    std::string clientManifestEntry = "src/entry-client.tsx";
    std::uint64_t acquireTimeoutMs = 0;
    std::uint64_t renderTimeoutMs = 50;
    bool wrapFragment = true;
    bool apiBridgeEnabled = true;
    bool logRenderMetrics = true;
    bool logRequestRoutes = false;

    HydraAssetMode configuredAssetMode = HydraAssetMode::kAuto;
    std::string configuredAssetModeRaw = "auto";
    bool devModeEnabled = false;
    std::string resolvedAssetMode = "prod";

    bool devProxyAssetsEnabled = false;
    bool devInjectHmrClient = false;
    std::string devProxyOrigin = "http://127.0.0.1:5174";
    std::string devClientEntryPath = "/src/entry-client.tsx";
    std::string devHmrClientPath = "/@vite/client";
    std::string devCssPath = "/src/styles.css";
    double devProxyTimeoutSec = 10.0;
    bool devAutoReloadEnabled = false;
    std::string devReloadProbePath = "/__hydra/test";
    std::uint64_t devReloadIntervalMs = 1000;
    bool devAnsiColorLogs = false;

    std::vector<std::string> warnings;
};

[[nodiscard]] const char *assetModeName(HydraAssetMode mode);

// Parses, validates, and normalizes HydraSsrPlugin config.
// Throws std::runtime_error on invalid production/mode-critical values.
[[nodiscard]] HydraSsrPluginConfig validateAndNormalizeHydraSsrPluginConfig(
    const Json::Value &config);

[[nodiscard]] std::string summarizeHydraSsrPluginConfig(
    const HydraSsrPluginConfig &config);

}  // namespace hydra
