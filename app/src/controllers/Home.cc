#include "hydra/HydraSsrPlugin.h"

#include <drogon/HttpController.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <json/value.h>

#include <chrono>
#include <cstdint>

namespace demo::controllers {
namespace {
const auto kProcessStartedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch())
                                   .count();
}  // namespace

class Home final : public drogon::HttpController<Home> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(Home::index, "/", drogon::Get);
    ADD_METHOD_TO(Home::test, "/__hydra/test", drogon::Get);
    METHOD_LIST_END

    void index(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
        Json::Value props;
        const std::string path = req->path().empty() ? "/" : req->path();
        std::string pathWithQuery = path;
        if (!req->query().empty()) {
            pathWithQuery.push_back('?');
            pathWithQuery.append(req->query());
        }

        props["page"] = "home";
        props["message"] = "Hello from HydraStack app";
        props["path"] = path;
        props["pathWithQuery"] = pathWithQuery;

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

        auto hydra = drogon::app().getPlugin<hydra::HydraSsrPlugin>();
        const auto html = hydra->render(req, props);

        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k200OK);
        response->setContentTypeCode(drogon::CT_TEXT_HTML);
        response->setBody(html);
        callback(response);
    }

    void test(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
        Json::Value payload;
        payload["ok"] = true;
        payload["service"] = "hydra_demo";
        payload["path"] = req->path();
        payload["query"] = req->query();
        payload["process_started_ms"] = static_cast<Json::Int64>(kProcessStartedMs);
        payload["tip"] =
            "C++ hot reload: working scripts/dev.sh; Vite HMR: edits under ui/src/*";

        callback(drogon::HttpResponse::newHttpJsonResponse(payload));
    }
};

}  // namespace demo::controllers
