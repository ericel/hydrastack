#include "hydra/HydraRoute.h"
#include "hydra/HydraSsrPlugin.h"

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <json/reader.h>
#include <json/value.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

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

struct UploadCleanupPolicy {
    std::filesystem::path uploadPath{"./uploads"};
    bool cleanupOnStartup{true};
    bool removeEmptyDirs{false};
    std::chrono::hours maxAge{std::chrono::hours(24 * 7)};

    std::filesystem::path tmpPath() const {
        return uploadPath / "tmp";
    }
};

bool isIgnorableRemoveError(const std::error_code &error) {
    return error == std::make_error_code(std::errc::directory_not_empty) ||
           error == std::make_error_code(std::errc::no_such_file_or_directory);
}

bool isDrogonFanoutDirectory(const std::filesystem::path &dirPath,
                             const std::filesystem::path &tmpRoot) {
    if (dirPath.parent_path() != tmpRoot) {
        return false;
    }

    const auto name = dirPath.filename().string();
    if (name.size() != 2) {
        return false;
    }

    return std::isxdigit(static_cast<unsigned char>(name[0])) &&
           std::isxdigit(static_cast<unsigned char>(name[1]));
}

bool readBoolOrDefault(const Json::Value &node,
                       const char *key,
                       bool defaultValue) {
    if (!node.isObject() || !node.isMember(key)) {
        return defaultValue;
    }
    const auto &value = node[key];
    if (!value.isBool()) {
        return defaultValue;
    }
    return value.asBool();
}

std::int64_t readNonNegativeIntOrDefault(const Json::Value &node,
                                         const char *key,
                                         std::int64_t defaultValue) {
    if (!node.isObject() || !node.isMember(key)) {
        return defaultValue;
    }

    const auto &value = node[key];
    if (value.isInt64()) {
        const auto parsed = value.asInt64();
        return parsed >= 0 ? parsed : defaultValue;
    }
    if (value.isUInt64()) {
        return static_cast<std::int64_t>(value.asUInt64());
    }
    if (value.isInt()) {
        const auto parsed = value.asInt();
        return parsed >= 0 ? parsed : defaultValue;
    }
    if (value.isUInt()) {
        return static_cast<std::int64_t>(value.asUInt());
    }
    return defaultValue;
}

std::int64_t resolveDefaultUploadRetentionHours(const Json::Value &root) {
    if (!root.isObject()) {
        return 24 * 7;
    }

    const auto &plugins = root["plugins"];
    if (!plugins.isArray()) {
        return 24 * 7;
    }

    for (const auto &plugin : plugins) {
        if (!plugin.isObject() || plugin["name"].asString() != "hydra::HydraSsrPlugin") {
            continue;
        }

        const auto &pluginConfig = plugin["config"];
        if (!pluginConfig.isObject()) {
            continue;
        }

        const auto assetMode = toLowerCopy(pluginConfig["asset_mode"].asString());
        if (assetMode == "dev") {
            return 24;
        }

        const auto devModeEnabled = readBoolOrDefault(pluginConfig["dev_mode"], "enabled", false);
        if (assetMode == "auto" && devModeEnabled) {
            return 24;
        }
        if (devModeEnabled) {
            return 24;
        }
    }

    return 24 * 7;
}

UploadCleanupPolicy loadUploadCleanupPolicy(const std::string &configPath) {
    UploadCleanupPolicy policy;

    Json::Value root;
    {
        std::ifstream configStream(configPath);
        if (!configStream.is_open()) {
            LOG_WARN << "HydraUploads | unable to open config: " << configPath
                     << " | using defaults";
            return policy;
        }

        Json::CharReaderBuilder builder;
        builder["collectComments"] = false;
        std::string errors;
        if (!Json::parseFromStream(builder, configStream, &root, &errors)) {
            LOG_WARN << "HydraUploads | unable to parse config: " << configPath
                     << " | using defaults"
                     << " | parse_error=" << errors;
            return policy;
        }
    }

    if (root.isMember("upload_path") && root["upload_path"].isString() &&
        !root["upload_path"].asString().empty()) {
        policy.uploadPath = root["upload_path"].asString();
    }

    const auto defaultRetentionHours = resolveDefaultUploadRetentionHours(root);
    policy.maxAge = std::chrono::hours(defaultRetentionHours);

    const auto &cleanupConfig = root["hydra_uploads"];
    policy.cleanupOnStartup =
        readBoolOrDefault(cleanupConfig, "cleanup_on_startup", policy.cleanupOnStartup);
    policy.removeEmptyDirs =
        readBoolOrDefault(cleanupConfig, "remove_empty_dirs", policy.removeEmptyDirs);
    policy.maxAge = std::chrono::hours(
        readNonNegativeIntOrDefault(cleanupConfig, "max_age_hours", defaultRetentionHours));

    return policy;
}

void applyUploadCleanupPolicy(const UploadCleanupPolicy &policy) {
    namespace fs = std::filesystem;
    const auto tmpRoot = policy.tmpPath();

    if (!policy.cleanupOnStartup) {
        LOG_INFO << "HydraUploads | cleanup_on_startup=off"
                 << " | upload_path=" << policy.uploadPath.string()
                 << " | tmp_path=" << tmpRoot.string()
                 << " | max_age_hours=" << policy.maxAge.count();
        return;
    }

    std::error_code ec;
    if (!fs::exists(tmpRoot, ec)) {
        if (ec) {
            LOG_ERROR << "HydraUploads | tmp_path_stat_failed"
                      << " | tmp_path=" << tmpRoot.string()
                      << " | error=" << ec.message();
            return;
        }
        LOG_INFO << "HydraUploads | cleanup_on_startup=on"
                 << " | tmp_path_missing=" << tmpRoot.string()
                 << " | max_age_hours=" << policy.maxAge.count();
        return;
    }

    std::vector<fs::path> filesToRemove;
    std::vector<fs::path> directories;
    std::size_t scanErrors = 0;

    const auto now = fs::file_time_type::clock::now();
    const auto maxAge = std::chrono::duration_cast<fs::file_time_type::duration>(
        std::chrono::hours(policy.maxAge.count()));

    fs::recursive_directory_iterator iter(
        tmpRoot, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        LOG_ERROR << "HydraUploads | scan_failed"
                  << " | tmp_path=" << tmpRoot.string()
                  << " | error=" << ec.message();
        return;
    }

    const fs::recursive_directory_iterator endIter;
    for (; iter != endIter; iter.increment(ec)) {
        if (ec) {
            ++scanErrors;
            ec.clear();
            continue;
        }

        const auto &entry = *iter;
        const auto entryPath = entry.path();

        std::error_code statusEc;
        const auto status = entry.symlink_status(statusEc);
        if (statusEc) {
            ++scanErrors;
            continue;
        }

        if (fs::is_directory(status)) {
            directories.push_back(entryPath);
            continue;
        }
        if (!fs::is_regular_file(status)) {
            continue;
        }

        std::error_code timeEc;
        const auto lastWrite = entry.last_write_time(timeEc);
        if (timeEc) {
            ++scanErrors;
            continue;
        }

        if ((now - lastWrite) > maxAge) {
            filesToRemove.push_back(entryPath);
        }
    }

    std::size_t removedFiles = 0;
    std::size_t removeErrors = 0;
    for (const auto &filePath : filesToRemove) {
        std::error_code removeEc;
        const bool removed = fs::remove(filePath, removeEc);
        if (removed) {
            ++removedFiles;
            continue;
        }
        if (removeEc && !isIgnorableRemoveError(removeEc)) {
            ++removeErrors;
        }
    }

    std::size_t removedDirs = 0;
    if (policy.removeEmptyDirs) {
        std::sort(directories.begin(), directories.end(), [](const fs::path &left,
                                                              const fs::path &right) {
            return left.native().size() > right.native().size();
        });
        for (const auto &dirPath : directories) {
            if (isDrogonFanoutDirectory(dirPath, tmpRoot)) {
                continue;
            }
            std::error_code removeEc;
            const bool removed = fs::remove(dirPath, removeEc);
            if (removed) {
                ++removedDirs;
                continue;
            }
            if (removeEc && !isIgnorableRemoveError(removeEc)) {
                ++removeErrors;
            }
        }
    }

    LOG_INFO << "HydraUploads | cleanup_on_startup=on"
             << " | upload_path=" << policy.uploadPath.string()
             << " | tmp_path=" << tmpRoot.string()
             << " | max_age_hours=" << policy.maxAge.count()
             << " | removed_files=" << removedFiles
             << " | removed_dirs=" << removedDirs
             << " | scan_errors=" << scanErrors
             << " | remove_errors=" << removeErrors;
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
                auto response = drogon::HttpResponse::newHttpResponse();
                response->setStatusCode(status);
                response->setContentTypeCode(drogon::CT_TEXT_HTML);
                response->setBody(hydra->render(req, buildErrorProps(status, req)));
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

    auto resolveCompatibilityConfigPath = [](std::string path) {
        namespace fs = std::filesystem;
        if (path.empty() || fs::exists(path)) {
            return path;
        }

        std::vector<std::string> candidates;
        if (path.rfind("app/", 0) == 0) {
            candidates.push_back("demo/" + path.substr(4));
        } else if (path.rfind("demo/", 0) == 0) {
            candidates.push_back("app/" + path.substr(5));
        }

        const auto appPos = path.find("/app/");
        if (appPos != std::string::npos) {
            auto candidate = path;
            candidate.replace(appPos, std::string("/app/").size(), "/demo/");
            candidates.push_back(std::move(candidate));
        }

        const auto demoPos = path.find("/demo/");
        if (demoPos != std::string::npos) {
            auto candidate = path;
            candidate.replace(demoPos, std::string("/demo/").size(), "/app/");
            candidates.push_back(std::move(candidate));
        }

        for (const auto &candidate : candidates) {
            if (fs::exists(candidate)) {
                return candidate;
            }
        }

        return path;
    };

    std::string configPath = "app/config.json";
    if (const char *configFromEnv = std::getenv("HYDRA_CONFIG");
        configFromEnv != nullptr && *configFromEnv != '\0') {
        configPath = configFromEnv;
    }
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
        configPath = argv[1];
    }
    configPath = resolveCompatibilityConfigPath(std::move(configPath));

    applyUploadCleanupPolicy(loadUploadCleanupPolicy(configPath));
    drogon::app().loadConfigFile(configPath);
    installHydraErrorHandler();
    drogon::app().run();
    return 0;
}
