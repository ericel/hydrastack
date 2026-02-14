#include "hydra/V8Platform.h"

#include <libplatform/libplatform.h>
#include <v8.h>

#include <array>
#include <memory>
#include <mutex>
#include <string>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace hydra {
namespace {

std::mutex gPlatformMutex;
std::unique_ptr<v8::Platform> gPlatform;
std::size_t gPlatformRefCount = 0;

std::string executablePath() {
#if defined(__APPLE__)
    uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) {
        return ".";
    }

    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) == 0) {
        if (!path.empty() && path.back() == '\0') {
            path.pop_back();
        }
        return path;
    }
#elif defined(__linux__)
    std::array<char, 4096> pathBuffer{};
    const auto length = readlink("/proc/self/exe", pathBuffer.data(), pathBuffer.size() - 1);
    if (length > 0) {
        pathBuffer[static_cast<std::size_t>(length)] = '\0';
        return std::string(pathBuffer.data(), static_cast<std::size_t>(length));
    }
#endif

    return ".";
}

}  // namespace

void V8Platform::initialize() {
    std::lock_guard<std::mutex> lock(gPlatformMutex);
    if (gPlatformRefCount++ > 0) {
        return;
    }

    const std::string exePath = executablePath();
    v8::V8::InitializeICUDefaultLocation(exePath.c_str());
    v8::V8::InitializeExternalStartupData(exePath.c_str());
    gPlatform = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(gPlatform.get());
    v8::V8::Initialize();
}

void V8Platform::shutdown() {
    std::lock_guard<std::mutex> lock(gPlatformMutex);
    if (gPlatformRefCount == 0) {
        return;
    }

    --gPlatformRefCount;
    if (gPlatformRefCount > 0) {
        return;
    }

    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    gPlatform.reset();
}

}  // namespace hydra
