#include "hydra/HydraSsrPlugin.h"

#include "hydra/HtmlShell.h"
#include "hydra/LogFmt.h"
#include "hydra/V8IsolatePool.h"
#include "hydra/V8Platform.h"

#include <drogon/drogon.h>
#include <json/reader.h>
#include <json/writer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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

const char *onOff(bool value) {
    return value ? "on" : "off";
}

bool isConsoleTty() {
#ifdef _WIN32
    const auto stdoutTty = ::_isatty(_fileno(stdout)) != 0;
    const auto stderrTty = ::_isatty(_fileno(stderr)) != 0;
#else
    const auto stdoutTty = ::isatty(fileno(stdout)) != 0;
    const auto stderrTty = ::isatty(fileno(stderr)) != 0;
#endif
    return stdoutTty || stderrTty;
}

std::string maybeColorizeLog(const std::string &line, const char *ansiCode, bool enabled) {
    if (!enabled || ansiCode == nullptr || *ansiCode == '\0') {
        return line;
    }

    std::string out;
    out.reserve(line.size() + 16);
    out.append("\x1b[");
    out.append(ansiCode);
    out.append("m");
    out.append(line);
    out.append("\x1b[0m");
    return out;
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

std::string sanitizeRequestId(std::string value) {
    value = trimAsciiWhitespace(std::move(value));
    if (value.empty()) {
        return {};
    }

    std::string sanitized;
    sanitized.reserve(value.size());
    for (const auto ch : value) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '-' || ch == '_' || ch == '.') {
            sanitized.push_back(ch);
        }
    }

    constexpr std::size_t kMaxRequestIdLen = 64;
    if (sanitized.size() > kMaxRequestIdLen) {
        sanitized.resize(kMaxRequestIdLen);
    }
    return sanitized;
}

std::string generateScriptNonce() {
    static constexpr char kNonceChars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<std::size_t> distribution(0, sizeof(kNonceChars) - 2);

    std::string nonce;
    nonce.reserve(24);
    for (std::size_t i = 0; i < 24; ++i) {
        nonce.push_back(kNonceChars[distribution(generator)]);
    }
    return nonce;
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

std::string normalizeThemeTag(std::string theme) {
    theme = trimAsciiWhitespace(std::move(theme));
    if (theme.empty()) {
        return {};
    }

    theme = toLowerCopy(std::move(theme));

    std::string normalized;
    normalized.reserve(theme.size());
    for (const auto ch : theme) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '-' || ch == '_') {
            normalized.push_back(ch);
        }
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

void appendNormalizedThemeArray(const Json::Value &value,
                                std::unordered_set<std::string> *setOut,
                                std::vector<std::string> *orderedOut) {
    if (!value.isArray() || setOut == nullptr || orderedOut == nullptr) {
        return;
    }

    for (const auto &item : value) {
        if (!item.isString()) {
            continue;
        }

        const auto theme = normalizeThemeTag(item.asString());
        if (theme.empty()) {
            continue;
        }

        if (setOut->insert(theme).second) {
            orderedOut->push_back(theme);
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

void appendUpperStringArray(const Json::Value &value,
                            std::unordered_set<std::string> *out) {
    if (out == nullptr || !value.isArray()) {
        return;
    }

    for (const auto &item : value) {
        if (!item.isString()) {
            continue;
        }

        auto key = trimAsciiWhitespace(item.asString());
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
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

std::optional<SsrRenderResult> tryParseSsrEnvelope(const std::string &renderOutput) {
    const auto firstNonWs = std::find_if_not(
        renderOutput.begin(), renderOutput.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        });
    if (firstNonWs == renderOutput.end() || *firstNonWs != '{') {
        return std::nullopt;
    }

    Json::Value payload;
    Json::CharReaderBuilder builder;
    JSONCPP_STRING errors;
    std::istringstream stream(renderOutput);
    if (!Json::parseFromStream(builder, stream, &payload, &errors) || !payload.isObject()) {
        return std::nullopt;
    }
    if (!payload.isMember("html")) {
        return std::nullopt;
    }

    SsrRenderResult result;
    if (payload["html"].isString()) {
        result.html = payload["html"].asString();
    } else {
        result.html.clear();
    }

    const auto status = payload.get("status", 200).asInt();
    result.status = status >= 100 && status <= 599 ? status : 200;

    if (payload.isMember("headers") && payload["headers"].isObject()) {
        for (const auto &headerName : payload["headers"].getMemberNames()) {
            const auto &headerValue = payload["headers"][headerName];
            if (headerValue.isString()) {
                result.headers[headerName] = headerValue.asString();
            } else if (headerValue.isBool()) {
                result.headers[headerName] = headerValue.asBool() ? "true" : "false";
            } else if (headerValue.isNumeric()) {
                result.headers[headerName] = headerValue.asString();
            }
        }
    }

    if (payload.isMember("redirect") && payload["redirect"].isString()) {
        const auto redirectTarget = trimAsciiWhitespace(payload["redirect"].asString());
        if (!redirectTarget.empty()) {
            result.headers["Location"] = redirectTarget;
            if (result.status < 300 || result.status > 399) {
                result.status = 302;
            }
        }
    } else if (result.headers.find("Location") != result.headers.end() &&
               (result.status < 300 || result.status > 399)) {
        result.status = 302;
    }

    return result;
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
                                                const std::string &routeUrl,
                                                const std::string &requestId) const {
    Json::Value context(Json::objectValue);
    context["routeUrl"] = routeUrl;
    context["requestId"] = requestId;
    context["locale"] = i18nDefaultLocale_;
    context["theme"] = themeDefault_;
    context["themeCookieName"] = themeCookieName_;
    context["themeQueryParam"] = themeQueryParam_;
    {
        Json::Value supportedThemes(Json::arrayValue);
        for (const auto &themeName : themeSupportedThemeOrder_) {
            supportedThemes.append(themeName);
        }
        if (supportedThemes.empty()) {
            supportedThemes.append(themeDefault_);
        }
        context["themeSupportedThemes"] = std::move(supportedThemes);
    }
    if (!req) {
        context["routePath"] = routeUrl;
        context["pathWithQuery"] = routeUrl;
        context["url"] = routeUrl;
        if (i18nIncludeLocaleCandidates_) {
            Json::Value candidates(Json::arrayValue);
            candidates.append(i18nDefaultLocale_);
            context["localeCandidates"] = std::move(candidates);
        }
        if (themeIncludeThemeCandidates_) {
            Json::Value candidates(Json::arrayValue);
            candidates.append(themeDefault_);
            context["themeCandidates"] = std::move(candidates);
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

    std::vector<std::string> rawThemeCandidates;
    if (!themeCookieName_.empty()) {
        const auto cookieTheme = req->getCookie(themeCookieName_);
        if (!cookieTheme.empty()) {
            rawThemeCandidates.push_back(cookieTheme);
        }
    }
    if (!themeQueryParam_.empty()) {
        const auto queryTheme = req->getParameter(themeQueryParam_);
        if (!queryTheme.empty()) {
            rawThemeCandidates.push_back(queryTheme);
        }
    }
    rawThemeCandidates.push_back(themeDefault_);

    std::vector<std::string> themeCandidates;
    for (const auto &candidate : rawThemeCandidates) {
        const auto normalized = normalizeThemeTag(candidate);
        if (normalized.empty()) {
            continue;
        }
        appendUniqueString(&themeCandidates, normalized);
    }

    std::string resolvedTheme = themeDefault_.empty() ? "ocean" : themeDefault_;
    for (const auto &candidate : themeCandidates) {
        if (themeSupportedThemes_.empty() ||
            themeSupportedThemes_.find(candidate) != themeSupportedThemes_.end()) {
            resolvedTheme = candidate;
            break;
        }
    }
    if (!themeSupportedThemes_.empty() &&
        themeSupportedThemes_.find(resolvedTheme) == themeSupportedThemes_.end() &&
        !themeSupportedThemeOrder_.empty()) {
        resolvedTheme = themeSupportedThemeOrder_.front();
    }
    context["theme"] = resolvedTheme;
    if (themeIncludeThemeCandidates_) {
        Json::Value candidates(Json::arrayValue);
        for (const auto &candidate : themeCandidates) {
            candidates.append(candidate);
        }
        context["themeCandidates"] = std::move(candidates);
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

std::string HydraSsrPlugin::resolveRequestId(const drogon::HttpRequestPtr &req) const {
    if (req) {
        const auto headerRequestId =
            sanitizeRequestId(firstHeaderToken(req->getHeader("x-request-id")));
        if (!headerRequestId.empty()) {
            return headerRequestId;
        }
    }

    const auto generated = requestIdCounter_.fetch_add(1, std::memory_order_relaxed) + 1;
    return "hydra-" + std::to_string(generated);
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

    auto normalizedMethod = trimAsciiWhitespace(request.method);
    std::transform(normalizedMethod.begin(),
                   normalizedMethod.end(),
                   normalizedMethod.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (normalizedMethod.empty()) {
        normalizedMethod = "GET";
    }

    if (apiBridgeAllowedMethods_.find(normalizedMethod) == apiBridgeAllowedMethods_.end()) {
        response.status = 405;
        response.body = "Hydra API bridge method is not allowed: " + normalizedMethod;
        return response;
    }

    bool pathAllowed = false;
    for (const auto &prefix : apiBridgeAllowedPathPrefixes_) {
        if (prefix.empty()) {
            continue;
        }
        if (request.path.rfind(prefix, 0) == 0) {
            pathAllowed = true;
            break;
        }
    }
    if (!pathAllowed) {
        response.status = 403;
        response.body = "Hydra API bridge path is not allowed: " + request.path;
        return response;
    }

    if (request.body.size() > apiBridgeMaxBodyBytes_) {
        response.status = 413;
        response.body = "Hydra API bridge body exceeds max_body_bytes";
        return response;
    }

    ApiBridgeRequest apiRequest;
    apiRequest.method = normalizedMethod;
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
    normalizedConfig_ = validateAndNormalizeHydraSsrPluginConfig(config);
    for (const auto &warning : normalizedConfig_.warnings) {
        LOG_WARN << "HydraConfig warning: " << warning;
    }

    ssrBundlePath_ = normalizedConfig_.ssrBundlePath;
    cssPath_ = normalizedConfig_.cssPath;
    clientJsPath_ = normalizedConfig_.clientJsPath;
    assetManifestPath_ = normalizedConfig_.assetManifestPath;
    assetPublicPrefix_ = normalizedConfig_.assetPublicPrefix;
    clientManifestEntry_ = normalizedConfig_.clientManifestEntry;
    isolateAcquireTimeoutMs_ = normalizedConfig_.acquireTimeoutMs;
    renderTimeoutMs_ = normalizedConfig_.renderTimeoutMs;
    wrapFragment_ = normalizedConfig_.wrapFragment;
    apiBridgeEnabled_ = normalizedConfig_.apiBridgeEnabled;
    logRenderMetrics_ = normalizedConfig_.logRenderMetrics;
    logRequestRoutes_ = normalizedConfig_.logRequestRoutes;
    devModeEnabled_ = normalizedConfig_.devModeEnabled;
    devProxyAssetsEnabled_ = normalizedConfig_.devProxyAssetsEnabled;
    devInjectHmrClient_ = normalizedConfig_.devInjectHmrClient;
    devAnsiColorLogs_ = normalizedConfig_.devAnsiColorLogs;
    ansiColorLogsActive_ = devModeEnabled_ && devAnsiColorLogs_ && isConsoleTty();
    devProxyOrigin_ = normalizedConfig_.devProxyOrigin;
    devClientEntryPath_ = normalizedConfig_.devClientEntryPath;
    devHmrClientPath_ = normalizedConfig_.devHmrClientPath;
    devCssPath_ = normalizedConfig_.devCssPath;
    devProxyTimeoutSec_ = normalizedConfig_.devProxyTimeoutSec;
    devAutoReloadEnabled_ = normalizedConfig_.devAutoReloadEnabled;
    devReloadProbePath_ = normalizedConfig_.devReloadProbePath;
    devReloadIntervalMs_ = normalizedConfig_.devReloadIntervalMs;
    const Json::Value *i18nConfig =
        config.isMember("i18n") && config["i18n"].isObject() ? &config["i18n"] : nullptr;
    const Json::Value *themeConfig =
        config.isMember("theme") && config["theme"].isObject() ? &config["theme"] : nullptr;
    const Json::Value *requestContextConfig =
        config.isMember("request_context") && config["request_context"].isObject()
            ? &config["request_context"]
            : nullptr;
    const Json::Value *apiBridgeConfig =
        config.isMember("api_bridge") && config["api_bridge"].isObject() ? &config["api_bridge"]
                                                                           : nullptr;
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
    auto readThemeString = [&](const char *nestedKey,
                               const char *topLevelKey,
                               const std::string &fallback) -> std::string {
        if (themeConfig && themeConfig->isMember(nestedKey)) {
            return (*themeConfig)[nestedKey].asString();
        }
        return config.get(topLevelKey, fallback).asString();
    };
    auto readThemeBool = [&](const char *nestedKey,
                             const char *topLevelKey,
                             bool fallback) -> bool {
        if (themeConfig && themeConfig->isMember(nestedKey)) {
            return (*themeConfig)[nestedKey].asBool();
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
    auto appendThemeArray = [&](const char *nestedKey, const char *topLevelKey) {
        if (themeConfig && themeConfig->isMember(nestedKey)) {
            appendNormalizedThemeArray(
                (*themeConfig)[nestedKey],
                &themeSupportedThemes_,
                &themeSupportedThemeOrder_);
            return;
        }
        if (config.isMember(topLevelKey)) {
            appendNormalizedThemeArray(
                config[topLevelKey], &themeSupportedThemes_, &themeSupportedThemeOrder_);
        }
    };
    auto appendApiBridgePathPrefixes = [&](const Json::Value &value) {
        if (!value.isArray()) {
            return;
        }
        for (const auto &item : value) {
            if (!item.isString()) {
                continue;
            }
            const auto prefix = trimAsciiWhitespace(item.asString());
            if (!prefix.empty()) {
                apiBridgeAllowedPathPrefixes_.push_back(prefix);
            }
        }
    };

    apiBridgeAllowedMethods_.clear();
    if (apiBridgeConfig != nullptr && apiBridgeConfig->isMember("allowed_methods")) {
        appendUpperStringArray((*apiBridgeConfig)["allowed_methods"], &apiBridgeAllowedMethods_);
    } else if (config.isMember("api_bridge_allowed_methods")) {
        appendUpperStringArray(config["api_bridge_allowed_methods"], &apiBridgeAllowedMethods_);
    }
    if (apiBridgeAllowedMethods_.empty()) {
        apiBridgeAllowedMethods_ = {"GET", "POST"};
    }

    apiBridgeAllowedPathPrefixes_.clear();
    if (apiBridgeConfig != nullptr && apiBridgeConfig->isMember("allowed_path_prefixes")) {
        appendApiBridgePathPrefixes((*apiBridgeConfig)["allowed_path_prefixes"]);
    } else if (config.isMember("api_bridge_allowed_path_prefixes")) {
        appendApiBridgePathPrefixes(config["api_bridge_allowed_path_prefixes"]);
    }
    if (apiBridgeAllowedPathPrefixes_.empty()) {
        apiBridgeAllowedPathPrefixes_.push_back("/hydra/internal/");
    }

    const auto readApiBridgeBodyLimit = [&]() -> std::uint64_t {
        if (apiBridgeConfig != nullptr && apiBridgeConfig->isMember("max_body_bytes")) {
            return (*apiBridgeConfig)["max_body_bytes"].asUInt64();
        }
        return config.get("api_bridge_max_body_bytes", 64 * 1024).asUInt64();
    };
    const auto maxBodyBytes = readApiBridgeBodyLimit();
    if (maxBodyBytes == 0 || maxBodyBytes > (16ULL * 1024ULL * 1024ULL)) {
        throw std::runtime_error(
            "HydraSsrPlugin config 'api_bridge.max_body_bytes' must be in range 1..16777216");
    }
    apiBridgeMaxBodyBytes_ = static_cast<std::size_t>(maxBodyBytes);

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

    themeDefault_ = normalizeThemeTag(readThemeString("defaultTheme", "theme_default", "ocean"));
    if (themeDefault_.empty()) {
        themeDefault_ = "ocean";
    }
    themeQueryParam_ = trimAsciiWhitespace(
        readThemeString("queryParam", "theme_query_param", "theme"));
    if (themeQueryParam_.empty()) {
        themeQueryParam_ = "theme";
    }
    themeCookieName_ = trimAsciiWhitespace(
        readThemeString("cookieName", "theme_cookie_name", "hydra_theme"));
    if (themeCookieName_.empty()) {
        themeCookieName_ = "hydra_theme";
    }
    themeIncludeThemeCandidates_ = readThemeBool(
        "includeThemeCandidates",
        "theme_include_theme_candidates",
        false);
    themeIncludeThemeCandidates_ = readThemeBool(
        "include_theme_candidates",
        "theme_includeThemeCandidates",
        themeIncludeThemeCandidates_);
    themeSupportedThemes_.clear();
    themeSupportedThemeOrder_.clear();
    appendThemeArray("supportedThemes", "theme_supported_themes");
    appendThemeArray("supported_themes", "theme_supportedThemes");
    if (themeSupportedThemes_.empty()) {
        themeSupportedThemes_.insert(themeDefault_);
        themeSupportedThemeOrder_.push_back(themeDefault_);
    } else if (themeSupportedThemes_.find(themeDefault_) == themeSupportedThemes_.end()) {
        themeSupportedThemes_.insert(themeDefault_);
        themeSupportedThemeOrder_.push_back(themeDefault_);
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

    if (devModeEnabled_) {
        auto line = logfmt::Line("HydraInit")
                        .block(summarizeHydraSsrPluginConfig(normalizedConfig_))
                        .group("runtime", {{"pool", std::to_string(isolatePoolSize_)}})
                        .group("flags",
                               {{"dev", logfmt::onOff(devModeEnabled_)},
                                {"api_bridge", logfmt::onOff(apiBridgeEnabled_)},
                                {"include_cookies", logfmt::onOff(requestContextIncludeCookies_)},
                                {"include_cookie_map",
                                 logfmt::onOff(requestContextIncludeCookieMap_)},
                                {"request_routes", logfmt::onOff(logRequestRoutes_)}})
                        .group("defaults",
                               {{"locale", i18nDefaultLocale_}, {"theme", themeDefault_}});
        LOG_INFO << maybeColorizeLog(line.str(), "1;36", ansiColorLogsActive_);
    } else {
        auto infoLine = logfmt::Line("HydraInit")
                            .block(summarizeHydraSsrPluginConfig(normalizedConfig_))
                            .group("runtime", {{"pool", std::to_string(isolatePoolSize_)}})
                            .group("flags",
                                   {{"dev", logfmt::onOff(devModeEnabled_)},
                                    {"api_bridge", logfmt::onOff(apiBridgeEnabled_)},
                                    {"request_routes", logfmt::onOff(logRequestRoutes_)}})
                            .group("defaults",
                                   {{"locale", i18nDefaultLocale_}, {"theme", themeDefault_}});
        LOG_INFO << maybeColorizeLog(infoLine.str(), "1;36", ansiColorLogsActive_);

        auto debugLine = logfmt::Line("HydraInit detail")
                             .group("flags",
                                    {{"include_cookies",
                                      logfmt::onOff(requestContextIncludeCookies_)},
                                     {"include_cookie_map",
                                      logfmt::onOff(requestContextIncludeCookieMap_)}});
        LOG_DEBUG << maybeColorizeLog(debugLine.str(), "2;37", ansiColorLogsActive_);
    }
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
    return renderResult(req, toCompactJson(props), options);
}

SsrRenderResult HydraSsrPlugin::renderResult(const drogon::HttpRequestPtr &req,
                                             const std::string &propsJson,
                                             const RenderOptions &options) const {
    if (!isolatePool_) {
        SsrRenderResult unavailable;
        unavailable.status = 500;
        unavailable.html = HtmlShell::errorPage("HydraSsrPlugin is not initialized");
        unavailable.headers["X-Request-Id"] = resolveRequestId(req);
        unavailable.headers["X-Content-Type-Options"] = "nosniff";
        unavailable.headers["Referrer-Policy"] = "strict-origin-when-cross-origin";
        return unavailable;
    }

    const auto routeUrl = buildRouteUrl(req, options);
    const auto requestId = resolveRequestId(req);
    const auto requestContext = buildRequestContext(req, routeUrl, requestId);
    const auto requestContextJson = toCompactJson(requestContext);
    std::string effectivePropsJson = propsJson;
    Json::Value propsObject;
    std::string pageId;
    if (parseJsonObject(propsJson, &propsObject)) {
        const auto route = propsObject["__hydra_route"];
        if (route.isObject() && route["pageId"].isString()) {
            pageId = route["pageId"].asString();
        } else if (propsObject["page"].isString()) {
            pageId = propsObject["page"].asString();
        }
        propsObject["__hydra_request"] = requestContext;
        effectivePropsJson = toCompactJson(propsObject);
    }
    const auto acquireStartedAt = std::chrono::steady_clock::now();
    const auto requestStartedAt = acquireStartedAt;
    std::uint64_t acquireWaitUs = 0;
    double acquireWaitMs = 0.0;
    const auto requestMethod = req ? req->methodString() : std::string("GET");
    const auto scriptNonce = devModeEnabled_ ? std::string{} : generateScriptNonce();
    const auto requestElapsedUs = [&]() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - requestStartedAt)
                .count());
    };

    const auto logRenderOk = [&](std::uint64_t renderIndex,
                                 double renderMs,
                                 double wrapMs,
                                 int statusCode) {
        if (!logRenderMetrics_) {
            return;
        }

        LOG_INFO << "HydraMetrics"
                 << " | status=ok"
                 << " | count=" << renderIndex
                 << " | route=" << routeUrl
                 << " | request_id=" << requestId
                 << " | http_status=" << statusCode
                 << " | latency_ms{acquire=" << acquireWaitMs
                 << ", render=" << renderMs
                 << ", wrap=" << wrapMs << "}"
                 << " | counters{pool_timeouts="
                 << poolTimeoutCount_.load(std::memory_order_relaxed)
                 << ", render_timeouts="
                 << renderTimeoutCount_.load(std::memory_order_relaxed)
                 << ", runtime_recycles="
                 << runtimeRecycleCount_.load(std::memory_order_relaxed)
                 << "}";
    };

    const auto logRenderFail = [&](const std::string &errorMessage, int statusCode) {
        if (!logRenderMetrics_) {
            return;
        }

        LOG_WARN << "HydraMetrics"
                 << " | status=fail"
                 << " | route=" << routeUrl
                 << " | request_id=" << requestId
                 << " | http_status=" << statusCode
                 << " | latency_ms{acquire=" << acquireWaitMs
                 << ", wrap=0}"
                 << " | counters{pool_timeouts="
                 << poolTimeoutCount_.load(std::memory_order_relaxed)
                 << ", render_timeouts="
                 << renderTimeoutCount_.load(std::memory_order_relaxed)
                 << ", runtime_recycles="
                 << runtimeRecycleCount_.load(std::memory_order_relaxed)
                 << "} | error=\"" << errorMessage << "\"";
    };

    const auto logRequestRoute = [&](const char *status,
                                     double totalMs,
                                     int statusCode,
                                     const std::string *errorMessage = nullptr) {
        if (!logRequestRoutes_) {
            return;
        }
        if (errorMessage != nullptr) {
            LOG_WARN << "HydraRequest"
                     << " | status=" << status
                     << " | method=" << requestMethod
                     << " | route=" << routeUrl
                     << " | request_id=" << requestId
                     << " | http_status=" << statusCode
                     << " | total_ms=" << totalMs
                     << " | error=\"" << *errorMessage << "\"";
            return;
        }

        LOG_INFO << "HydraRequest"
                 << " | status=" << status
                 << " | method=" << requestMethod
                 << " | route=" << routeUrl
                 << " | request_id=" << requestId
                 << " | http_status=" << statusCode
                 << " | page=" << (pageId.empty() ? "-" : pageId)
                 << " | total_ms=" << totalMs;
    };
    const auto applySecurityHeaders = [&](SsrRenderResult *response, bool wrappedWithShell) {
        if (response == nullptr) {
            return;
        }

        response->headers.try_emplace("X-Content-Type-Options", "nosniff");
        response->headers.try_emplace("Referrer-Policy", "strict-origin-when-cross-origin");
        response->headers.try_emplace("X-Frame-Options", "DENY");

        if (devModeEnabled_ ||
            response->headers.find("Content-Security-Policy") != response->headers.end()) {
            return;
        }

        if (wrappedWithShell && !scriptNonce.empty()) {
            response->headers["Content-Security-Policy"] =
                "default-src 'self'; script-src 'self' 'nonce-" + scriptNonce +
                "'; style-src 'self' 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; "
                "object-src 'none'; base-uri 'self'; frame-ancestors 'none'";
        } else {
            response->headers["Content-Security-Policy"] =
                "default-src 'self'; object-src 'none'; base-uri 'self'; frame-ancestors 'none'";
        }
    };

    try {
        auto lease = isolatePool_->acquire(isolateAcquireTimeoutMs_);
        acquireWaitUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - acquireStartedAt)
                .count());
        acquireWaitMs = static_cast<double>(acquireWaitUs) / 1000.0;

        try {
            const auto renderStartedAt = std::chrono::steady_clock::now();
            auto rawRenderOutput =
                lease->render(
                    routeUrl,
                    effectivePropsJson,
                    requestContextJson,
                    isolatePool_->renderTimeoutMs());
            observeAcquireWait(acquireWaitMs);
            const auto renderUs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - renderStartedAt)
                    .count());
            const auto renderMs = static_cast<double>(renderUs) / 1000.0;
            observeRenderLatency(renderMs);
            const auto renderIndex =
                renderCount_.fetch_add(1, std::memory_order_relaxed) + 1;
            std::uint64_t wrapUs = 0;
            double wrapMs = 0.0;
            SsrRenderResult renderResult;
            if (const auto parsed = tryParseSsrEnvelope(rawRenderOutput); parsed.has_value()) {
                renderResult = *parsed;
            } else {
                renderResult.html = std::move(rawRenderOutput);
                renderResult.status = 200;
            }

            const bool isRedirect =
                renderResult.status >= 300 &&
                renderResult.status <= 399 &&
                renderResult.headers.find("Location") != renderResult.headers.end();

            if (!isRedirect &&
                wrapFragment_ &&
                !renderResult.html.empty() &&
                !isLikelyFullDocument(renderResult.html)) {
                HtmlShellAssets assets;
                assets.cssPath = cssPath_;
                assets.clientJsPath = clientJsPath_;
                assets.hmrClientPath = hmrClientPath_;
                assets.scriptNonce = scriptNonce;
                assets.clientJsModule = clientJsModule_;
                if (devModeEnabled_ && devAutoReloadEnabled_) {
                    assets.devReloadProbePath = normalizeBrowserPath(devReloadProbePath_);
                    assets.devReloadIntervalMs = devReloadIntervalMs_;
                }
                const auto wrapStartedAt = std::chrono::steady_clock::now();
                auto wrappedHtml = HtmlShell::wrap(
                    renderResult.html,
                    effectivePropsJson,
                    assets);
                wrapUs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - wrapStartedAt)
                        .count());
                wrapMs = static_cast<double>(wrapUs) / 1000.0;
                const auto totalUs = requestElapsedUs();
                const auto totalMs = static_cast<double>(totalUs) / 1000.0;
                requestOkCount_.fetch_add(1, std::memory_order_relaxed);
                observeRequestCode(renderResult.status);
                observeRequestLatency(totalMs);
                totalRequestUs_.fetch_add(totalUs, std::memory_order_relaxed);
                totalAcquireWaitUs_.fetch_add(acquireWaitUs, std::memory_order_relaxed);
                totalRenderUs_.fetch_add(renderUs, std::memory_order_relaxed);
                totalWrapUs_.fetch_add(wrapUs, std::memory_order_relaxed);
                renderResult.html = std::move(wrappedHtml);
                renderResult.headers.try_emplace("X-Request-Id", requestId);
                applySecurityHeaders(&renderResult, true);
                logRenderOk(renderIndex, renderMs, wrapMs, renderResult.status);
                logRequestRoute("ok", totalMs, renderResult.status);
                return renderResult;
            }

            if (!isRedirect &&
                !wrapFragment_ &&
                !renderResult.html.empty() &&
                !isLikelyFullDocument(renderResult.html)) {
                bool expected = false;
                if (warnedUnwrappedFragment_.compare_exchange_strong(
                        expected, true, std::memory_order_relaxed)) {
                    LOG_WARN << "HydraSsrPlugin wrap_fragment=false while SSR returned HTML fragment. "
                             << "This can break CSS/JS injection.";
                }
            }

            const auto totalUs = requestElapsedUs();
            const auto totalMs = static_cast<double>(totalUs) / 1000.0;
            requestOkCount_.fetch_add(1, std::memory_order_relaxed);
            observeRequestCode(renderResult.status);
            observeRequestLatency(totalMs);
            totalRequestUs_.fetch_add(totalUs, std::memory_order_relaxed);
            totalAcquireWaitUs_.fetch_add(acquireWaitUs, std::memory_order_relaxed);
            totalRenderUs_.fetch_add(renderUs, std::memory_order_relaxed);
            totalWrapUs_.fetch_add(wrapUs, std::memory_order_relaxed);
            renderResult.headers.try_emplace("X-Request-Id", requestId);
            applySecurityHeaders(&renderResult, false);
            logRenderOk(renderIndex, renderMs, wrapMs, renderResult.status);
            logRequestRoute("ok", totalMs, renderResult.status);

            return renderResult;
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
        const auto totalUs = requestElapsedUs();
        const auto totalMs = static_cast<double>(totalUs) / 1000.0;
        requestFailCount_.fetch_add(1, std::memory_order_relaxed);
        observeRequestCode(500);
        observeRequestLatency(totalMs);
        renderErrorCount_.fetch_add(1, std::memory_order_relaxed);
        totalRequestUs_.fetch_add(totalUs, std::memory_order_relaxed);
        totalAcquireWaitUs_.fetch_add(acquireWaitUs, std::memory_order_relaxed);
        observeAcquireWait(acquireWaitMs);
        logRenderFail(message, 500);
        logRequestRoute("fail", totalMs, 500, &message);
        LOG_ERROR << "HydraStack render failed for url=" << routeUrl
                  << ", request_id=" << requestId << ": " << ex.what();
        SsrRenderResult failed;
        failed.status = 500;
        failed.html = HtmlShell::errorPage(ex.what());
        failed.headers["X-Request-Id"] = requestId;
        applySecurityHeaders(&failed, false);
        return failed;
    } catch (...) {
        const auto totalUs = requestElapsedUs();
        const auto totalMs = static_cast<double>(totalUs) / 1000.0;
        requestFailCount_.fetch_add(1, std::memory_order_relaxed);
        observeRequestCode(500);
        observeRequestLatency(totalMs);
        renderErrorCount_.fetch_add(1, std::memory_order_relaxed);
        totalRequestUs_.fetch_add(totalUs, std::memory_order_relaxed);
        totalAcquireWaitUs_.fetch_add(acquireWaitUs, std::memory_order_relaxed);
        observeAcquireWait(acquireWaitMs);
        logRenderFail("unknown", 500);
        const std::string unknownError = "unknown";
        logRequestRoute("fail", totalMs, 500, &unknownError);
        LOG_ERROR << "HydraStack render failed for url=" << routeUrl
                  << ", request_id=" << requestId << ": unknown exception";
        SsrRenderResult failed;
        failed.status = 500;
        failed.html = HtmlShell::errorPage("Unknown SSR runtime error");
        failed.headers["X-Request-Id"] = requestId;
        applySecurityHeaders(&failed, false);
        return failed;
    }
}

void HydraSsrPlugin::observeAcquireWait(double valueMs) const {
    static constexpr std::array<double, kLatencyHistogramBucketCount - 1> kUpperBoundsMs = {
        1.0, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 2500.0, 5000.0, 10000.0};
    std::size_t bucketIndex = 0;
    while (bucketIndex < kUpperBoundsMs.size() && valueMs > kUpperBoundsMs[bucketIndex]) {
        ++bucketIndex;
    }
    acquireWaitHistogram_[bucketIndex].fetch_add(1, std::memory_order_relaxed);
}

void HydraSsrPlugin::observeRenderLatency(double valueMs) const {
    static constexpr std::array<double, kLatencyHistogramBucketCount - 1> kUpperBoundsMs = {
        1.0, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 2500.0, 5000.0, 10000.0};
    std::size_t bucketIndex = 0;
    while (bucketIndex < kUpperBoundsMs.size() && valueMs > kUpperBoundsMs[bucketIndex]) {
        ++bucketIndex;
    }
    renderLatencyHistogram_[bucketIndex].fetch_add(1, std::memory_order_relaxed);
}

void HydraSsrPlugin::observeRequestLatency(double valueMs) const {
    static constexpr std::array<double, kLatencyHistogramBucketCount - 1> kUpperBoundsMs = {
        1.0, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 2500.0, 5000.0, 10000.0};
    std::size_t bucketIndex = 0;
    while (bucketIndex < kUpperBoundsMs.size() && valueMs > kUpperBoundsMs[bucketIndex]) {
        ++bucketIndex;
    }
    requestLatencyHistogram_[bucketIndex].fetch_add(1, std::memory_order_relaxed);
}

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
    const auto totalRequests = snapshot.requestsOk + snapshot.requestsFail;
    const auto poolInUse = isolatePool_ ? isolatePool_->inUseCount() : 0;
    const auto poolSize = isolatePool_ ? isolatePool_->size() : 0;
    static constexpr std::array<double, kLatencyHistogramBucketCount - 1> kUpperBoundsMs = {
        1.0, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 2500.0, 5000.0, 10000.0};

    std::ostringstream out;
    const auto emitHistogram = [&](const char *name,
                                   const char *helpText,
                                   const auto &histogramBuckets,
                                   double sumMs,
                                   std::uint64_t count) {
        out << "# HELP " << name << " " << helpText << '\n';
        out << "# TYPE " << name << " histogram\n";
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < kUpperBoundsMs.size(); ++i) {
            cumulative += histogramBuckets[i].load(std::memory_order_relaxed);
            out << name << "_bucket{le=\"" << static_cast<std::uint64_t>(kUpperBoundsMs[i]) << "\"} "
                << cumulative << '\n';
        }
        cumulative += histogramBuckets[kUpperBoundsMs.size()].load(std::memory_order_relaxed);
        out << name << "_bucket{le=\"+Inf\"} " << cumulative << '\n';
        out << name << "_sum " << sumMs << '\n';
        out << name << "_count " << count << '\n';
    };

    emitHistogram(
        "hydra_acquire_wait_ms",
        "Hydra isolate acquire wait histogram in milliseconds.",
        acquireWaitHistogram_,
        static_cast<double>(snapshot.totalAcquireWaitUs) / 1000.0,
        totalRequests);
    emitHistogram(
        "hydra_render_latency_ms",
        "Hydra engine-side SSR render latency histogram in milliseconds.",
        renderLatencyHistogram_,
        static_cast<double>(snapshot.totalRenderUs) / 1000.0,
        snapshot.requestsOk);
    emitHistogram(
        "hydra_request_total_ms",
        "Hydra end-to-end request latency histogram in milliseconds.",
        requestLatencyHistogram_,
        static_cast<double>(snapshot.totalRequestUs) / 1000.0,
        totalRequests);

    out << "# HELP hydra_pool_in_use Number of V8 runtimes currently leased.\n";
    out << "# TYPE hydra_pool_in_use gauge\n";
    out << "hydra_pool_in_use " << poolInUse << '\n';

    out << "# HELP hydra_pool_size Total V8 runtimes in the pool.\n";
    out << "# TYPE hydra_pool_size gauge\n";
    out << "hydra_pool_size " << poolSize << '\n';

    out << "# HELP hydra_render_timeouts_total Total SSR render timeout terminations.\n";
    out << "# TYPE hydra_render_timeouts_total counter\n";
    out << "hydra_render_timeouts_total " << snapshot.renderTimeouts << '\n';

    out << "# HELP hydra_recycles_total Total runtime recycle events.\n";
    out << "# TYPE hydra_recycles_total counter\n";
    out << "hydra_recycles_total " << snapshot.runtimeRecycles << '\n';

    out << "# HELP hydra_render_errors_total Total SSR render failures.\n";
    out << "# TYPE hydra_render_errors_total counter\n";
    out << "hydra_render_errors_total " << snapshot.renderErrors << '\n';

    out << "# HELP hydra_requests_total Total SSR requests by status.\n";
    out << "# TYPE hydra_requests_total counter\n";
    out << "hydra_requests_total{status=\"ok\"} " << snapshot.requestsOk << '\n';
    out << "hydra_requests_total{status=\"fail\"} " << snapshot.requestsFail << '\n';

    out << "# HELP hydra_requests_by_code_total Total SSR requests by HTTP status code.\n";
    out << "# TYPE hydra_requests_by_code_total counter\n";
    for (std::size_t statusCode = 100; statusCode <= kHttpStatusCodeMax; ++statusCode) {
        const auto count = requestCodeCounts_[statusCode].load(std::memory_order_relaxed);
        if (count == 0) {
            continue;
        }
        out << "hydra_requests_by_code_total{code=\"" << statusCode << "\"} " << count << '\n';
    }

    return out.str();
}

}  // namespace hydra
