#include <drogon/drogon.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

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

    std::string configPath = "demo/config.json";
    if (const char *configFromEnv = std::getenv("HYDRA_CONFIG");
        configFromEnv != nullptr && *configFromEnv != '\0') {
        configPath = configFromEnv;
    }
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
        configPath = argv[1];
    }
    configPath = resolveCompatibilityConfigPath(std::move(configPath));

    drogon::app().loadConfigFile(configPath);
    drogon::app().run();
    return 0;
}
