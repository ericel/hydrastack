#include "hydra/HydraSsrPlugin.h"

#include <drogon/HttpController.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <json/value.h>

#include <cstdint>

namespace demo::controllers {

class Home final : public drogon::HttpController<Home> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(Home::index, "/", drogon::Get);
    METHOD_LIST_END

    void index(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
        Json::Value props;
        props["page"] = "home";
        props["message"] = "Hello from HydraStack";
        props["path"] = req->path();

        const auto burnMs = req->getOptionalParameter<int>("burn_ms").value_or(0);
        const auto showCounter = req->getOptionalParameter<int>("counter").value_or(0);
        if (burnMs > 0 || showCounter > 0) {
            Json::Value testConfig;
            if (burnMs > 0) {
                testConfig["burnMs"] = static_cast<Json::Int64>(burnMs);
            }
            if (showCounter > 0) {
                testConfig["counter"] = true;
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
};

}  // namespace demo::controllers
