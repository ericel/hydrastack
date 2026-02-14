#include "hydra/HtmlShell.h"

#include <sstream>

namespace hydra {

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

    html << "  </head>\n"
         << "  <body>\n"
         << "    <div id=\"root\">" << appHtml << "</div>\n"
         << "    <script id=\"__HYDRA_PROPS__\" type=\"application/json\">"
         << escapeForScriptTag(propsJson) << "</script>\n";

    if (!assets.clientJsPath.empty()) {
        html << "    <script src=\"" << assets.clientJsPath << "\" defer></script>\n";
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
