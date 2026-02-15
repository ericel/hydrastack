#include "hydra/HtmlShell.h"

#include <sstream>

namespace hydra {
namespace {

std::string deriveReactRefreshPath(const std::string &hmrClientPath) {
    static constexpr std::string_view kViteClientSuffix = "/@vite/client";
    const auto suffixPos = hmrClientPath.rfind(kViteClientSuffix);
    if (suffixPos == std::string::npos ||
        suffixPos + kViteClientSuffix.size() != hmrClientPath.size()) {
        return {};
    }

    return hmrClientPath.substr(0, suffixPos) + "/@react-refresh";
}

std::string escapeForJsString(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const auto ch : value) {
        switch (ch) {
            case '\\':
                escaped.append("\\\\");
                break;
            case '"':
                escaped.append("\\\"");
                break;
            case '\n':
                escaped.append("\\n");
                break;
            case '\r':
                escaped.append("\\r");
                break;
            case '\t':
                escaped.append("\\t");
                break;
            case '<':
                escaped.append("\\u003c");
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
}

std::string nonceAttribute(const HtmlShellAssets &assets) {
    if (assets.scriptNonce.empty()) {
        return {};
    }
    return " nonce=\"" + assets.scriptNonce + "\"";
}

}  // namespace

std::string HtmlShell::wrap(const std::string &appHtml,
                            const std::string &propsJson,
                            const HtmlShellAssets &assets) {
    std::ostringstream html;
    html << "<!doctype html>\n"
         << "<html lang=\"en\">\n"
         << "  <head>\n"
         << "    <meta charset=\"utf-8\" />\n"
         << "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />\n"
         << "    <title>HydraStack</title>\n";

    if (!assets.cssPath.empty()) {
        html << "    <link rel=\"stylesheet\" href=\"" << assets.cssPath << "\" />\n";
    }

    const auto nonceAttr = nonceAttribute(assets);

    html << "  </head>\n"
         << "  <body>\n"
         << "    <div id=\"root\">" << appHtml << "</div>\n"
         << "    <script id=\"__HYDRA_PROPS__\" type=\"application/json\"" << nonceAttr << ">"
         << escapeForScriptTag(propsJson) << "</script>\n";

    if (!assets.hmrClientPath.empty()) {
        if (const auto reactRefreshPath = deriveReactRefreshPath(assets.hmrClientPath);
            !reactRefreshPath.empty()) {
            html << "    <script type=\"module\"" << nonceAttr << ">\n"
                 << "      import RefreshRuntime from \"" << reactRefreshPath << "\";\n"
                 << "      RefreshRuntime.injectIntoGlobalHook(window);\n"
                 << "      window.$RefreshReg$ = () => {};\n"
                 << "      window.$RefreshSig$ = () => (type) => type;\n"
                 << "      window.__vite_plugin_react_preamble_installed__ = true;\n"
                 << "    </script>\n";
        }

        html << "    <script type=\"module\" src=\"" << assets.hmrClientPath << "\""
             << nonceAttr << "></script>\n";
    }

    if (!assets.clientJsPath.empty()) {
        if (assets.clientJsModule) {
            html << "    <script type=\"module\" src=\"" << assets.clientJsPath
                 << "\"" << nonceAttr << "></script>\n";
        } else {
            html << "    <script src=\"" << assets.clientJsPath << "\" defer"
                 << nonceAttr << "></script>\n";
        }
    }

    if (!assets.devReloadProbePath.empty() && assets.devReloadIntervalMs > 0) {
        html << "    <script" << nonceAttr << ">\n"
             << "      (() => {\n"
             << "        const probePath = \"" << escapeForJsString(assets.devReloadProbePath)
             << "\";\n"
             << "        const intervalMs = " << assets.devReloadIntervalMs << ";\n"
             << "        let lastProcessStartedMs = 0;\n"
             << "        let sawServerUnavailable = false;\n"
             << "\n"
             << "        const poll = async () => {\n"
             << "          try {\n"
             << "            const separator = probePath.includes(\"?\") ? \"&\" : \"?\";\n"
             << "            const response = await fetch(`${probePath}${separator}"
                "__hydra_reload_ts=${Date.now()}`, {\n"
             << "              cache: \"no-store\",\n"
             << "              credentials: \"same-origin\"\n"
             << "            });\n"
             << "            if (!response.ok) {\n"
             << "              sawServerUnavailable = true;\n"
             << "              return;\n"
             << "            }\n"
             << "\n"
             << "            const payload = await response.json();\n"
             << "            const current = Number(payload.process_started_ms ?? 0);\n"
             << "            if (!Number.isFinite(current) || current <= 0) {\n"
             << "              return;\n"
             << "            }\n"
             << "\n"
             << "            if (lastProcessStartedMs === 0) {\n"
             << "              lastProcessStartedMs = current;\n"
             << "              if (sawServerUnavailable) {\n"
             << "                window.location.reload();\n"
             << "                return;\n"
             << "              }\n"
             << "              sawServerUnavailable = false;\n"
             << "              return;\n"
             << "            }\n"
             << "\n"
             << "            if (current !== lastProcessStartedMs || sawServerUnavailable) {\n"
             << "              window.location.reload();\n"
             << "              return;\n"
             << "            }\n"
             << "\n"
             << "            sawServerUnavailable = false;\n"
             << "          } catch (error) {\n"
             << "            sawServerUnavailable = true;\n"
             << "          }\n"
             << "        };\n"
             << "\n"
             << "        window.setInterval(() => {\n"
             << "          void poll();\n"
             << "        }, intervalMs);\n"
             << "        void poll();\n"
             << "      })();\n"
             << "    </script>\n";
    }

    html << "  </body>\n"
         << "</html>\n";
    return html.str();
}

std::string HtmlShell::errorPage(const std::string &message) {
    std::ostringstream html;
    html << "<!doctype html>\n"
         << "<html lang=\"en\">\n"
         << "  <head><meta charset=\"utf-8\" /><title>HydraStack Error</title></head>\n"
         << "  <body>\n"
         << "    <h1>HydraStack SSR Error</h1>\n"
         << "    <pre>" << escapeForScriptTag(message) << "</pre>\n"
         << "  </body>\n"
         << "</html>\n";
    return html.str();
}

std::string HtmlShell::escapeForScriptTag(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());

    for (const char ch : value) {
        switch (ch) {
            case '<':
                escaped.append("\\u003c");
                break;
            case '>':
                escaped.append("\\u003e");
                break;
            case '&':
                escaped.append("\\u0026");
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }

    return escaped;
}

}  // namespace hydra
