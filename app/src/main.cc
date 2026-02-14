#include <drogon/drogon.h>

int main() {
#ifdef HYDRA_PUBLIC_DIR
    drogon::app().setDocumentRoot(HYDRA_PUBLIC_DIR);
#endif

    drogon::app().loadConfigFile("app/config.json");
    drogon::app().run();
    return 0;
}
