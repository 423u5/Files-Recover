#pragma once

// The user's request to stop a command (Ctrl+C), as the commands see it.
//
// The first request stops the running command cleanly: the action the command
// registered is called (it cancels the scan, the recovery job or the imaging
// under way), and the command ends at its next consistent point, keeping what
// it did. Requests are sticky: a command checks requested() before each step,
// and an action registered after a request is called at once, so a request
// that comes between two steps is not lost. A second request is not handled
// here: the console handler then lets Windows end the process at once, which
// the session journal and the image metadata are made to survive.
//
// Thread safety: every member may be called from any thread. The action runs
// on the thread that calls request() (Windows runs console handlers on a
// thread of their own), under the object's lock, so Scope's destructor waits
// for an action under way: the action never outlives what it refers to. An
// action may run more than once; it must be idempotent and must not block.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

namespace recovery::cli {

class Interrupt {
public:
    Interrupt() = default;
    Interrupt(const Interrupt&) = delete;
    Interrupt& operator=(const Interrupt&) = delete;
    Interrupt(Interrupt&&) = delete;
    Interrupt& operator=(Interrupt&&) = delete;

    // Asks the running command to stop. True for the first request (it is
    // handled: the command stops cleanly), false for the ones after it (the
    // caller may end the process).
    bool request() noexcept;
    [[nodiscard]] bool requested() const noexcept;
    [[nodiscard]] std::uint32_t requests() const noexcept;

    // The command is done (the process is about to end).
    void finish() noexcept;
    // Waits until finish() was called, at most `timeout`. True when it was.
    [[nodiscard]] bool waitFinished(std::chrono::milliseconds timeout) noexcept;

    // Registers `action` for its lifetime (the previous one is restored after
    // it). It is called at once when a request came before.
    class Scope {
    public:
        Scope(Interrupt* interrupt, std::function<void()> action);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;

    private:
        Interrupt* interrupt_;
        std::function<void()> previous_;
    };

private:
    void callAction() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable finishedChanged_;
    std::function<void()> action_;
    std::uint32_t requests_ = 0;
    bool finished_ = false;
};

}  // namespace recovery::cli
