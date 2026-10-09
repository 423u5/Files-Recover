#include "cli/interrupt.hpp"

#include <utility>

namespace recovery::cli {

bool Interrupt::request() noexcept {
    try {
        const std::lock_guard lock(mutex_);
        ++requests_;
        if (requests_ > 1) {
            return false;
        }
        callAction();
        return true;
    } catch (...) {
        return false;
    }
}

bool Interrupt::requested() const noexcept {
    return requests() > 0;
}

std::uint32_t Interrupt::requests() const noexcept {
    try {
        const std::lock_guard lock(mutex_);
        return requests_;
    } catch (...) {
        return 0;
    }
}

void Interrupt::finish() noexcept {
    try {
        {
            const std::lock_guard lock(mutex_);
            finished_ = true;
        }
        finishedChanged_.notify_all();
    } catch (...) {
        // Nothing waits for a process that cannot lock a mutex.
    }
}

bool Interrupt::waitFinished(std::chrono::milliseconds timeout) noexcept {
    try {
        std::unique_lock lock(mutex_);
        return finishedChanged_.wait_for(lock, timeout, [this] { return finished_; });
    } catch (...) {
        return false;
    }
}

// Called with mutex_ held.
void Interrupt::callAction() noexcept {
    if (!action_) {
        return;
    }
    try {
        action_();
    } catch (...) {
        // The action only asks a job to stop; there is nothing to undo.
    }
}

Interrupt::Scope::Scope(Interrupt* interrupt, std::function<void()> action) : interrupt_(interrupt) {
    if (interrupt_ == nullptr) {
        return;
    }
    const std::lock_guard lock(interrupt_->mutex_);
    previous_ = std::exchange(interrupt_->action_, std::move(action));
    if (interrupt_->requests_ > 0) {
        interrupt_->callAction();
    }
}

Interrupt::Scope::~Scope() {
    if (interrupt_ == nullptr) {
        return;
    }
    try {
        const std::lock_guard lock(interrupt_->mutex_);
        interrupt_->action_ = std::move(previous_);
    } catch (...) {
        // A destructor cannot report it; the action stays registered.
    }
}

}  // namespace recovery::cli
