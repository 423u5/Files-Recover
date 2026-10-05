#pragma once

// Control of a long-running job (P15): cancel it, pause it, resume it.

#include "recovery/cancellation.hpp"
#include "recovery/result.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace recovery {

// Cancels, pauses and resumes a job (a scan, a recovery) from any thread.
//
// The job polls token() for cancellation, as every stage of the engine does,
// and calls waitWhilePaused() at its safe points: places where it may stop
// for any length of time without holding anything another thread needs. A
// pause therefore takes effect at the job's next safe point, not at once.
// Cancellation also ends a pause: the waiting threads return Cancelled.
//
// Copies share their state (like CancellationSource), so the caller keeps a
// copy and gives one to the job.
//
// Thread safety: every member may be called from any thread.
class JobControl {
public:
    JobControl();

    void requestCancellation() noexcept;
    [[nodiscard]] bool isCancellationRequested() const noexcept;
    // Cancelled when requestCancellation() is called.
    [[nodiscard]] CancellationToken token() const noexcept { return cancellation_.token(); }

    // Asks the job to wait at its next safe point until resume() or
    // requestCancellation().
    void pause() noexcept;
    void resume() noexcept;
    [[nodiscard]] bool isPauseRequested() const noexcept;

    // Blocks while a pause is requested. Returns Cancelled when cancellation
    // is requested before or during the wait, success otherwise.
    [[nodiscard]] Status waitWhilePaused() const;
    // Threads blocked in waitWhilePaused() now.
    [[nodiscard]] std::uint32_t waitingThreads() const noexcept;

private:
    struct State {
        // kPaused and kCancelled bits; waited on with std::atomic::wait.
        std::atomic<std::uint32_t> flags{0};
        std::atomic<std::uint32_t> waiting{0};
    };
    static constexpr std::uint32_t kPaused = 1;
    static constexpr std::uint32_t kCancelled = 2;

    std::shared_ptr<State> state_;
    CancellationSource cancellation_;
};

}  // namespace recovery
