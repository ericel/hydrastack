#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace hydra {

struct HtmlShellAssets {
    std::string cssPath = "/assets/app.css";
    std::string clientJsPath = "/assets/client.js";
    std::string hmrClientPath;
    std::string scriptNonce;
    bool clientJsModule = false;
    std::string devReloadProbePath;
    std::uint64_t devReloadIntervalMs = 0;
};

class HtmlShell {
  public:
    [[nodiscard]] static std::string wrap(const std::string &appHtml,
                                          const std::string &propsJson,
                                          const HtmlShellAssets &assets);

    [[nodiscard]] static std::string errorPage(const std::string &message);
    [[nodiscard]] static std::string escapeForScriptTag(std::string_view value);
};

}  // namespace hydra
