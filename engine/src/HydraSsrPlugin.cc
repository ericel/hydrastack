#include "hydra/HydraSsrPlugin.h"

#include "hydra/HtmlShell.h"
#include "hydra/V8IsolatePool.h"
#include "hydra/V8Platform.h"

#include <drogon/drogon.h>
#include <json/reader.h>
#include <json/writer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace hydra {
namespace {

std::string toCompactJson(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    return Json::writeString(builder, value);
}

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

enum class AssetMode {
    kAuto,
    kDev,
    kProd,
};

AssetMode parseAssetMode(std::string rawMode, bool *isValid = nullptr) {
    rawMode = toLowerCopy(trimAsciiWhitespace(std::move(rawMode)));
    if (rawMode.empty() || rawMode == "auto") {
        if (isValid != nullptr) {
            *isValid = true;
        }
        return AssetMode::kAuto;
    }
    if (rawMode == "dev") {
        if (isValid != nullptr) {
            *isValid = true;
        }
        return AssetMode::kDev;
    }
    if (rawMode == "prod") {
        if (isValid != nullptr) {
            *isValid = true;
        }
        return AssetMode::kProd;
    }

    if (isValid != nullptr) {
        *isValid = false;
    }
    return AssetMode::kAuto;
}

const char *assetModeName(AssetMode mode) {
    switch (mode) {
        case AssetMode::kDev:
            return "dev";
        case AssetMode::kProd:
            return "prod";
        case AssetMode::kAuto:
        default:
            return "auto";
    }
}

std::string firstHeaderToken(const std::string &value) {
    if (value.empty()) {
        return {};
    }

    const auto commaPos = value.find(',');
    if (commaPos == std::string::npos) {
        return trimAsciiWhitespace(value);
    }

    return trimAsciiWhitespace(value.substr(0, commaPos));
}

void appendUniqueString(std::vector<std::string> *values, const std::string &value) {
    if (values == nullptr || value.empty()) {
        return;
    }
    if (std::find(values->begin(), values->end(), value) != values->end()) {
        return;
    }
    values->push_back(value);
}

std::string normalizeLocaleTag(std::string locale) {
    locale = trimAsciiWhitespace(std::move(locale));
    if (locale.empty()) {
        return {};
    }

    for (auto &ch : locale) {
        if (ch == '_') {
            ch = '-';
        }
    }

    locale = toLowerCopy(std::move(locale));

    std::string normalized;
    normalized.reserve(locale.size());
    bool previousDash = false;
    for (const auto ch : locale) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch)) {
            normalized.push_back(ch);
            previousDash = false;
            continue;
        }
        if (ch == '-' && !previousDash && !normalized.empty()) {
            normalized.push_back(ch);
            previousDash = true;
        }
    }

    while (!normalized.empty() && normalized.back() == '-') {
        normalized.pop_back();
    }

    return normalized;
}

std::vector<std::string> localeFallbackChain(const std::string &normalizedLocale) {
    std::vector<std::string> chain;
    auto current = normalizedLocale;
    while (!current.empty()) {
        chain.push_back(current);
        const auto separator = current.rfind('-');
        if (separator == std::string::npos) {
            break;
        }
        current = current.substr(0, separator);
    }
    return chain;
}

struct AcceptLanguageItem {
    std::string locale;
    double quality = 1.0;
    std::size_t order = 0;
};

std::vector<std::string> parseAcceptLanguageCandidates(const std::string &headerValue) {
    std::vector<AcceptLanguageItem> parsed;
    std::size_t order = 0;
    std::size_t start = 0;

    while (start <= headerValue.size()) {
        const auto comma = headerValue.find(',', start);
        const auto chunk = comma == std::string::npos
                               ? headerValue.substr(start)
                               : headerValue.substr(start, comma - start);
        const auto token = trimAsciiWhitespace(chunk);
        if (!token.empty()) {
            auto language = token;
            auto quality = 1.0;
            const auto semicolon = token.find(';');
            if (semicolon != std::string::npos) {
                language = trimAsciiWhitespace(token.substr(0, semicolon));
                auto params = token.substr(semicolon + 1);
                std::size_t paramStart = 0;
                while (paramStart <= params.size()) {
                    const auto paramEnd = params.find(';', paramStart);
                    const auto rawParam = paramEnd == std::string::npos
                                              ? params.substr(paramStart)
                                              : params.substr(paramStart, paramEnd - paramStart);
                    const auto param = trimAsciiWhitespace(rawParam);
                    if (!param.empty()) {
                        const auto equals = param.find('=');
                        if (equals != std::string::npos) {
                            const auto key = toLowerCopy(trimAsciiWhitespace(param.substr(0, equals)));
                            const auto value = trimAsciiWhitespace(param.substr(equals + 1));
                            if (key == "q") {
                                try {
                                    quality = std::stod(value);
                                } catch (...) {
                                    quality = 0.0;
                                }
                            }
                        }
                    }
                    if (paramEnd == std::string::npos) {
                        break;
                    }
                    paramStart = paramEnd + 1;
                }
            }

            if (!language.empty() && language != "*" && quality > 0.0) {
                parsed.push_back({language, quality, order++});
            }
        }

        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }

    std::stable_sort(parsed.begin(), parsed.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.quality == rhs.quality) {
            return lhs.order < rhs.order;
        }
        return lhs.quality > rhs.quality;
    });

    std::vector<std::string> orderedLocales;
    orderedLocales.reserve(parsed.size());
    for (const auto &item : parsed) {
        orderedLocales.push_back(item.locale);
    }
    return orderedLocales;
}

void appendNormalizedLocaleArray(const Json::Value &value,
                                 std::unordered_set<std::string> *setOut,
                                 std::vector<std::string> *orderedOut) {
    if (!value.isArray() || setOut == nullptr || orderedOut == nullptr) {
        return;
    }

    for (const auto &item : value) {
        if (!item.isString()) {
            continue;
        }

        const auto locale = normalizeLocaleTag(item.asString());
        if (locale.empty()) {
            continue;
        }

        if (setOut->insert(locale).second) {
            orderedOut->push_back(locale);
        }
    }
}

void appendLowerStringArray(const Json::Value &value,
                            std::unordered_set<std::string> *out) {
    if (out == nullptr || !value.isArray()) {
        return;
    }

    for (const auto &item : value) {
        if (!item.isString()) {
            continue;
        }

        auto key = toLowerCopy(item.asString());
        if (!key.empty()) {
            out->insert(std::move(key));
        }
    }
}

bool parseJsonObject(const std::string &json, Json::Value *out) {
    if (out == nullptr) {
        return false;
    }

    Json::CharReaderBuilder builder;
    JSONCPP_STRING errors;
    std::istringstream stream(json);
    if (!Json::parseFromStream(builder, stream, out, &errors)) {
        return false;
    }

    return out->isObject();
}

bool isLikelyFullDocument(const std::string &html) {
    return html.find("<html") != std::string::npos ||
           html.find("<!doctype") != std::string::npos ||
           html.find("<!DOCTYPE") != std::string::npos;
}

bool containsText(const std::string &value, const std::string &needle) {
    return value.find(needle) != std::string::npos;
}

bool hasHttpScheme(const std::string &value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

std::string normalizeBrowserPath(std::string path) {
    if (path.empty() || hasHttpScheme(path) || path.front() == '/') {
        return path;
    }
    path.insert(path.begin(), '/');
    return path;
}

std::string joinOriginAndPath(std::string origin, std::string path) {
    path = normalizeBrowserPath(std::move(path));
    if (origin.empty()) {
        return path;
    }
    while (!origin.empty() && origin.back() == '/') {
        origin.pop_back();
    }
    if (path.empty()) {
        return origin;
    }
    return origin + path;
}

std::string normalizePublicPrefix(std::string publicPrefix) {
    std::replace(publicPrefix.begin(), publicPrefix.end(), '\\', '/');
    if (publicPrefix.empty()) {
        return "/assets";
    }
    if (publicPrefix.front() != '/') {
        publicPrefix.insert(publicPrefix.begin(), '/');
    }
    while (publicPrefix.size() > 1 && publicPrefix.back() == '/') {
        publicPrefix.pop_back();
    }
    return publicPrefix;
}

std::string toPublicAssetPath(std::string filePath, const std::string &publicPrefix) {
    std::replace(filePath.begin(), filePath.end(), '\\', '/');
    while (filePath.rfind("./", 0) == 0) {
        filePath.erase(0, 2);
    }

    if (filePath.empty()) {
        return {};
    }
    if (filePath.front() == '/') {
        return filePath;
    }
    if (filePath.rfind("assets/", 0) == 0) {
        return "/" + filePath;
    }

    return normalizePublicPrefix(publicPrefix) + "/" + filePath;
}

const Json::Value *findClientEntry(const Json::Value &manifest,
                                   const std::string &clientEntryKey) {
    if (manifest.isMember(clientEntryKey) && manifest[clientEntryKey].isObject()) {
        return &manifest[clientEntryKey];
    }

    const Json::Value *fallback = nullptr;
    for (const auto &key : manifest.getMemberNames()) {
        const auto &entry = manifest[key];
        if (!entry.isObject() || !entry.get("isEntry", false).asBool()) {
            continue;
        }

        const auto file = entry.get("file", "").asString();
        if (file.empty()) {
            continue;
        }

        if (key.find("entry-client") != std::string::npos ||
            file.find("client") != std::string::npos) {
            return &entry;
        }

        if (fallback == nullptr && file.ends_with(".js")) {
            fallback = &entry;
        }
    }

    return fallback;
}

std::optional<HtmlShellAssets> resolveAssetsFromManifest(
    const std::string &manifestPath,
    const std::string &publicPrefix,
    const std::string &clientEntryKey) {
    std::ifstream input(manifestPath);
    if (!input) {
        LOG_WARN << "HydraStack manifest not found: " << manifestPath;
        return std::nullopt;
    }

    Json::Value manifest;
    Json::CharReaderBuilder readerBuilder;
    JSONCPP_STRING errors;
    if (!Json::parseFromStream(readerBuilder, input, &manifest, &errors) ||
        !manifest.isObject()) {
        LOG_WARN << "HydraStack manifest parse failed: " << manifestPath
                 << " error: " << errors;
        return std::nullopt;
    }

    const auto *clientEntry = findClientEntry(manifest, clientEntryKey);
    if (clientEntry == nullptr) {
        LOG_WARN << "HydraStack manifest has no client entry: " << clientEntryKey;
        return std::nullopt;
    }

    HtmlShellAssets assets;
    assets.cssPath.clear();
    assets.clientJsPath.clear();
    assets.clientJsPath =
        toPublicAssetPath(clientEntry->get("file", "").asString(), publicPrefix);

    if (clientEntry->isMember("css") && (*clientEntry)["css"].isArray() &&
        !(*clientEntry)["css"].empty()) {
        assets.cssPath =
            toPublicAssetPath((*clientEntry)["css"][0].asString(), publicPrefix);
    }

    if (assets.cssPath.empty()) {
        if (clientEntry->isMember("imports") && (*clientEntry)["imports"].isArray()) {
            for (const auto &importValue : (*clientEntry)["imports"]) {
                if (!importValue.isString()) {
                    continue;
                }
                const auto importKey = importValue.asString();
                if (!manifest.isMember(importKey) || !manifest[importKey].isObject()) {
                    continue;
                }
                const auto &importEntry = manifest[importKey];
                if (!importEntry.isMember("css") || !importEntry["css"].isArray() ||
                    importEntry["css"].empty()) {
                    continue;
                }

                assets.cssPath =
                    toPublicAssetPath(importEntry["css"][0].asString(), publicPrefix);
                break;
            }
        }
    }

    if (assets.cssPath.empty()) {
        if (manifest.isMember("style.css") && manifest["style.css"].isObject()) {
            const auto file = manifest["style.css"].get("file", "").asString();
            if (!file.empty()) {
                assets.cssPath = toPublicAssetPath(file, publicPrefix);
            }
        }
    }

    if (assets.cssPath.empty()) {
        for (const auto &key : manifest.getMemberNames()) {
            if (!manifest[key].isObject()) {
                continue;
            }

            const auto file = manifest[key].get("file", "").asString();
            if (file.ends_with(".css")) {
                assets.cssPath = toPublicAssetPath(file, publicPrefix);
                break;
            }
        }
    }

    if (assets.clientJsPath.empty()) {
        LOG_WARN << "HydraStack manifest missing JS file for client entry";
        return std::nullopt;
    }

    return assets;
}

}  // namespace

HydraSsrPlugin::~HydraSsrPlugin() = default;

std::string HydraSsrPlugin::buildRouteUrl(const drogon::HttpRequestPtr &req,
                                          const RenderOptions &options) const {
    if (!options.urlOverride.empty()) {
        return options.urlOverride;
    }
    if (!req) {
        return "/";
    }

    std::string routeUrl = req->path().empty() ? "/" : req->path();
    const auto &query = req->query();
    if (!query.empty()) {
        routeUrl.push_back('?');
        routeUrl.append(query);
    }
    return routeUrl;
}

Json::Value HydraSsrPlugin::buildRequestContext(const drogon::HttpRequestPtr &req,
                                                const std::string &routeUrl) const {
    Json::Value context(Json::objectValue);
    context["routeUrl"] = routeUrl;
    context["locale"] = i18nDefaultLocale_;
    if (!req) {
        context["routePath"] = routeUrl;
        context["pathWithQuery"] = routeUrl;
        context["url"] = routeUrl;
        if (i18nIncludeLocaleCandidates_) {
            Json::Value candidates(Json::arrayValue);
            candidates.append(i18nDefaultLocale_);
            context["localeCandidates"] = std::move(candidates);
        }
        return context;
    }

    const std::string routePath = req->path().empty() ? "/" : req->path();
    const auto &query = req->query();
    std::string pathWithQuery = routePath;
    if (!query.empty()) {
        pathWithQuery.push_back('?');
        pathWithQuery.append(query);
    }
    context["routePath"] = routePath;
    context["pathWithQuery"] = pathWithQuery;

    std::string host = firstHeaderToken(req->getHeader("x-forwarded-host"));
    if (host.empty()) {
        host = firstHeaderToken(req->getHeader("host"));
    }

    std::string proto = toLowerCopy(firstHeaderToken(req->getHeader("x-forwarded-proto")));
    if (proto.empty()) {
        proto = "http";
    } else if (proto != "https" && proto != "http") {
        proto = "http";
    }

    if (!host.empty()) {
        context["url"] = proto + "://" + host + pathWithQuery;
    } else {
        context["url"] = pathWithQuery;
    }
    context["path"] = routePath;
    context["query"] = query;
    context["method"] = req->methodString();

    std::vector<std::string> rawLocaleCandidates;
    if (!i18nCookieName_.empty()) {
        const auto cookieLocale = req->getCookie(i18nCookieName_);
        if (!cookieLocale.empty()) {
            rawLocaleCandidates.push_back(cookieLocale);
        }
    }
    if (!i18nQueryParam_.empty()) {
        const auto queryLocale = req->getParameter(i18nQueryParam_);
        if (!queryLocale.empty()) {
            rawLocaleCandidates.push_back(queryLocale);
        }
    }

    const auto acceptLanguageCandidates =
        parseAcceptLanguageCandidates(req->getHeader("accept-language"));
    rawLocaleCandidates.insert(rawLocaleCandidates.end(),
                               acceptLanguageCandidates.begin(),
                               acceptLanguageCandidates.end());
    rawLocaleCandidates.push_back(i18nDefaultLocale_);

    std::vector<std::string> localeCandidates;
    for (const auto &candidate : rawLocaleCandidates) {
        const auto normalized = normalizeLocaleTag(candidate);
        if (normalized.empty()) {
            continue;
        }

        for (const auto &fallbackLocale : localeFallbackChain(normalized)) {
            appendUniqueString(&localeCandidates, fallbackLocale);
        }
    }

    std::string resolvedLocale = i18nDefaultLocale_;
    if (resolvedLocale.empty()) {
        resolvedLocale = "en";
    }
    for (const auto &candidate : localeCandidates) {
        if (i18nSupportedLocales_.empty() ||
            i18nSupportedLocales_.find(candidate) != i18nSupportedLocales_.end()) {
            resolvedLocale = candidate;
            break;
        }
    }
    if (!i18nSupportedLocales_.empty() &&
        i18nSupportedLocales_.find(resolvedLocale) == i18nSupportedLocales_.end() &&
        !i18nSupportedLocaleOrder_.empty()) {
        resolvedLocale = i18nSupportedLocaleOrder_.front();
    }
    context["locale"] = resolvedLocale;
    if (i18nIncludeLocaleCandidates_) {
        Json::Value candidates(Json::arrayValue);
        for (const auto &candidate : localeCandidates) {
            candidates.append(candidate);
        }
        context["localeCandidates"] = std::move(candidates);
    }

    const auto shouldIncludeHeader = [&](const std::string &headerName) {
        const auto normalized = toLowerCopy(headerName);
        if (normalized.rfind("x-forwarded-", 0) == 0) {
            return false;
        }
        if (normalized == "authorization" ||
            normalized == "proxy-authorization" ||
            normalized == "cookie" ||
            normalized == "set-cookie" ||
            normalized == "x-api-key") {
            return false;
        }
        if (!requestContextHeaderAllowlist_.empty() &&
            requestContextHeaderAllowlist_.find(normalized) ==
                requestContextHeaderAllowlist_.end()) {
            return false;
        }
        return requestContextHeaderBlocklist_.find(normalized) ==
               requestContextHeaderBlocklist_.end();
    };

    Json::Value headers(Json::objectValue);
    for (const auto &[headerName, headerValue] : req->getHeaders()) {
        if (!shouldIncludeHeader(headerName)) {
            continue;
        }
        headers[headerName] = headerValue;
    }
    context["headers"] = std::move(headers);

    const auto shouldIncludeCookie = [&](const std::string &cookieName) {
        if (requestContextAllowedCookies_.empty()) {
            return true;
        }
        return requestContextAllowedCookies_.find(toLowerCopy(cookieName)) !=
               requestContextAllowedCookies_.end();
    };

    Json::Value cookieMap(Json::objectValue);
    std::string cookieHeader;
    bool firstCookie = true;
    if (requestContextIncludeCookies_ || requestContextIncludeCookieMap_) {
        for (const auto &[cookieName, cookieValue] : req->getCookies()) {
            if (!shouldIncludeCookie(cookieName)) {
                continue;
            }

            if (requestContextIncludeCookieMap_) {
                cookieMap[cookieName] = cookieValue;
            }

            if (requestContextIncludeCookies_) {
                if (!firstCookie) {
                    cookieHeader.append("; ");
                }
                cookieHeader.append(cookieName);
                cookieHeader.push_back('=');
                cookieHeader.append(cookieValue);
                firstCookie = false;
            }
        }
    }

    if (requestContextIncludeCookies_ &&
        cookieHeader.empty() &&
        requestContextAllowedCookies_.empty()) {
        cookieHeader = req->getHeader("cookie");
    }

    context["cookies"] = requestContextIncludeCookies_ ? cookieHeader : "";
    if (requestContextIncludeCookieMap_) {
        context["cookieMap"] = std::move(cookieMap);
    }
    return context;
}

V8SsrRuntime::BridgeResponse HydraSsrPlugin::dispatchApiBridge(
    const V8SsrRuntime::BridgeRequest &request) const {
    V8SsrRuntime::BridgeResponse response;
    if (!apiBridgeEnabled_) {
        response.status = 503;
        response.body = "Hydra API bridge disabled";
        return response;
    }

    ApiBridgeHandler handler;
    {
        std::lock_guard<std::mutex> lock(apiBridgeMutex_);
        handler = apiBridgeHandler_;
    }

    if (!handler) {
        response.status = 404;
        response.body = "No Hydra API bridge handler registered";
        return response;
    }

    ApiBridgeRequest apiRequest;
    apiRequest.method = request.method;
    apiRequest.path = request.path;
    apiRequest.query = request.query;
    apiRequest.body = request.body;
    apiRequest.headers = request.headers;

    try {
        const auto apiResponse = handler(apiRequest);
        response.status = apiResponse.status;
        response.body = apiResponse.body;
        response.headers = apiResponse.headers;
    } catch (const std::exception &ex) {
        response.status = 500;
        response.body = ex.what();
    } catch (...) {
        response.status = 500;
        response.body = "Unknown Hydra API bridge error";
    }

    return response;
}

void HydraSsrPlugin::registerDevProxyRoutes() {
    if (!devProxyAssetsEnabled_) {
        return;
    }

    const auto forwardProxy =
        [origin = devProxyOrigin_, timeout = devProxyTimeoutSec_](
            const drogon::HttpRequestPtr &req,
            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            drogon::app().forward(req, std::move(callback), origin, timeout);
        };

    // Exact routes for Vite runtime endpoints used by CSS/React refresh.
    drogon::app().registerHandler(
        "/@vite/client",
        forwardProxy,
        {},
        "hydra_dev_proxy_vite_client");
    drogon::app().registerHandler(
        "/@react-refresh",
        forwardProxy,
        {},
        "hydra_dev_proxy_react_refresh");

    const auto registerProxy = [forwardProxy](const std::string &pattern,
                                              const std::string &handlerName) {
        drogon::app().registerHandlerViaRegex(
            pattern,
            forwardProxy,
            {},
            handlerName);
    };

    registerProxy("^/assets/.*$", "hydra_dev_proxy_assets");
    registerProxy("^/@vite/.*$", "hydra_dev_proxy_vite");
    registerProxy("^/%40vite/.*$", "hydra_dev_proxy_vite_encoded");
    registerProxy("^/@id/.*$", "hydra_dev_proxy_id");
    registerProxy("^/@fs/.*$", "hydra_dev_proxy_fs");
    registerProxy("^/%40id/.*$", "hydra_dev_proxy_id_encoded");
    registerProxy("^/%40fs/.*$", "hydra_dev_proxy_fs_encoded");
    registerProxy("^/src/.*$", "hydra_dev_proxy_src");
    registerProxy("^/node_modules/.*$", "hydra_dev_proxy_node_modules");
}

void HydraSsrPlugin::initAndStart(const Json::Value &config) {
    ssrBundlePath_ = config.get("ssr_bundle_path", "./public/assets/ssr-bundle.js").asString();
    cssPath_ = config.get("css_path", "").asString();
    clientJsPath_ = config.get("client_js_path", "").asString();
    assetManifestPath_ = config.get("asset_manifest_path", "").asString();
    if (assetManifestPath_.empty()) {
        assetManifestPath_ = config.get("manifest_path", "./public/assets/manifest.json").asString();
    }
    assetPublicPrefix_ = config.get("asset_public_prefix", "/assets").asString();
    clientManifestEntry_ = config.get("client_manifest_entry", "").asString();
    if (clientManifestEntry_.empty()) {
        clientManifestEntry_ = config.get("client_entry_key", "src/entry-client.tsx").asString();
    }
    isolateAcquireTimeoutMs_ = config.get("acquire_timeout_ms", 0).asUInt64();
    renderTimeoutMs_ = config.get("render_timeout_ms", 50).asUInt64();
    wrapFragment_ = config.get("wrap_fragment", true).asBool();
    apiBridgeEnabled_ = config.get("api_bridge_enabled", true).asBool();
    logRenderMetrics_ = config.get("log_render_metrics", true).asBool();
    const Json::Value *i18nConfig =
        config.isMember("i18n") && config["i18n"].isObject() ? &config["i18n"] : nullptr;
    const Json::Value *requestContextConfig =
        config.isMember("request_context") && config["request_context"].isObject()
            ? &config["request_context"]
            : nullptr;

    const Json::Value *devModeConfig =
        config.isMember("dev_mode") && config["dev_mode"].isObject() ? &config["dev_mode"]
                                                                       : nullptr;

    auto readDevBool = [&](const char *nestedKey,
                           const char *topLevelKey,
                           bool fallback) -> bool {
        if (devModeConfig && devModeConfig->isMember(nestedKey)) {
            return (*devModeConfig)[nestedKey].asBool();
        }
        return config.get(topLevelKey, fallback).asBool();
    };
    auto readDevString = [&](const char *nestedKey,
                             const char *topLevelKey,
                             const std::string &fallback) -> std::string {
        if (devModeConfig && devModeConfig->isMember(nestedKey)) {
            return (*devModeConfig)[nestedKey].asString();
        }
        return config.get(topLevelKey, fallback).asString();
    };
    auto readDevDouble = [&](const char *nestedKey,
                             const char *topLevelKey,
                             double fallback) -> double {
        if (devModeConfig && devModeConfig->isMember(nestedKey)) {
            return (*devModeConfig)[nestedKey].asDouble();
        }
        return config.get(topLevelKey, fallback).asDouble();
    };
    auto readDevUInt64 = [&](const char *nestedKey,
                             const char *topLevelKey,
                             std::uint64_t fallback) -> std::uint64_t {
        if (devModeConfig && devModeConfig->isMember(nestedKey)) {
            return (*devModeConfig)[nestedKey].asUInt64();
        }
        return config.get(topLevelKey, fallback).asUInt64();
    };
    auto readRequestContextBool = [&](const char *nestedKey,
                                      const char *topLevelKey,
                                      bool fallback) -> bool {
        if (requestContextConfig && requestContextConfig->isMember(nestedKey)) {
            return (*requestContextConfig)[nestedKey].asBool();
        }
        return config.get(topLevelKey, fallback).asBool();
    };
    auto readI18nString = [&](const char *nestedKey,
                              const char *topLevelKey,
                              const std::string &fallback) -> std::string {
        if (i18nConfig && i18nConfig->isMember(nestedKey)) {
            return (*i18nConfig)[nestedKey].asString();
        }
        return config.get(topLevelKey, fallback).asString();
    };
    auto readI18nBool = [&](const char *nestedKey,
                            const char *topLevelKey,
                            bool fallback) -> bool {
        if (i18nConfig && i18nConfig->isMember(nestedKey)) {
            return (*i18nConfig)[nestedKey].asBool();
        }
        return config.get(topLevelKey, fallback).asBool();
    };
    auto appendRequestContextArray = [&](const char *nestedKey,
                                         const char *topLevelKey,
                                         std::unordered_set<std::string> *out) {
        if (out == nullptr) {
            return;
        }
        if (requestContextConfig && requestContextConfig->isMember(nestedKey)) {
            appendLowerStringArray((*requestContextConfig)[nestedKey], out);
            return;
        }
        if (config.isMember(topLevelKey)) {
            appendLowerStringArray(config[topLevelKey], out);
        }
    };
    auto appendI18nLocaleArray = [&](const char *nestedKey, const char *topLevelKey) {
        if (i18nConfig && i18nConfig->isMember(nestedKey)) {
            appendNormalizedLocaleArray(
                (*i18nConfig)[nestedKey], &i18nSupportedLocales_, &i18nSupportedLocaleOrder_);
            return;
        }
        if (config.isMember(topLevelKey)) {
            appendNormalizedLocaleArray(
                config[topLevelKey], &i18nSupportedLocales_, &i18nSupportedLocaleOrder_);
        }
    };

    std::string configuredAssetModeRaw = config.get("asset_mode", "").asString();
    if (configuredAssetModeRaw.empty() && devModeConfig != nullptr &&
        devModeConfig->isMember("asset_mode")) {
        configuredAssetModeRaw = (*devModeConfig)["asset_mode"].asString();
    }
    bool assetModeValueValid = true;
    const auto configuredAssetMode = parseAssetMode(configuredAssetModeRaw, &assetModeValueValid);
    if (!assetModeValueValid) {
        LOG_WARN << "HydraStack invalid asset_mode='" << configuredAssetModeRaw
                 << "', expected one of: auto|dev|prod. Falling back to auto.";
    }

    const bool legacyDevModeEnabled = readDevBool("enabled", "dev_mode_enabled", false);
    devModeEnabled_ = configuredAssetMode == AssetMode::kAuto
                          ? legacyDevModeEnabled
                          : (configuredAssetMode == AssetMode::kDev);
    const auto resolvedAssetMode = devModeEnabled_ ? "dev" : "prod";

    devProxyAssetsEnabled_ = readDevBool("proxy_assets", "dev_proxy_assets", devModeEnabled_);
    devInjectHmrClient_ =
        readDevBool("inject_hmr_client", "dev_inject_hmr_client", devModeEnabled_);
    devProxyOrigin_ =
        readDevString("vite_origin", "dev_proxy_origin", "http://127.0.0.1:5174");
    devClientEntryPath_ =
        readDevString("client_entry_path", "dev_client_entry_path", "/src/entry-client.tsx");
    devHmrClientPath_ =
        readDevString("hmr_client_path", "dev_hmr_client_path", "/@vite/client");
    devCssPath_ =
        readDevString("css_path", "dev_css_path", "/src/styles.css");
    devProxyTimeoutSec_ =
        readDevDouble("proxy_timeout_sec", "dev_proxy_timeout_sec", 10.0);
    devAutoReloadEnabled_ =
        readDevBool("auto_reload", "dev_auto_reload", devModeEnabled_);
    devReloadProbePath_ =
        readDevString("reload_probe_path", "dev_reload_probe_path", "/__hydra/test");
    devReloadIntervalMs_ =
        readDevUInt64("reload_interval_ms", "dev_reload_interval_ms", 1000);
    i18nDefaultLocale_ =
        normalizeLocaleTag(readI18nString("defaultLocale", "i18n_default_locale", "en"));
    if (i18nDefaultLocale_.empty()) {
        i18nDefaultLocale_ = "en";
    }
    i18nQueryParam_ = trimAsciiWhitespace(
        readI18nString("queryParam", "i18n_query_param", "lang"));
    if (i18nQueryParam_.empty()) {
        i18nQueryParam_ = "lang";
    }
    i18nCookieName_ = trimAsciiWhitespace(
        readI18nString("cookieName", "i18n_cookie_name", "hydra_lang"));
    if (i18nCookieName_.empty()) {
        i18nCookieName_ = "hydra_lang";
    }
    i18nIncludeLocaleCandidates_ = readI18nBool(
        "includeLocaleCandidates",
        "i18n_include_locale_candidates",
        false);
    i18nIncludeLocaleCandidates_ = readI18nBool(
        "include_locale_candidates",
        "i18n_includeLocaleCandidates",
        i18nIncludeLocaleCandidates_);
    i18nSupportedLocales_.clear();
    i18nSupportedLocaleOrder_.clear();
    appendI18nLocaleArray("supportedLocales", "i18n_supported_locales");
    appendI18nLocaleArray("supported_locales", "i18n_supportedLocales");
    if (i18nSupportedLocales_.empty()) {
        i18nSupportedLocales_.insert(i18nDefaultLocale_);
        i18nSupportedLocaleOrder_.push_back(i18nDefaultLocale_);
    } else if (i18nSupportedLocales_.find(i18nDefaultLocale_) ==
               i18nSupportedLocales_.end()) {
        i18nSupportedLocales_.insert(i18nDefaultLocale_);
        i18nSupportedLocaleOrder_.push_back(i18nDefaultLocale_);
    }

    requestContextIncludeCookies_ = readRequestContextBool(
        "include_cookies", "request_context_include_cookies", false);
    requestContextIncludeCookieMap_ = readRequestContextBool(
        "includeCookieMap",
        "request_context_includeCookieMap",
        requestContextIncludeCookies_);
    requestContextIncludeCookieMap_ = readRequestContextBool(
        "include_cookie_map",
        "request_context_include_cookie_map",
        requestContextIncludeCookieMap_);

    requestContextAllowedCookies_.clear();
    appendRequestContextArray(
        "allowed_cookies", "request_context_allowed_cookies", &requestContextAllowedCookies_);

    requestContextHeaderAllowlist_.clear();
    appendRequestContextArray(
        "include_headers", "request_context_include_headers", &requestContextHeaderAllowlist_);
    appendRequestContextArray(
        "include_header_allowlist",
        "request_context_include_header_allowlist",
        &requestContextHeaderAllowlist_);

    requestContextHeaderBlocklist_ = {
        "authorization",
        "proxy-authorization",
        "cookie",
        "set-cookie",
        "x-api-key",
    };
    appendRequestContextArray(
        "exclude_headers", "request_context_exclude_headers", &requestContextHeaderBlocklist_);
    appendRequestContextArray(
        "include_header_blocklist",
        "request_context_include_header_blocklist",
        &requestContextHeaderBlocklist_);

    {
        std::lock_guard<std::mutex> lock(apiBridgeMutex_);
        if (!apiBridgeHandler_) {
            apiBridgeHandler_ = [](const ApiBridgeRequest &request) {
                ApiBridgeResponse response;
                if (request.path == "/hydra/internal/health") {
                    response.status = 200;
                    response.body = "ok";
                } else if (request.path == "/hydra/internal/echo") {
                    response.status = 200;
                    response.body = request.body;
                } else {
                    response.status = 404;
                    response.body = "No internal handler for " + request.path;
                }
                return response;
            };
        }
    }

    clientJsModule_ = false;
    hmrClientPath_.clear();

    if (auto manifestAssets = resolveAssetsFromManifest(
            assetManifestPath_, assetPublicPrefix_, clientManifestEntry_)) {
        if (cssPath_.empty()) {
            cssPath_ = manifestAssets->cssPath;
        }
        if (clientJsPath_.empty()) {
            clientJsPath_ = manifestAssets->clientJsPath;
        }
    }

    if (devModeEnabled_) {
        cssPath_ = devProxyAssetsEnabled_
                       ? normalizeBrowserPath(devCssPath_)
                       : joinOriginAndPath(devProxyOrigin_, devCssPath_);
        clientJsModule_ = true;

        if (devProxyAssetsEnabled_) {
            clientJsPath_ = normalizeBrowserPath(devClientEntryPath_);
            if (devInjectHmrClient_) {
                hmrClientPath_ = normalizeBrowserPath(devHmrClientPath_);
            }
            registerDevProxyRoutes();
        } else {
            clientJsPath_ = joinOriginAndPath(devProxyOrigin_, devClientEntryPath_);
            if (devInjectHmrClient_) {
                hmrClientPath_ = joinOriginAndPath(devProxyOrigin_, devHmrClientPath_);
            }
        }
    } else {
        if (cssPath_.empty()) {
            cssPath_ = "/assets/app.css";
            LOG_WARN << "HydraStack falling back to default css path: " << cssPath_;
        }
        if (clientJsPath_.empty()) {
            clientJsPath_ = "/assets/client.js";
            LOG_WARN << "HydraStack falling back to default client path: " << clientJsPath_;
        }
    }

    const auto configuredPoolSize = config.isMember("pool_size")
                                        ? config["pool_size"].asUInt()
                                        : config.get("isolate_pool_size", 0).asUInt();
    const auto threadCount = std::max<std::size_t>(1, drogon::app().getThreadNum());
    isolatePoolSize_ =
        configuredPoolSize > 0 ? static_cast<std::size_t>(configuredPoolSize) : threadCount;

    V8Platform::initialize();
    try {
        isolatePool_ = std::make_unique<V8IsolatePool>(
            isolatePoolSize_,
            ssrBundlePath_,
            renderTimeoutMs_,
            [this](const V8SsrRuntime::BridgeRequest &request) {
                return dispatchApiBridge(request);
            });
    } catch (...) {
        V8Platform::shutdown();
        throw;
    }

    LOG_INFO << "HydraSsrPlugin initialized with bundle: " << ssrBundlePath_
             << ", pool size: " << isolatePoolSize_
             << ", acquire timeout(ms): " << isolateAcquireTimeoutMs_
             << ", css: " << cssPath_
             << ", client: " << clientJsPath_
             << ", asset mode: " << resolvedAssetMode
             << ", configured asset_mode: " << assetModeName(configuredAssetMode)
             << ", dev mode: " << (devModeEnabled_ ? "on" : "off")
             << ", api bridge: " << (apiBridgeEnabled_ ? "on" : "off")
             << ", locale default: " << i18nDefaultLocale_
             << ", include cookies: " << (requestContextIncludeCookies_ ? "on" : "off")
             << ", include cookieMap: "
             << (requestContextIncludeCookieMap_ ? "on" : "off");
}

void HydraSsrPlugin::setApiBridgeHandler(ApiBridgeHandler handler) {
    std::lock_guard<std::mutex> lock(apiBridgeMutex_);
    apiBridgeHandler_ = std::move(handler);
}

void HydraSsrPlugin::shutdown() {
    isolatePool_.reset();
    V8Platform::shutdown();
}

std::string HydraSsrPlugin::render(const drogon::HttpRequestPtr &req,
                                   const Json::Value &props,
                                   const RenderOptions &options) const {
    return render(req, toCompactJson(props), options);
}

std::string HydraSsrPlugin::render(const drogon::HttpRequestPtr &req,
                                   const std::string &propsJson,
                                   const RenderOptions &options) const {
    if (!isolatePool_) {
        return HtmlShell::errorPage("HydraSsrPlugin is not initialized");
    }

    const auto routeUrl = buildRouteUrl(req, options);
    const auto requestContext = buildRequestContext(req, routeUrl);
    const auto requestContextJson = toCompactJson(requestContext);
    std::string effectivePropsJson = propsJson;
    Json::Value propsObject;
    if (parseJsonObject(propsJson, &propsObject)) {
        propsObject["__hydra_request"] = requestContext;
        effectivePropsJson = toCompactJson(propsObject);
    }
    const auto acquireStartedAt = std::chrono::steady_clock::now();
    std::uint64_t acquireWaitMs = 0;

    try {
        auto lease = isolatePool_->acquire(isolateAcquireTimeoutMs_);
        acquireWaitMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - acquireStartedAt)
                .count());

        try {
            const auto renderStartedAt = std::chrono::steady_clock::now();
            auto html =
                lease->render(
                    routeUrl,
                    effectivePropsJson,
                    requestContextJson,
                    isolatePool_->renderTimeoutMs());
            const auto renderMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - renderStartedAt)
                    .count());
            const auto renderIndex =
                renderCount_.fetch_add(1, std::memory_order_relaxed) + 1;
            std::uint64_t wrapMs = 0;

            if (wrapFragment_ && !isLikelyFullDocument(html)) {
                HtmlShellAssets assets;
                assets.cssPath = cssPath_;
                assets.clientJsPath = clientJsPath_;
                assets.hmrClientPath = hmrClientPath_;
                assets.clientJsModule = clientJsModule_;
                if (devModeEnabled_ && devAutoReloadEnabled_) {
                    assets.devReloadProbePath = normalizeBrowserPath(devReloadProbePath_);
                    assets.devReloadIntervalMs = devReloadIntervalMs_;
                }
                const auto wrapStartedAt = std::chrono::steady_clock::now();
                auto wrappedHtml = HtmlShell::wrap(
                    html,
                    effectivePropsJson,
                    assets);
                wrapMs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - wrapStartedAt)
                        .count());
                if (logRenderMetrics_) {
                    LOG_INFO << "HydraMetrics render_ok count=" << renderIndex
                             << " route=" << routeUrl
                             << " acquire_wait_ms=" << acquireWaitMs
                             << " render_ms=" << renderMs
                             << " wrap_ms=" << wrapMs
                             << " pool_timeouts="
                             << poolTimeoutCount_.load(std::memory_order_relaxed)
                             << " render_timeouts="
                             << renderTimeoutCount_.load(std::memory_order_relaxed)
                             << " runtime_recycles="
                             << runtimeRecycleCount_.load(std::memory_order_relaxed);
                }
                return wrappedHtml;
            }

            if (logRenderMetrics_) {
                LOG_INFO << "HydraMetrics render_ok count=" << renderIndex
                         << " route=" << routeUrl
                         << " acquire_wait_ms=" << acquireWaitMs
                         << " render_ms=" << renderMs
                         << " wrap_ms=" << wrapMs
                         << " pool_timeouts="
                         << poolTimeoutCount_.load(std::memory_order_relaxed)
                         << " render_timeouts="
                         << renderTimeoutCount_.load(std::memory_order_relaxed)
                         << " runtime_recycles="
                         << runtimeRecycleCount_.load(std::memory_order_relaxed);
            }

            return html;
        } catch (const std::exception &renderEx) {
            lease.markForRecycle();
            runtimeRecycleCount_.fetch_add(1, std::memory_order_relaxed);
            const std::string message = renderEx.what();
            if (containsText(message, "SSR render exceeded timeout")) {
                renderTimeoutCount_.fetch_add(1, std::memory_order_relaxed);
            }
            throw;
        } catch (...) {
            lease.markForRecycle();
            runtimeRecycleCount_.fetch_add(1, std::memory_order_relaxed);
            throw;
        }
    } catch (const std::exception &ex) {
        const std::string message = ex.what();
        if (containsText(message, "Timed out waiting for available V8 isolate")) {
            poolTimeoutCount_.fetch_add(1, std::memory_order_relaxed);
        }
        if (containsText(message, "SSR render exceeded timeout")) {
            renderTimeoutCount_.fetch_add(1, std::memory_order_relaxed);
        }
        if (logRenderMetrics_) {
            LOG_WARN << "HydraMetrics render_fail route=" << routeUrl
                     << " acquire_wait_ms=" << acquireWaitMs
                     << " wrap_ms=0"
                     << " pool_timeouts="
                     << poolTimeoutCount_.load(std::memory_order_relaxed)
                     << " render_timeouts="
                     << renderTimeoutCount_.load(std::memory_order_relaxed)
                     << " runtime_recycles="
                     << runtimeRecycleCount_.load(std::memory_order_relaxed)
                     << " error=\"" << message << "\"";
        }
        LOG_ERROR << "HydraStack render failed for url=" << routeUrl << ": " << ex.what();
        return HtmlShell::errorPage(ex.what());
    } catch (...) {
        if (logRenderMetrics_) {
            LOG_WARN << "HydraMetrics render_fail route=" << routeUrl
                     << " acquire_wait_ms=" << acquireWaitMs
                     << " wrap_ms=0"
                     << " pool_timeouts="
                     << poolTimeoutCount_.load(std::memory_order_relaxed)
                     << " render_timeouts="
                     << renderTimeoutCount_.load(std::memory_order_relaxed)
                     << " runtime_recycles="
                     << runtimeRecycleCount_.load(std::memory_order_relaxed)
                     << " error=\"unknown\"";
        }
        LOG_ERROR << "HydraStack render failed for url=" << routeUrl
                  << ": unknown exception";
        return HtmlShell::errorPage("Unknown SSR runtime error");
    }
}

}  // namespace hydra
