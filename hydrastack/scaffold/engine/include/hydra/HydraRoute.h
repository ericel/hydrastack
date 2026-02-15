#pragma once

#include <json/value.h>

#include <string>

namespace hydra {

class HydraRoute final {
  public:
    template <typename StringMap>
    static Json::Value toJsonObject(const StringMap &values) {
        Json::Value out(Json::objectValue);
        for (const auto &[key, value] : values) {
            out[key] = value;
        }
        return out;
    }

    static void set(Json::Value &props,
                    const std::string &pageId,
                    const Json::Value &params = Json::Value(Json::objectValue),
                    const Json::Value &query = Json::Value(Json::objectValue),
                    const std::string &routePath = {},
                    const std::string &routeUrl = {}) {
        Json::Value route(Json::objectValue);
        route["pageId"] = pageId;
        route["params"] = params.isObject() ? params : Json::Value(Json::objectValue);
        route["query"] = query.isObject() ? query : Json::Value(Json::objectValue);
        route["routePath"] = routePath;
        route["routeUrl"] = routeUrl;

        props["__hydra_route"] = std::move(route);
    }
};

}  // namespace hydra
