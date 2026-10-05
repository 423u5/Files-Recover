#include "recovery/job_control.hpp"

namespace recovery {

JobControl::JobControl() : state_(std::make_shared<State>()) {}

void JobControl::requestCancellation() noexcept {
    cancellation_.requestCancellation();
    state_->flags.fetch_or(kCancelled, std::memory_order_acq_rel);
    state_->flags.notify_all();
}

bool JobControl::isCancellationRequested() const noexcept {
    return cancellation_.isCancellationRequested();
}

void JobControl::pause() noexcept {
    state_->flags.fetch_or(kPaused, std::memory_order_acq_rel);
    state_->flags.notify_all();
}

void JobControl::resume() noexcept {
    state_->flags.fetch_and(~kPaused, std::memory_order_acq_rel);
    state_->flags.notify_all();
}

bool JobControl::isPauseRequested() const noexcept {
    return (state_->flags.load(std::memory_order_acquire) & kPaused) != 0;
}

Status JobControl::waitWhilePaused() const {
    State& state = *state_;
    state.waiting.fetch_add(1, std::memory_order_acq_rel);
    Status outcome = success();
    for (;;) {
        const std::uint32_t flags = state.flags.load(std::memory_order_acquire);
        if ((flags & kCancelled) != 0) {
            outcome = makeError(ErrorCode::Cancelled, "the job was cancelled");
            break;
        }
        if ((flags & kPaused) == 0) {
            break;
        }
        state.flags.wait(flags, std::memory_order_acquire);
    }
    state.waiting.fetch_sub(1, std::memory_order_acq_rel);
    return outcome;
}

std::uint32_t JobControl::waitingThreads() const noexcept {
    return state_->waiting.load(std::memory_order_acquire);
}

}  // namespace recovery
