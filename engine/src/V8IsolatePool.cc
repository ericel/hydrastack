#include "hydra/V8IsolatePool.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace hydra {

V8IsolatePool::Lease::Lease(V8IsolatePool *pool, std::size_t runtimeIndex)
    : pool_(pool), runtimeIndex_(runtimeIndex) {}

V8IsolatePool::Lease::~Lease() {
    release();
}

V8IsolatePool::Lease::Lease(Lease &&other) noexcept
    : pool_(other.pool_), runtimeIndex_(other.runtimeIndex_) {
    other.pool_ = nullptr;
    other.runtimeIndex_ = 0;
}

V8IsolatePool::Lease &V8IsolatePool::Lease::operator=(Lease &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    release();

    pool_ = other.pool_;
    runtimeIndex_ = other.runtimeIndex_;
    other.pool_ = nullptr;
    other.runtimeIndex_ = 0;

    return *this;
}

V8SsrRuntime *V8IsolatePool::Lease::operator->() const {
    return &runtime();
}

V8SsrRuntime &V8IsolatePool::Lease::runtime() const {
    return *pool_->runtimes_.at(runtimeIndex_);
}

void V8IsolatePool::Lease::markForRecycle() {
    recycle_ = true;
}

void V8IsolatePool::Lease::release() {
    if (pool_ == nullptr) {
        return;
    }

    if (recycle_) {
        pool_->recycle(runtimeIndex_);
    } else {
        pool_->release(runtimeIndex_);
    }
    pool_ = nullptr;
    runtimeIndex_ = 0;
    recycle_ = false;
}

V8IsolatePool::V8IsolatePool(std::size_t size,
                             std::string bundlePath,
                             std::uint64_t renderTimeoutMs,
                             FetchBridge fetchBridge)
    : bundlePath_(std::move(bundlePath)),
      fetchBridge_(std::move(fetchBridge)),
      renderTimeoutMs_(renderTimeoutMs) {
    const std::size_t poolSize = std::max<std::size_t>(1, size);
    runtimes_.reserve(poolSize);

    for (std::size_t i = 0; i < poolSize; ++i) {
        runtimes_.push_back(std::make_unique<V8SsrRuntime>(bundlePath_, fetchBridge_));
        availableRuntimes_.push(i);
    }
}

V8IsolatePool::Lease V8IsolatePool::acquire(std::uint64_t acquireTimeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (acquireTimeoutMs == 0) {
        cv_.wait(lock, [this] { return !availableRuntimes_.empty(); });
    } else {
        const auto ready = cv_.wait_for(lock,
                                        std::chrono::milliseconds(acquireTimeoutMs),
                                        [this] { return !availableRuntimes_.empty(); });
        if (!ready) {
            throw std::runtime_error("Timed out waiting for available V8 isolate");
        }
    }

    const std::size_t index = availableRuntimes_.front();
    availableRuntimes_.pop();
    return Lease(this, index);
}

std::uint64_t V8IsolatePool::renderTimeoutMs() const {
    return renderTimeoutMs_;
}

void V8IsolatePool::release(std::size_t runtimeIndex) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        availableRuntimes_.push(runtimeIndex);
    }
    cv_.notify_one();
}

void V8IsolatePool::recycle(std::size_t runtimeIndex) noexcept {
    std::unique_ptr<V8SsrRuntime> replacement;
    try {
        replacement = std::make_unique<V8SsrRuntime>(bundlePath_, fetchBridge_);
    } catch (...) {
        // Keep the existing runtime if recycle failed.
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (replacement) {
            runtimes_[runtimeIndex] = std::move(replacement);
        }
        availableRuntimes_.push(runtimeIndex);
    }
    cv_.notify_one();
}

}  // namespace hydra
