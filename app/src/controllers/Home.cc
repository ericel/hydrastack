#include "hydra/HydraRoute.h"
#include "hydra/HydraSsrPlugin.h"

#include <drogon/HttpController.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <json/value.h>

#include <chrono>
#include <cstdint>
#include <algorithm>

namespace demo::controllers {
namespace {
const auto kProcessStartedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch())
                                   .count();

using HttpCallback = std::function<void(const drogon::HttpResponsePtr &)>;

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

Json::Value buildRouteQuery(const drogon::HttpRequestPtr &req) {
    if (!req) {
        return Json::Value(Json::objectValue);
    }

    return hydra::HydraRoute::toJsonObject(req->getParameters());
}
}  // namespace

class Home final : public drogon::HttpController<Home> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(Home::index, "/", drogon::Get);
    ADD_METHOD_TO(Home::postDetail, "/posts/{1}", drogon::Get);
    ADD_METHOD_TO(Home::redirectHome, "/go-home", drogon::Get);
    ADD_METHOD_TO(Home::notFoundPage, "/not-found", drogon::Get);
    ADD_METHOD_TO(Home::test, "/__hydra/test", drogon::Get);
    ADD_METHOD_TO(Home::metrics, "/__hydra/metrics", drogon::Get);
    METHOD_LIST_END

    void index(const drogon::HttpRequestPtr &req,
               HttpCallback &&callback) const {
        Json::Value props(Json::objectValue);
        const std::string path = req->path().empty() ? "/" : req->path();
        const std::string pathWithQuery = buildPathWithQuery(req);

        props["page"] = "home";
        props["path"] = path;
        props["pathWithQuery"] = pathWithQuery;
        hydra::HydraRoute::set(
            props,
            "home",
            Json::Value(Json::objectValue),
            buildRouteQuery(req),
            path,
            pathWithQuery);

        const auto burnMs = req->getOptionalParameter<int>("burn_ms").value_or(0);
        const auto showCounter = req->getOptionalParameter<int>("counter").value_or(0);
        const auto bridgePath = req->getParameter("bridge_path");
        if (burnMs > 0 || showCounter > 0 || !bridgePath.empty()) {
            Json::Value testConfig;
            if (burnMs > 0) {
                testConfig["burnMs"] = static_cast<Json::Int64>(burnMs);
            }
            if (showCounter > 0) {
                testConfig["counter"] = true;
            }
            if (!bridgePath.empty()) {
                testConfig["bridgePath"] = bridgePath;
            }
            props["__hydra_test"] = std::move(testConfig);
        }

        renderPage(req, std::move(callback), std::move(props));
    }

    void postDetail(const drogon::HttpRequestPtr &req,
                    HttpCallback &&callback,
                    std::string postId) const {
        Json::Value props(Json::objectValue);
        const std::string path = req->path().empty() ? "/" : req->path();
        const std::string pathWithQuery = buildPathWithQuery(req);

        Json::Value routeParams(Json::objectValue);
        routeParams["postId"] = postId;

        props["page"] = "post_detail";
        props["path"] = path;
        props["pathWithQuery"] = pathWithQuery;
        props["postId"] = postId;
        props["messageKey"] = "post_detail_title";
        hydra::HydraRoute::set(
            props,
            "post_detail",
            routeParams,
            buildRouteQuery(req), 
            path,
            pathWithQuery);

        renderPage(req, std::move(callback), std::move(props));
    }

    void redirectHome(const drogon::HttpRequestPtr &req,
                      HttpCallback &&callback) const {
        Json::Value props(Json::objectValue);
        const std::string path = req->path().empty() ? "/" : req->path();
        const std::string pathWithQuery = buildPathWithQuery(req);

        props["page"] = "redirect_home";
        props["path"] = path;
        props["pathWithQuery"] = pathWithQuery;
        hydra::HydraRoute::set(
            props,
            "redirect_home",
            Json::Value(Json::objectValue),
            buildRouteQuery(req),
            path,
            pathWithQuery);

        renderPage(req, std::move(callback), std::move(props));
    }

    void notFoundPage(const drogon::HttpRequestPtr &req,
                      HttpCallback &&callback) const {
        Json::Value props(Json::objectValue);
        const std::string path = req->path().empty() ? "/" : req->path();
        const std::string pathWithQuery = buildPathWithQuery(req);

        props["page"] = "not_found";
        props["path"] = path;
        props["pathWithQuery"] = pathWithQuery;
        hydra::HydraRoute::set(
            props,
            "not_found",
            Json::Value(Json::objectValue),
            buildRouteQuery(req),
            path,
            pathWithQuery);

        renderPage(req, std::move(callback), std::move(props));
    }

    void test(const drogon::HttpRequestPtr &req,
              HttpCallback &&callback) const {
        Json::Value payload;
        payload["ok"] = true;
        payload["service"] = "hydra_demo";
        payload["path"] = req->path();
        payload["query"] = req->query();
        payload["process_started_ms"] = static_cast<Json::Int64>(kProcessStartedMs);
        payload["tip"] =
            "C++ hot reload:mm working scripts/dev.sh; Vite HMR: edits under ui/src/*";

        callback(drogon::HttpResponse::newHttpJsonResponse(payload));
    }

    void metrics(const drogon::HttpRequestPtr &,
                 HttpCallback &&callback) const {
        auto hydra = drogon::app().getPlugin<hydra::HydraSsrPlugin>();
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k200OK);
        response->setContentTypeString("text/plain; version=0.0.4; charset=utf-8");
        response->setBody(hydra->metricsPrometheus());
        callback(response);
    }

  private:
    void renderPage(const drogon::HttpRequestPtr &req,
                    HttpCallback &&callback,
                    Json::Value props) const {
        auto hydra = drogon::app().getPlugin<hydra::HydraSsrPlugin>();
        const auto rendered = hydra->renderResult(req, props);

        auto response = drogon::HttpResponse::newHttpResponse();
        const auto normalizedStatus = std::clamp(rendered.status, 100, 599);
        response->setStatusCode(static_cast<drogon::HttpStatusCode>(normalizedStatus));
        response->setContentTypeCode(drogon::CT_TEXT_HTML);
        response->setBody(rendered.html);
        for (const auto &[headerName, headerValue] : rendered.headers) {
            response->addHeader(headerName, headerValue);
        }
        callback(response);
    }
};

}  // namespace demo::controllers
