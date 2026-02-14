#include "hydra/V8IsolatePool.h"

#include <algorithm>
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

void V8IsolatePool::Lease::release() {
    if (pool_ == nullptr) {
        return;
    }

    pool_->release(runtimeIndex_);
    pool_ = nullptr;
    runtimeIndex_ = 0;
}

V8IsolatePool::V8IsolatePool(std::size_t size,
                             std::string bundlePath,
                             std::uint64_t renderTimeoutMs)
    : renderTimeoutMs_(renderTimeoutMs) {
    const std::size_t poolSize = std::max<std::size_t>(1, size);
    runtimes_.reserve(poolSize);

    for (std::size_t i = 0; i < poolSize; ++i) {
        runtimes_.push_back(std::make_unique<V8SsrRuntime>(bundlePath));
        availableRuntimes_.push(i);
    }
}

V8IsolatePool::Lease V8IsolatePool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !availableRuntimes_.empty(); });

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

}  // namespace hydra
