// Runs engine/test/ssr_runtime_globals.js inside the real SSR runtime and
// fails if any web-platform conformance check fails.
//
// Usage: hydra_ssr_runtime_globals_test <path/to/ssr_runtime_globals.js>

#include "hydra/V8Platform.h"
#include "hydra/V8SsrRuntime.h"

#include <json/reader.h>
#include <json/writer.h>

#include <iostream>
#include <sstream>
#include <string>

namespace {

hydra::V8SsrRuntime::BridgeResponse echoBridge(const hydra::V8SsrRuntime::BridgeRequest &request) {
    hydra::V8SsrRuntime::BridgeResponse response;
    if (request.path.rfind("/api/status/", 0) == 0) {
        response.status = std::stoi(request.path.substr(std::string("/api/status/").size()));
        response.body = "status";
        return response;
    }
    Json::Value body(Json::objectValue);
    body["method"] = request.method;
    body["path"] = request.path;
    body["query"] = request.query;
    body["body"] = request.body;
    const auto header = request.headers.find("x-a");
    if (header != request.headers.end()) {
        body["x_a"] = header->second;
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    response.body = Json::writeString(writer, body);
    response.headers["content-type"] = "application/json";
    return response;
}

bool report(const std::string &phase, const std::string &json) {
    Json::Value result;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream stream(json);
    if (!Json::parseFromStream(reader, stream, &result, &errors)) {
        std::cerr << "[ssr_runtime_globals] " << phase << ": unreadable result: " << json << '\n';
        return false;
    }
    const auto &failures = result["failures"];
    for (const auto &failure : failures) {
        std::cerr << "[ssr_runtime_globals] FAIL " << failure.asString() << '\n';
    }
    std::cout << "[ssr_runtime_globals] " << phase << ": " << result["passed"].asInt() << " passed, "
              << failures.size() << " failed\n";
    return failures.empty();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <ssr_runtime_globals.js>\n";
        return 2;
    }
    hydra::V8Platform::initialize();
    bool ok = false;
    try {
        hydra::V8SsrRuntime runtime(argv[1], echoBridge);
        // Promise callbacks run once the outermost render call returns, so the
        // second render reports the async checks started by the first.
        const bool syncOk = report("sync", runtime.render("/sync", "{}"));
        ok = report("sync+async", runtime.render("/async", "{}")) && syncOk;
    } catch (const std::exception &error) {
        std::cerr << "[ssr_runtime_globals] runtime error: " << error.what() << '\n';
    }
    hydra::V8Platform::shutdown();
    return ok ? 0 : 1;
}
