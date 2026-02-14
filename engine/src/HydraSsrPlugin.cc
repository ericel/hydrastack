#include "hydra/HydraSsrPlugin.h"

#include "hydra/HtmlShell.h"
#include "hydra/V8IsolatePool.h"
#include "hydra/V8Platform.h"

#include <drogon/drogon.h>
#include <json/writer.h>

#include <algorithm>
#include <stdexcept>

namespace hydra {
namespace {

std::string toCompactJson(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    return Json::writeString(builder, value);
}

bool isLikelyFullDocument(const std::string &html) {
    return html.find("<html") != std::string::npos ||
           html.find("<!doctype") != std::string::npos ||
           html.find("<!DOCTYPE") != std::string::npos;
}

}  // namespace

HydraSsrPlugin::~HydraSsrPlugin() = default;

void HydraSsrPlugin::initAndStart(const Json::Value &config) {
    ssrBundlePath_ = config.get("ssr_bundle_path", "./public/assets/ssr-bundle.js").asString();
    cssPath_ = config.get("css_path", "/assets/app.css").asString();
    clientJsPath_ = config.get("client_js_path", "/assets/client.js").asString();
    renderTimeoutMs_ = config.get("render_timeout_ms", 50).asUInt64();
    wrapFragment_ = config.get("wrap_fragment", false).asBool();

    const auto configuredPoolSize = config.get("isolate_pool_size", 0).asUInt();
    const auto threadCount = std::max<std::size_t>(1, drogon::app().getThreadNum());
    isolatePoolSize_ =
        configuredPoolSize > 0 ? static_cast<std::size_t>(configuredPoolSize) : threadCount;

    V8Platform::initialize();
    try {
        isolatePool_ =
            std::make_unique<V8IsolatePool>(isolatePoolSize_, ssrBundlePath_, renderTimeoutMs_);
    } catch (...) {
        V8Platform::shutdown();
        throw;
    }

    LOG_INFO << "HydraSsrPlugin initialized with bundle: " << ssrBundlePath_
             << ", pool size: " << isolatePoolSize_;
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

    const std::string url = !options.urlOverride.empty()
                                ? options.urlOverride
                                : (req ? req->path() : std::string("/"));

    try {
        auto lease = isolatePool_->acquire();
        auto html = lease->render(url, propsJson, isolatePool_->renderTimeoutMs());

        if (wrapFragment_ && !isLikelyFullDocument(html)) {
            HtmlShellAssets assets;
            assets.cssPath = cssPath_;
            assets.clientJsPath = clientJsPath_;
            return HtmlShell::wrap(
                html,
                propsJson,
                assets);
        }

        return html;
    } catch (const std::exception &ex) {
        LOG_ERROR << "HydraStack render failed for url=" << url << ": " << ex.what();
        return HtmlShell::errorPage(ex.what());
    }
}

}  // namespace hydra
