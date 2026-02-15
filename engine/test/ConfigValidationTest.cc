#include "hydra/Config.h"

#include <json/value.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expectThrows(const std::function<void()> &fn, const std::string &label) {
    try {
        fn();
    } catch (const std::exception &) {
        return;
    }

    throw std::runtime_error("expected exception for: " + label);
}

void expectTrue(bool condition, const std::string &label) {
    if (!condition) {
        throw std::runtime_error("assertion failed: " + label);
    }
}

Json::Value makeBaseConfig(const std::string &assetMode) {
    Json::Value config(Json::objectValue);
    config["asset_mode"] = assetMode;
    config["acquire_timeout_ms"] = 200;
    config["render_timeout_ms"] = 50;

    Json::Value devMode(Json::objectValue);
    devMode["enabled"] = assetMode == "dev";
    devMode["vite_origin"] = "http://127.0.0.1:5174";
    devMode["client_entry_path"] = "/src/entry-client.tsx";
    devMode["css_path"] = "/src/styles.css";
    devMode["proxy_timeout_sec"] = 10.0;
    devMode["reload_interval_ms"] = 1000;
    config["dev_mode"] = devMode;

    return config;
}

std::filesystem::path writeManifestFile() {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                ("hydra-config-test-manifest-" + std::to_string(now) + ".json");
    std::ofstream out(path);
    out << "{}";
    out.close();
    return path;
}

}  // namespace

int main() {
    try {
        const auto manifestPath = writeManifestFile();

        {
            auto config = makeBaseConfig("prod");
            config["manifest_path"] = manifestPath.string();
            const auto normalized = hydra::validateAndNormalizeHydraSsrPluginConfig(config);
            expectTrue(normalized.devModeEnabled == false, "prod mode resolved");
            expectTrue(normalized.renderTimeoutMs == 50, "render timeout preserved");
        }

        {
            auto config = makeBaseConfig("dev");
            const auto normalized = hydra::validateAndNormalizeHydraSsrPluginConfig(config);
            expectTrue(normalized.devModeEnabled == true, "dev mode resolved");
            expectTrue(normalized.resolvedAssetMode == "dev", "resolved asset mode");
        }

        {
            auto config = makeBaseConfig("prod");
            config["manifest_path"] = "/tmp/does-not-exist-manifest.json";
            expectThrows(
                [&]() { (void)hydra::validateAndNormalizeHydraSsrPluginConfig(config); },
                "missing manifest in prod");
        }

        {
            auto config = makeBaseConfig("invalid");
            expectThrows(
                [&]() { (void)hydra::validateAndNormalizeHydraSsrPluginConfig(config); },
                "invalid asset mode");
        }

        {
            auto config = makeBaseConfig("dev");
            config["render_timeout_ms"] = 0;
            expectThrows(
                [&]() { (void)hydra::validateAndNormalizeHydraSsrPluginConfig(config); },
                "invalid render timeout");
        }

        {
            auto config = makeBaseConfig("dev");
            config["dev_mode"]["vite_origin"] = "127.0.0.1:5174";
            expectThrows(
                [&]() { (void)hydra::validateAndNormalizeHydraSsrPluginConfig(config); },
                "invalid vite origin");
        }

        {
            auto config = makeBaseConfig("dev");
            config["dev_mode"]["mystery_key"] = true;
            expectThrows(
                [&]() { (void)hydra::validateAndNormalizeHydraSsrPluginConfig(config); },
                "unknown dev_mode key");
        }

        std::filesystem::remove(manifestPath);
        std::cout << "[config-test] PASS\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "[config-test] FAIL: " << ex.what() << '\n';
        return 1;
    }
}
