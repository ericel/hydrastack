#include "hydra/HydraRoute.h"
#include "hydra/HydraSsrPlugin.h"

#include <drogon/HttpController.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <json/value.h>

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <string>

namespace app::controllers {
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

Json::Value buildDemoPost(const std::string &postId) {
    Json::Value post(Json::objectValue);
    post["id"] = postId;
    post["title"] = "HydraStack demo post";
    post["author"] = "Hydra Team";
    post["summary"] =
        "This is a seeded post payload from the C++ controller for SSR and hydration demos.";
    post["body"] =
        "HydraStack keeps routing in Drogon and rendering in React SSR. This post payload "
        "comes from Home.cc so UI can render deterministic data on both server and client.";
    post["publishedAt"] = "2026-02-15";
    post["readMinutes"] = 4;
    post["likes"] = 128;
    post["tags"] = Json::arrayValue;
    post["tags"].append("hydrastack");
    post["tags"].append("drogon");
    post["tags"].append("react-ssr");

    if (postId == "123") {
        post["title"] = "Post 123: Controller-provided test data";
        post["summary"] = "Seed data from C++ controller flowing into React SSR.";
        post["body"] =
            "Route /posts/123 now includes test content from the Home controller. "
            "Use this to verify request context, hydration match, and route contracts.";
        post["likes"] = 321;
        post["readMinutes"] = 6;
        post["tags"] = Json::arrayValue;
        post["tags"].append("routing");
        post["tags"].append("controller");
        post["tags"].append("hydration");
    } else if (postId == "456") {
        post["title"] = "Post 456: Alternate route payload";
        post["author"] = "Hydra Bench Bot";
        post["summary"] = "A second seeded route to make data differences obvious.";
        post["body"] =
            "This route intentionally returns different values so /posts/123 and /posts/456 "
            "render visibly different SSR output.";
        post["publishedAt"] = "2026-02-16";
        post["readMinutes"] = 2;
        post["likes"] = 42;
        post["tags"] = Json::arrayValue;
        post["tags"].append("alternate");
        post["tags"].append("route-contract");
        post["tags"].append("sample-data");
    }

    return post;
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
        props["post"] = buildDemoPost(postId);
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
            "C++ hot reload: run hydra dev; Vite HMR: edits under ui/src/*";

        callback(drogon::HttpResponse::newHttpJsonResponse(payload));
    }

    void metrics(const drogon::HttpRequestPtr &,
                 HttpCallback &&callback) const {
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k501NotImplemented);
        response->setContentTypeString("text/plain; charset=utf-8");
        response->setBody("# Hydra metrics API is unavailable in this build\n");
        callback(response);
    }

  private:
    void renderPage(const drogon::HttpRequestPtr &req,
                    HttpCallback &&callback,
                    Json::Value props) const {
        auto hydra = drogon::app().getPlugin<hydra::HydraSsrPlugin>();
        const auto html = hydra->render(req, props);

        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k200OK);
        response->setContentTypeCode(drogon::CT_TEXT_HTML);
        response->setBody(html);
        callback(response);
    }
};

}  // namespace app::controllers
