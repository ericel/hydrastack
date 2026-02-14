#include <drogon/drogon.h>

#include <cstdlib>
#include <string>

int main(int argc, char *argv[]) {
#ifdef HYDRA_PUBLIC_DIR
    drogon::app().setDocumentRoot(HYDRA_PUBLIC_DIR);
#endif

    std::string configPath = "app/config.json";
    if (const char *configFromEnv = std::getenv("HYDRA_CONFIG");
        configFromEnv != nullptr && *configFromEnv != '\0') {
        configPath = configFromEnv;
    }
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
        configPath = argv[1];
    }

    drogon::app().loadConfigFile(configPath);
    drogon::app().run();
    return 0;
}
