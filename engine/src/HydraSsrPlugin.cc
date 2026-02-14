#include "hydra/HydraSsrPlugin.h"

#include "hydra/HtmlShell.h"
#include "hydra/V8IsolatePool.h"
#include "hydra/V8Platform.h"

#include <drogon/drogon.h>
#include <json/reader.h>
#include <json/writer.h>

#include <algorithm>
#include <fstream>
#include <optional>
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

}  // namespace

HydraSsrPlugin::~HydraSsrPlugin() = default;

void HydraSsrPlugin::initAndStart(const Json::Value &config) {
    ssrBundlePath_ = config.get("ssr_bundle_path", "./public/assets/ssr-bundle.js").asString();
    cssPath_ = config.get("css_path", "").asString();
    clientJsPath_ = config.get("client_js_path", "").asString();
    assetManifestPath_ = config.get("asset_manifest_path", "").asString();
    if (assetManifestPath_.empty()) {
        assetManifestPath_ = config.get("manifest_path", "./public/assets/manifest.json").asString();
    }
    assetPublicPrefix_ = config.get("asset_public_prefix", "/assets").asString();
    clientManifestEntry_ = config.get("client_manifest_entry", "").asString();
    if (clientManifestEntry_.empty()) {
        clientManifestEntry_ = config.get("client_entry_key", "src/entry-client.tsx").asString();
    }
    isolateAcquireTimeoutMs_ = config.get("acquire_timeout_ms", 0).asUInt64();
    renderTimeoutMs_ = config.get("render_timeout_ms", 50).asUInt64();
    wrapFragment_ = config.get("wrap_fragment", true).asBool();

    if (auto manifestAssets = resolveAssetsFromManifest(
            assetManifestPath_, assetPublicPrefix_, clientManifestEntry_)) {
        if (cssPath_.empty()) {
            cssPath_ = manifestAssets->cssPath;
        }
        if (clientJsPath_.empty()) {
            clientJsPath_ = manifestAssets->clientJsPath;
        }
    }

    if (cssPath_.empty()) {
        cssPath_ = "/assets/app.css";
        LOG_WARN << "HydraStack falling back to default css path: " << cssPath_;
    }
    if (clientJsPath_.empty()) {
        clientJsPath_ = "/assets/client.js";
        LOG_WARN << "HydraStack falling back to default client path: " << clientJsPath_;
    }

    const auto configuredPoolSize = config.isMember("pool_size")
                                        ? config["pool_size"].asUInt()
                                        : config.get("isolate_pool_size", 0).asUInt();
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
             << ", pool size: " << isolatePoolSize_
             << ", acquire timeout(ms): " << isolateAcquireTimeoutMs_
             << ", css: " << cssPath_
             << ", client: " << clientJsPath_;
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
        auto lease = isolatePool_->acquire(isolateAcquireTimeoutMs_);
        try {
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
        } catch (...) {
            lease.markForRecycle();
            throw;
        }
    } catch (const std::exception &ex) {
        LOG_ERROR << "HydraStack render failed for url=" << url << ": " << ex.what();
        return HtmlShell::errorPage(ex.what());
    } catch (...) {
        LOG_ERROR << "HydraStack render failed for url=" << url
                  << ": unknown exception";
        return HtmlShell::errorPage("Unknown SSR runtime error");
    }
}

}  // namespace hydra
