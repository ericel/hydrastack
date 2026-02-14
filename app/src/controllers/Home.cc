#include "hydra/HydraSsrPlugin.h"

#include <drogon/HttpController.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <json/value.h>

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
