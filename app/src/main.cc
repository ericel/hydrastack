#include "hydra/HydraRoute.h"
#include "hydra/HydraSsrPlugin.h"

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <json/value.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace {

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool pathLooksLikeFile(const std::string &path) {
    const auto lastSlash = path.find_last_of('/');
    const auto lastDot = path.find_last_of('.');
    if (lastDot == std::string::npos) {
        return false;
    }
    return lastSlash == std::string::npos || lastDot > lastSlash;
}

bool pathShouldBypassHydraErrorUi(const std::string &path) {
    return path.rfind("/assets/", 0) == 0 ||
           path.rfind("/@vite/", 0) == 0 ||
           path.rfind("/src/", 0) == 0 ||
           path.rfind("/node_modules/", 0) == 0;
}

std::string buildPathWithQuery(const drogon::HttpRequestPtr &req) {
    if (!req) {
        return "/";
    }

    const std::string path = req->path().empty() ? "/" : req->path();
    if (req->query().empty()) {
        return path;
    }

    return path + "?" + req->query();
}

bool shouldUseHydraErrorUi(drogon::HttpStatusCode status,
                           const drogon::HttpRequestPtr &req) {
    const auto code = static_cast<int>(status);
    if (code < 400 || req == nullptr) {
        return false;
    }

    const std::string method = req->methodString();
    if (method != "GET" && method != "HEAD") {
        return false;
    }

    const auto path = req->path().empty() ? std::string("/") : req->path();
    if (pathLooksLikeFile(path) || pathShouldBypassHydraErrorUi(path)) {
        return false;
    }

    const auto fetchDest = toLowerCopy(req->getHeader("sec-fetch-dest"));
    if (!fetchDest.empty() && fetchDest != "document" && fetchDest != "iframe") {
        return false;
    }

    const auto accept = toLowerCopy(req->getHeader("accept"));
    if (accept.empty()) {
        return true;
    }

    return accept.find("text/html") != std::string::npos ||
           accept.find("application/xhtml+xml") != std::string::npos ||
           (accept.find("*/*") != std::string::npos && !pathLooksLikeFile(path));
}

Json::Value buildErrorProps(drogon::HttpStatusCode status,
                            const drogon::HttpRequestPtr &req) {
    const auto statusCode = static_cast<int>(status);
    const auto reason = std::string(drogon::statusCodeToString(statusCode));

    const std::string routePath = (req && !req->path().empty()) ? req->path() : "/";
    const std::string routeUrl = buildPathWithQuery(req);

    Json::Value props(Json::objectValue);
    props["page"] = "error_http";
    props["path"] = routePath;
    props["pathWithQuery"] = routeUrl;
    props["errorStatusCode"] = statusCode;
    props["errorReason"] = reason;
    props["message"] = reason.empty() ? "Request failed" : reason;

    Json::Value params(Json::objectValue);
    Json::Value query = req ? hydra::HydraRoute::toJsonObject(req->getParameters())
                            : Json::Value(Json::objectValue);
    hydra::HydraRoute::set(props, "error_http", params, query, routePath, routeUrl);
    return props;
}

void installHydraErrorHandler() {
    drogon::app().setCustomErrorHandler(
        [](drogon::HttpStatusCode status, const drogon::HttpRequestPtr &req) {
            auto fallback = drogon::HttpResponse::newHttpResponse(status, drogon::CT_TEXT_HTML);

            if (!shouldUseHydraErrorUi(status, req)) {
                return fallback;
            }

            auto *hydra = drogon::app().getPlugin<hydra::HydraSsrPlugin>();
            if (hydra == nullptr) {
                return fallback;
            }

            try {
                auto rendered = hydra->renderResult(req, buildErrorProps(status, req));

                auto response = drogon::HttpResponse::newHttpResponse();
                const auto responseStatus = std::clamp(rendered.status, 100, 599);
                response->setStatusCode(
                    static_cast<drogon::HttpStatusCode>(responseStatus));
                response->setContentTypeCode(drogon::CT_TEXT_HTML);
                response->setBody(rendered.html);
                for (const auto &[headerName, headerValue] : rendered.headers) {
                    response->addHeader(headerName, headerValue);
                }
                return response;
            } catch (...) {
                return fallback;
            }
        });
}

}  // namespace

int main(int argc, char *argv[]) {
#ifdef HYDRA_PUBLIC_DIR
    drogon::app().setDocumentRoot(HYDRA_PUBLIC_DIR);
#endif

    std::string configPath = "app/config.json";
    if (const char *configFromEnv = std::getenv("HYDRA_CONFIG");
        configFromEnv != nullptr && *configFromEnv != '\0') {
        configPath = configFromEnv;
    }
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
        configPath = argv[1];
    }

    drogon::app().loadConfigFile(configPath);
    installHydraErrorHandler();
    drogon::app().run();
    return 0;
}
