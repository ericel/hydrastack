#pragma once

#include "hydra/V8SsrRuntime.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace hydra {

class V8IsolatePool {
  public:
    class Lease {
      public:
        Lease() = default;
        Lease(V8IsolatePool *pool, std::size_t runtimeIndex);
        ~Lease();

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;

        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;

        [[nodiscard]] V8SsrRuntime *operator->() const;
        [[nodiscard]] V8SsrRuntime &runtime() const;
        void markForRecycle();

      private:
        void release();

        V8IsolatePool *pool_ = nullptr;
        std::size_t runtimeIndex_ = 0;
        bool recycle_ = false;
    };

    V8IsolatePool(std::size_t size,
                  std::string bundlePath,
                  std::uint64_t renderTimeoutMs);

    [[nodiscard]] Lease acquire(std::uint64_t acquireTimeoutMs = 0);
    [[nodiscard]] std::uint64_t renderTimeoutMs() const;

  private:
    friend class Lease;

    void release(std::size_t runtimeIndex);
    void recycle(std::size_t runtimeIndex) noexcept;

    std::vector<std::unique_ptr<V8SsrRuntime>> runtimes_;
    std::queue<std::size_t> availableRuntimes_;
    std::string bundlePath_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t renderTimeoutMs_ = 0;
};

}  // namespace hydra
