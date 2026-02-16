#include "hydra/Config.h"

#include <json/reader.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace hydra {
namespace {

constexpr std::uint64_t kMaxAcquireTimeoutMs = 300000;
constexpr std::uint64_t kMaxRenderTimeoutMs = 120000;
constexpr std::uint64_t kMaxReloadIntervalMs = 600000;
constexpr double kMaxProxyTimeoutSec = 300.0;

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trimAsciiWhitespace(std::string value) {
    const auto isWs = [](unsigned char ch) { return std::isspace(ch) != 0; };

    while (!value.empty() && isWs(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && isWs(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

bool hasHttpScheme(std::string_view value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

HydraAssetMode parseAssetMode(std::string rawMode) {
    rawMode = toLowerCopy(trimAsciiWhitespace(std::move(rawMode)));
    if (rawMode.empty() || rawMode == "auto") {
        return HydraAssetMode::kAuto;
    }
    if (rawMode == "dev") {
        return HydraAssetMode::kDev;
    }
    if (rawMode == "prod") {
        return HydraAssetMode::kProd;
    }
    throw std::runtime_error(
        "HydraSsrPlugin config 'asset_mode' must be one of: auto|dev|prod");
}

bool readNestedBool(const Json::Value *object,
                    const Json::Value &fallbackRoot,
                    const char *nestedKey,
                    const char *fallbackKey,
                    bool fallbackValue) {
    if (object != nullptr && object->isMember(nestedKey)) {
        return (*object)[nestedKey].asBool();
    }
    return fallbackRoot.get(fallbackKey, fallbackValue).asBool();
}

std::string readNestedString(const Json::Value *object,
                             const Json::Value &fallbackRoot,
                             const char *nestedKey,
                             const char *fallbackKey,
                             const std::string &fallbackValue) {
    if (object != nullptr && object->isMember(nestedKey)) {
        return (*object)[nestedKey].asString();
    }
    return fallbackRoot.get(fallbackKey, fallbackValue).asString();
}

double readNestedDouble(const Json::Value *object,
                        const Json::Value &fallbackRoot,
                        const char *nestedKey,
                        const char *fallbackKey,
                        double fallbackValue) {
    if (object != nullptr && object->isMember(nestedKey)) {
        return (*object)[nestedKey].asDouble();
    }
    return fallbackRoot.get(fallbackKey, fallbackValue).asDouble();
}

std::uint64_t readNestedUInt64(const Json::Value *object,
                               const Json::Value &fallbackRoot,
                               const char *nestedKey,
                               const char *fallbackKey,
                               std::uint64_t fallbackValue) {
    if (object != nullptr && object->isMember(nestedKey)) {
        return (*object)[nestedKey].asUInt64();
    }
    return fallbackRoot.get(fallbackKey, fallbackValue).asUInt64();
}

void validateManifestPath(const std::string &manifestPath) {
    if (trimAsciiWhitespace(manifestPath).empty()) {
        throw std::runtime_error("HydraSsrPlugin config 'asset_manifest_path' must be set");
    }

    std::ifstream input(manifestPath);
    if (!input) {
        throw std::runtime_error("HydraSsrPlugin manifest not found: " + manifestPath);
    }

    Json::Value manifest;
    Json::CharReaderBuilder readerBuilder;
    JSONCPP_STRING errors;
    if (!Json::parseFromStream(readerBuilder, input, &manifest, &errors) ||
        !manifest.isObject()) {
        throw std::runtime_error("HydraSsrPlugin manifest parse failed (" + manifestPath +
                                 "): " + errors);
    }
}

}  // namespace

const char *assetModeName(HydraAssetMode mode) {
    switch (mode) {
        case HydraAssetMode::kDev:
            return "dev";
        case HydraAssetMode::kProd:
            return "prod";
        case HydraAssetMode::kAuto:
        default:
            return "auto";
    }
}

HydraSsrPluginConfig validateAndNormalizeHydraSsrPluginConfig(const Json::Value &config) {
    HydraSsrPluginConfig normalized;

    normalized.ssrBundlePath =
        config.get("ssr_bundle_path", normalized.ssrBundlePath).asString();
    normalized.cssPath = config.get("css_path", "").asString();
    normalized.clientJsPath = config.get("client_js_path", "").asString();
    normalized.assetManifestPath = config.get("asset_manifest_path", "").asString();
    if (normalized.assetManifestPath.empty()) {
        normalized.assetManifestPath =
            config.get("manifest_path", normalized.assetManifestPath).asString();
    }
    if (normalized.assetManifestPath.empty()) {
        normalized.assetManifestPath = "./public/assets/manifest.json";
    }
    normalized.assetPublicPrefix =
        config.get("asset_public_prefix", normalized.assetPublicPrefix).asString();
    normalized.clientManifestEntry = config.get("client_manifest_entry", "").asString();
    if (normalized.clientManifestEntry.empty()) {
        normalized.clientManifestEntry =
            config.get("client_entry_key", normalized.clientManifestEntry).asString();
    }
    if (normalized.clientManifestEntry.empty()) {
        normalized.clientManifestEntry = "src/entry-client.tsx";
    }

    normalized.acquireTimeoutMs = config.get("acquire_timeout_ms", 0).asUInt64();
    normalized.renderTimeoutMs = config.get("render_timeout_ms", normalized.renderTimeoutMs).asUInt64();
    normalized.wrapFragment = config.get("wrap_fragment", normalized.wrapFragment).asBool();
    normalized.logRenderMetrics =
        config.get("log_render_metrics", normalized.logRenderMetrics).asBool();

    const Json::Value *devModeConfig =
        config.isMember("dev_mode") && config["dev_mode"].isObject() ? &config["dev_mode"]
                                                                       : nullptr;

    if (devModeConfig != nullptr) {
        static const std::unordered_set<std::string> knownDevKeys = {
            "enabled",
            "proxy_assets",
            "inject_hmr_client",
            "vite_origin",
            "client_entry_path",
            "hmr_client_path",
            "css_path",
            "proxy_timeout_sec",
            "auto_reload",
            "reload_probe_path",
            "reload_interval_ms",
            "asset_mode",
            "log_request_routes",
            "ansi_color_logs",
        };
        for (const auto &key : devModeConfig->getMemberNames()) {
            if (knownDevKeys.find(key) == knownDevKeys.end()) {
                throw std::runtime_error(
                    "HydraSsrPlugin config 'dev_mode." + key + "' is not supported");
            }
        }
    }

    normalized.configuredAssetModeRaw = config.get("asset_mode", "").asString();
    if (normalized.configuredAssetModeRaw.empty() && devModeConfig != nullptr &&
        devModeConfig->isMember("asset_mode")) {
        normalized.configuredAssetModeRaw = (*devModeConfig)["asset_mode"].asString();
    }
    if (trimAsciiWhitespace(normalized.configuredAssetModeRaw).empty()) {
        normalized.configuredAssetModeRaw = "auto";
    }
    normalized.configuredAssetMode = parseAssetMode(normalized.configuredAssetModeRaw);

    const bool legacyDevModeEnabled =
        readNestedBool(devModeConfig, config, "enabled", "dev_mode_enabled", false);
    normalized.devModeEnabled =
        normalized.configuredAssetMode == HydraAssetMode::kAuto
            ? legacyDevModeEnabled
            : (normalized.configuredAssetMode == HydraAssetMode::kDev);
    normalized.resolvedAssetMode = normalized.devModeEnabled ? "dev" : "prod";
    normalized.apiBridgeEnabled =
        config.isMember("api_bridge_enabled")
            ? config["api_bridge_enabled"].asBool()
            : normalized.devModeEnabled;

    const bool hasLogRequestRoutesConfig =
        (devModeConfig != nullptr && devModeConfig->isMember("log_request_routes")) ||
        config.isMember("log_request_routes") ||
        config.isMember("log_requests");
    const bool configuredLogRequestRoutes =
        devModeConfig != nullptr && devModeConfig->isMember("log_request_routes")
            ? (*devModeConfig)["log_request_routes"].asBool()
            : (config.isMember("log_request_routes")
                   ? config["log_request_routes"].asBool()
                   : config.get("log_requests", false).asBool());
    normalized.logRequestRoutes =
        hasLogRequestRoutesConfig ? configuredLogRequestRoutes : normalized.devModeEnabled;

    normalized.devProxyAssetsEnabled =
        readNestedBool(devModeConfig, config, "proxy_assets", "dev_proxy_assets",
                       normalized.devModeEnabled);
    normalized.devInjectHmrClient =
        readNestedBool(devModeConfig, config, "inject_hmr_client", "dev_inject_hmr_client",
                       normalized.devModeEnabled);
    normalized.devProxyOrigin =
        readNestedString(devModeConfig, config, "vite_origin", "dev_proxy_origin",
                         normalized.devProxyOrigin);
    normalized.devClientEntryPath =
        readNestedString(devModeConfig, config, "client_entry_path", "dev_client_entry_path",
                         normalized.devClientEntryPath);
    normalized.devHmrClientPath =
        readNestedString(devModeConfig, config, "hmr_client_path", "dev_hmr_client_path",
                         normalized.devHmrClientPath);
    normalized.devCssPath =
        readNestedString(devModeConfig, config, "css_path", "dev_css_path",
                         normalized.devCssPath);
    normalized.devProxyTimeoutSec =
        readNestedDouble(devModeConfig, config, "proxy_timeout_sec", "dev_proxy_timeout_sec",
                         normalized.devProxyTimeoutSec);
    normalized.devAutoReloadEnabled =
        readNestedBool(devModeConfig, config, "auto_reload", "dev_auto_reload",
                       normalized.devModeEnabled);
    normalized.devReloadProbePath =
        readNestedString(devModeConfig, config, "reload_probe_path", "dev_reload_probe_path",
                         normalized.devReloadProbePath);
    normalized.devReloadIntervalMs =
        readNestedUInt64(devModeConfig, config, "reload_interval_ms", "dev_reload_interval_ms",
                         normalized.devReloadIntervalMs);
    normalized.devAnsiColorLogs =
        readNestedBool(
            devModeConfig, config, "ansi_color_logs", "dev_ansi_color_logs", false);

    if (normalized.acquireTimeoutMs > kMaxAcquireTimeoutMs) {
        throw std::runtime_error("HydraSsrPlugin config 'acquire_timeout_ms' is too large");
    }
    if (normalized.renderTimeoutMs == 0 || normalized.renderTimeoutMs > kMaxRenderTimeoutMs) {
        throw std::runtime_error(
            "HydraSsrPlugin config 'render_timeout_ms' must be in range 1..120000");
    }

    if (normalized.devModeEnabled) {
        if (!hasHttpScheme(trimAsciiWhitespace(normalized.devProxyOrigin))) {
            throw std::runtime_error(
                "HydraSsrPlugin config 'dev_mode.vite_origin' must start with http:// or https://");
        }
        if (trimAsciiWhitespace(normalized.devClientEntryPath).empty()) {
            throw std::runtime_error(
                "HydraSsrPlugin config 'dev_mode.client_entry_path' must be set");
        }
        if (trimAsciiWhitespace(normalized.devCssPath).empty()) {
            throw std::runtime_error("HydraSsrPlugin config 'dev_mode.css_path' must be set");
        }
        if (normalized.devInjectHmrClient &&
            trimAsciiWhitespace(normalized.devHmrClientPath).empty()) {
            throw std::runtime_error(
                "HydraSsrPlugin config 'dev_mode.hmr_client_path' must be set");
        }
        if (normalized.devProxyTimeoutSec <= 0.0 ||
            normalized.devProxyTimeoutSec > kMaxProxyTimeoutSec) {
            throw std::runtime_error(
                "HydraSsrPlugin config 'dev_mode.proxy_timeout_sec' must be in range (0,300]");
        }
        if (normalized.devReloadIntervalMs == 0 ||
            normalized.devReloadIntervalMs > kMaxReloadIntervalMs) {
            throw std::runtime_error(
                "HydraSsrPlugin config 'dev_mode.reload_interval_ms' must be in range 1..600000");
        }
    } else {
        validateManifestPath(normalized.assetManifestPath);
    }

    return normalized;
}

std::string summarizeHydraSsrPluginConfig(const HydraSsrPluginConfig &config) {
    std::ostringstream out;
    out << "runtime{bundle=" << config.ssrBundlePath
        << ", timeout_ms{acquire=" << config.acquireTimeoutMs
        << ", render=" << config.renderTimeoutMs << "}}"
        << " | assets{mode=" << config.resolvedAssetMode
        << ", configured=" << assetModeName(config.configuredAssetMode)
        << ", manifest=" << config.assetManifestPath
        << ", css=" << (config.cssPath.empty() ? "<manifest/dev>" : config.cssPath)
        << ", client=" << (config.clientJsPath.empty() ? "<manifest/dev>" : config.clientJsPath)
        << "}"
        << " | dev{enabled=" << (config.devModeEnabled ? "on" : "off")
        << ", origin=" << config.devProxyOrigin
        << ", proxy_assets=" << (config.devProxyAssetsEnabled ? "on" : "off")
        << ", ansi_color_logs=" << (config.devAnsiColorLogs ? "on" : "off")
        << "}";
    return out.str();
}

}  // namespace hydra
