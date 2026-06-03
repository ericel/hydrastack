#include "hydra/HydraSsrPlugin.h"

#include "hydra/HtmlShell.h"

#include <json/reader.h>
#include <json/writer.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace hydra {
namespace {

std::string trimCopy(std::string value) {
    const auto isNotSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), isNotSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), isNotSpace).base(),
                value.end());
    return value;
}

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string normalizeLocale(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        if (ch == '_') {
            return '-';
        }
        return static_cast<char>(std::tolower(ch));
    });
    return trimCopy(std::move(value));
}

std::string normalizeTheme(std::string value) {
    return toLowerCopy(trimCopy(std::move(value)));
}

std::string readStringOrDefault(const Json::Value &node,
                                const char *key,
                                std::string fallback) {
    if (!node.isObject() || !node.isMember(key) || !node[key].isString()) {
        return fallback;
    }
    const std::string value = trimCopy(node[key].asString());
    return value.empty() ? fallback : value;
}

bool readBoolOrDefault(const Json::Value &node, const char *key, bool fallback) {
    if (!node.isObject() || !node.isMember(key) || !node[key].isBool()) {
        return fallback;
    }
    return node[key].asBool();
}

std::vector<std::string> readStringArray(const Json::Value &node,
                                         const char *key,
                                         const std::vector<std::string> &fallback,
                                         bool normalizeAsLocale) {
    if (!node.isObject() || !node.isMember(key) || !node[key].isArray()) {
        return fallback;
    }

    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto &entry : node[key]) {
        if (!entry.isString()) {
            continue;
        }
        std::string value = trimCopy(entry.asString());
        value = normalizeAsLocale ? normalizeLocale(std::move(value))
                                  : normalizeTheme(std::move(value));
        if (value.empty() || seen.contains(value)) {
            continue;
        }
        seen.insert(value);
        out.push_back(std::move(value));
    }
    return out.empty() ? fallback : out;
}

std::string compactJson(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    writer["commentStyle"] = "None";
    return Json::writeString(writer, value);
}

bool parseJsonObject(const std::string &json, Json::Value *out) {
    if (out == nullptr || json.empty()) {
        return false;
    }

    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(json);
    Json::Value parsed;
    if (!Json::parseFromStream(builder, stream, &parsed, &errors) || !parsed.isObject()) {
        return false;
    }
    *out = std::move(parsed);
    return true;
}

std::string joinUrlPath(std::string_view prefix, std::string_view file) {
    std::string out(prefix);
    if (out.empty() || out.back() != '/') {
        out.push_back('/');
    }
    if (!file.empty() && file.front() == '/') {
        out.append(file.begin() + 1, file.end());
    } else {
        out.append(file.begin(), file.end());
    }
    return out;
}

std::string buildAbsoluteRequestUrl(const drogon::HttpRequestPtr &req,
                                    const std::string &routeUrl) {
    if (!req) {
        return routeUrl.empty() ? "/" : routeUrl;
    }

    const std::string forwardedProto = trimCopy(req->getHeader("x-forwarded-proto"));
    const std::string scheme = forwardedProto.empty() ? "http" : forwardedProto;
    const std::string host = trimCopy(req->getHeader("host"));
    if (host.empty()) {
        return routeUrl.empty() ? "/" : routeUrl;
    }
    return scheme + "://" + host + (routeUrl.empty() ? "/" : routeUrl);
}

void appendUnique(std::vector<std::string> *values, const std::string &value) {
    if (values == nullptr || value.empty()) {
        return;
    }
    if (std::find(values->begin(), values->end(), value) == values->end()) {
        values->push_back(value);
    }
}

}  // namespace

void V8IsolatePoolDeleter::operator()(V8IsolatePool *) const noexcept {}

HydraSsrPlugin::~HydraSsrPlugin() = default;

void HydraSsrPlugin::initAndStart(const Json::Value &config) {
    shellTitle_ = readStringOrDefault(config, "shell_title", shellTitle_);

    const auto &shellMeta = config["shell_meta"];
    shellDescription_ = readStringOrDefault(shellMeta, "description", shellDescription_);
    shellSiteName_ = readStringOrDefault(shellMeta, "site_name", shellSiteName_);
    shellOgType_ = readStringOrDefault(shellMeta, "og_type", shellOgType_);
    shellImageUrl_ = readStringOrDefault(shellMeta, "image_url", shellImageUrl_);
    shellTwitterCard_ = readStringOrDefault(shellMeta, "twitter_card", shellTwitterCard_);
    shellCanonicalUrl_ = readStringOrDefault(shellMeta, "canonical_url", shellCanonicalUrl_);
    shellRobots_ = readStringOrDefault(shellMeta, "robots", shellRobots_);

    assetManifestPath_ =
        readStringOrDefault(config, "asset_manifest_path", assetManifestPath_);
    assetPublicPrefix_ =
        readStringOrDefault(config, "asset_public_prefix", assetPublicPrefix_);
    clientManifestEntry_ =
        readStringOrDefault(config, "client_manifest_entry", clientManifestEntry_);

    const auto &i18n = config["i18n"];
    i18nDefaultLocale_ =
        normalizeLocale(readStringOrDefault(i18n, "defaultLocale", i18nDefaultLocale_));
    i18nQueryParam_ = readStringOrDefault(i18n, "queryParam", i18nQueryParam_);
    i18nCookieName_ = readStringOrDefault(i18n, "cookieName", i18nCookieName_);
    i18nIncludeLocaleCandidates_ =
        readBoolOrDefault(i18n, "includeLocaleCandidates", i18nIncludeLocaleCandidates_);
    i18nSupportedLocaleOrder_ =
        readStringArray(i18n, "supportedLocales", {i18nDefaultLocale_}, true);
    i18nSupportedLocales_.clear();
    for (const auto &locale : i18nSupportedLocaleOrder_) {
        i18nSupportedLocales_.insert(locale);
    }

    const auto &theme = config["theme"];
    themeDefault_ = normalizeTheme(readStringOrDefault(theme, "defaultTheme", themeDefault_));
    themeQueryParam_ = readStringOrDefault(theme, "queryParam", themeQueryParam_);
    themeCookieName_ = readStringOrDefault(theme, "cookieName", themeCookieName_);
    themeIncludeThemeCandidates_ =
        readBoolOrDefault(theme, "includeThemeCandidates", themeIncludeThemeCandidates_);
    themeSupportedThemeOrder_ =
        readStringArray(theme, "supportedThemes", {themeDefault_}, false);
    themeSupportedThemes_.clear();
    for (const auto &themeName : themeSupportedThemeOrder_) {
        themeSupportedThemes_.insert(themeName);
    }

    Json::Value manifest(Json::objectValue);
    {
        std::ifstream manifestStream(assetManifestPath_);
        if (manifestStream.is_open()) {
            Json::CharReaderBuilder builder;
            std::string errors;
            Json::parseFromStream(builder, manifestStream, &manifest, &errors);
        }
    }

    const auto &clientEntry = manifest[clientManifestEntry_];
    if (clientEntry.isObject()) {
        if (clientEntry["file"].isString()) {
            clientJsPath_ = joinUrlPath(assetPublicPrefix_, clientEntry["file"].asString());
        }
        clientJsModule_ = true;
    }

    const auto &styleEntry = manifest["style.css"];
    if (styleEntry.isObject() && styleEntry["file"].isString()) {
        cssPath_ = joinUrlPath(assetPublicPrefix_, styleEntry["file"].asString());
    }

    if (!clientEntry.isObject()) {
        const auto &devMode = config["dev_mode"];
        clientJsPath_ = readStringOrDefault(devMode, "client_entry_path", clientJsPath_);
        cssPath_ = readStringOrDefault(devMode, "css_path", cssPath_);
        hmrClientPath_ = readStringOrDefault(devMode, "hmr_client_path", hmrClientPath_);
        clientJsModule_ = readBoolOrDefault(devMode, "client_js_module", true);
    }
}

void HydraSsrPlugin::shutdown() {}

std::string HydraSsrPlugin::buildRouteUrl(const drogon::HttpRequestPtr &req,
                                          const RenderOptions &options) const {
    if (!options.urlOverride.empty()) {
        return options.urlOverride;
    }
    if (!req) {
        return "/";
    }

    const std::string path = req->path().empty() ? "/" : req->path();
    if (req->query().empty()) {
        return path;
    }
    return path + "?" + req->query();
}

Json::Value HydraSsrPlugin::buildRequestContext(const drogon::HttpRequestPtr &req,
                                                const std::string &routeUrl,
                                                const std::string &requestId) const {
    Json::Value context(Json::objectValue);

    const std::string routePath = req && !req->path().empty() ? req->path() : "/";
    context["routePath"] = routePath;
    context["routeUrl"] = routeUrl.empty() ? routePath : routeUrl;
    context["url"] = buildAbsoluteRequestUrl(req, context["routeUrl"].asString());
    context["path"] = routePath;
    context["query"] = req ? req->query() : "";
    context["method"] = req ? req->methodString() : "GET";
    context["requestId"] = requestId;

    std::vector<std::string> localeCandidates;
    if (req) {
        appendUnique(&localeCandidates, normalizeLocale(req->getParameter(i18nQueryParam_)));
        appendUnique(&localeCandidates, normalizeLocale(req->getCookie(i18nCookieName_)));
    }
    appendUnique(&localeCandidates, i18nDefaultLocale_);

    std::string locale = i18nDefaultLocale_.empty() ? "en" : i18nDefaultLocale_;
    for (const auto &candidate : localeCandidates) {
        if (i18nSupportedLocales_.empty() || i18nSupportedLocales_.contains(candidate)) {
            locale = candidate;
            break;
        }
    }
    context["locale"] = locale;

    if (i18nIncludeLocaleCandidates_) {
        Json::Value candidates(Json::arrayValue);
        for (const auto &candidate : localeCandidates) {
            candidates.append(candidate);
            const auto dashPos = candidate.find('-');
            if (dashPos != std::string::npos) {
                const auto base = candidate.substr(0, dashPos);
                if (!base.empty() && base != candidate) {
                    candidates.append(base);
                }
            }
        }
        context["localeCandidates"] = std::move(candidates);
    }

    std::vector<std::string> themeCandidates;
    if (req) {
        appendUnique(&themeCandidates, normalizeTheme(req->getParameter(themeQueryParam_)));
        appendUnique(&themeCandidates, normalizeTheme(req->getCookie(themeCookieName_)));
    }
    appendUnique(&themeCandidates, themeDefault_);

    std::string theme = themeDefault_.empty() ? "default" : themeDefault_;
    for (const auto &candidate : themeCandidates) {
        if (themeSupportedThemes_.empty() || themeSupportedThemes_.contains(candidate)) {
            theme = candidate;
            break;
        }
    }
    context["theme"] = theme;

    if (themeIncludeThemeCandidates_) {
        Json::Value candidates(Json::arrayValue);
        for (const auto &candidate : themeCandidates) {
            candidates.append(candidate);
        }
        context["themeCandidates"] = std::move(candidates);
    }

    return context;
}

std::string HydraSsrPlugin::resolveRequestId(const drogon::HttpRequestPtr &req) const {
    if (req) {
        const auto headerRequestId = trimCopy(req->getHeader("x-request-id"));
        if (!headerRequestId.empty()) {
            return headerRequestId;
        }
    }
    const auto generated = requestIdCounter_.fetch_add(1, std::memory_order_relaxed) + 1;
    return "hydra-" + std::to_string(generated);
}

std::string HydraSsrPlugin::render(const drogon::HttpRequestPtr &req,
                                   const Json::Value &props,
                                   const RenderOptions &options) const {
    return renderResult(req, props, options).html;
}

std::string HydraSsrPlugin::render(const drogon::HttpRequestPtr &req,
                                   const std::string &propsJson,
                                   const RenderOptions &options) const {
    return renderResult(req, propsJson, options).html;
}

SsrRenderResult HydraSsrPlugin::renderResult(const drogon::HttpRequestPtr &req,
                                             const Json::Value &props,
                                             const RenderOptions &options) const {
    return renderResult(req, compactJson(props), options);
}

SsrRenderResult HydraSsrPlugin::renderResult(const drogon::HttpRequestPtr &req,
                                             const std::string &propsJson,
                                             const RenderOptions &options) const {
    const auto routeUrl = buildRouteUrl(req, options);
    const auto requestId = resolveRequestId(req);
    const auto requestContext = buildRequestContext(req, routeUrl, requestId);

    Json::Value propsObject(Json::objectValue);
    if (!parseJsonObject(propsJson, &propsObject)) {
        propsObject = Json::Value(Json::objectValue);
    }
    propsObject["__hydra_request"] = requestContext;
    const auto effectivePropsJson = compactJson(propsObject);

    HtmlShellAssets assets;
    assets.title = shellTitle_;
    assets.description = shellDescription_;
    assets.canonicalUrl = shellCanonicalUrl_;
    assets.robots = shellRobots_;
    assets.ogType = shellOgType_;
    assets.imageUrl = shellImageUrl_;
    assets.siteName = shellSiteName_;
    assets.twitterCard = shellTwitterCard_;
    assets.cssPath = cssPath_;
    assets.clientJsPath = clientJsPath_;
    assets.hmrClientPath = hmrClientPath_;
    assets.clientJsModule = clientJsModule_;
    if (devModeEnabled_ && devAutoReloadEnabled_) {
        assets.devReloadProbePath = devReloadProbePath_;
        assets.devReloadIntervalMs = devReloadIntervalMs_;
    }

    SsrRenderResult result;
    result.status = 200;
    result.html = HtmlShell::wrap("", effectivePropsJson, assets);
    result.headers["X-Request-Id"] = requestId;
    result.headers["X-Content-Type-Options"] = "nosniff";
    result.headers["Referrer-Policy"] = "strict-origin-when-cross-origin";
    result.headers["X-Frame-Options"] = "DENY";
    requestOkCount_.fetch_add(1, std::memory_order_relaxed);
    observeRequestCode(result.status);
    return result;
}

void HydraSsrPlugin::setApiBridgeHandler(ApiBridgeHandler handler) {
    std::lock_guard<std::mutex> lock(apiBridgeMutex_);
    apiBridgeHandler_ = std::move(handler);
}

ApiBridgeResponse HydraSsrPlugin::dispatchApiBridge(const ApiBridgeRequest &request) const {
    ApiBridgeResponse response;
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
    return handler(request);
}

void HydraSsrPlugin::registerDevProxyRoutes() {}

void HydraSsrPlugin::observeAcquireWait(double) const {}
void HydraSsrPlugin::observeRenderLatency(double) const {}
void HydraSsrPlugin::observeRequestLatency(double) const {}

void HydraSsrPlugin::observeRequestCode(int statusCode) const {
    if (statusCode < 100 || statusCode > static_cast<int>(kHttpStatusCodeMax)) {
        return;
    }
    requestCodeCounts_[static_cast<std::size_t>(statusCode)]
        .fetch_add(1, std::memory_order_relaxed);
}

HydraMetricsSnapshot HydraSsrPlugin::metricsSnapshot() const {
    HydraMetricsSnapshot snapshot;
    snapshot.requestsOk = requestOkCount_.load(std::memory_order_relaxed);
    snapshot.requestsFail = requestFailCount_.load(std::memory_order_relaxed);
    snapshot.renderErrors = renderErrorCount_.load(std::memory_order_relaxed);
    snapshot.poolTimeouts = poolTimeoutCount_.load(std::memory_order_relaxed);
    snapshot.renderTimeouts = renderTimeoutCount_.load(std::memory_order_relaxed);
    snapshot.runtimeRecycles = runtimeRecycleCount_.load(std::memory_order_relaxed);
    snapshot.totalAcquireWaitUs = totalAcquireWaitUs_.load(std::memory_order_relaxed);
    snapshot.totalRenderUs = totalRenderUs_.load(std::memory_order_relaxed);
    snapshot.totalWrapUs = totalWrapUs_.load(std::memory_order_relaxed);
    snapshot.totalRequestUs = totalRequestUs_.load(std::memory_order_relaxed);
    snapshot.totalAcquireWaitMs = snapshot.totalAcquireWaitUs / 1000;
    snapshot.totalRenderMs = snapshot.totalRenderUs / 1000;
    snapshot.totalWrapMs = snapshot.totalWrapUs / 1000;
    snapshot.totalRequestMs = snapshot.totalRequestUs / 1000;
    return snapshot;
}

std::string HydraSsrPlugin::metricsPrometheus() const {
    const auto snapshot = metricsSnapshot();
    std::ostringstream out;
    out << "# HELP hydra_requests_total Total Hydra shell render requests\n";
    out << "# TYPE hydra_requests_total counter\n";
    out << "hydra_requests_total{status=\"ok\"} " << snapshot.requestsOk << '\n';
    out << "hydra_requests_total{status=\"fail\"} " << snapshot.requestsFail << '\n';
    out << "# HELP hydra_shell_mode Hydra shell renderer mode\n";
    out << "# TYPE hydra_shell_mode gauge\n";
    out << "hydra_shell_mode 1\n";
    return out.str();
}

}  // namespace hydra
