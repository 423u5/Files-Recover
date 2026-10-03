#pragma once

#include <atomic>
#include <memory>

namespace recovery {

// Cooperative cancellation.
//
// Thread safety: a CancellationSource may be cancelled from any thread while
// any number of threads poll tokens obtained from it. Copies of a source or
// token share the same state.

class CancellationToken {
public:
    // A default-constructed token can never be cancelled.
    CancellationToken() noexcept = default;

    [[nodiscard]] bool isCancellationRequested() const noexcept {
        return state_ != nullptr && state_->load(std::memory_order_acquire);
    }

    [[nodiscard]] bool canBeCancelled() const noexcept { return state_ != nullptr; }

private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<const std::atomic<bool>> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<const std::atomic<bool>> state_;
};

class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<std::atomic<bool>>(false)) {}

    void requestCancellation() noexcept { state_->store(true, std::memory_order_release); }

    [[nodiscard]] bool isCancellationRequested() const noexcept {
        return state_->load(std::memory_order_acquire);
    }

    [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken(state_); }

private:
    std::shared_ptr<std::atomic<bool>> state_;
};

}  // namespace recovery
