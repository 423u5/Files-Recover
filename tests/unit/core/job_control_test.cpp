// Job control (P15): a pause holds the threads that reach a safe point until
// it is resumed, cancellation ends a pause and cancels the token, and copies
// share their state.

#include "recovery/job_control.hpp"

#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace recovery {
namespace {

using namespace std::chrono_literals;

template <class Condition>
bool eventually(Condition condition) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

TEST(JobControlTest, WithoutAPauseTheJobGoesOn) {
    JobControl control;
    EXPECT_FALSE(control.isPauseRequested());
    EXPECT_FALSE(control.isCancellationRequested());
    RECOVERY_EXPECT_OK(control.waitWhilePaused());
    EXPECT_EQ(control.waitingThreads(), 0U);
    EXPECT_TRUE(control.token().canBeCancelled());
    EXPECT_FALSE(control.token().isCancellationRequested());
}

TEST(JobControlTest, APauseHoldsThreadsUntilResumed) {
    JobControl control;
    control.pause();
    EXPECT_TRUE(control.isPauseRequested());
    std::atomic<int> passed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([&] {
            EXPECT_TRUE(control.waitWhilePaused().ok());
            passed.fetch_add(1);
        });
    }
    ASSERT_TRUE(eventually([&] { return control.waitingThreads() == 3; }));
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(passed.load(), 0);
    control.resume();
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(passed.load(), 3);
    EXPECT_EQ(control.waitingThreads(), 0U);
    EXPECT_FALSE(control.isPauseRequested());
}

TEST(JobControlTest, CancellationEndsAPause) {
    JobControl control;
    const CancellationToken token = control.token();
    control.pause();
    std::atomic<bool> cancelled{false};
    std::thread waiter([&] {
        const Status status = control.waitWhilePaused();
        cancelled = !status.ok() && status.error().code == ErrorCode::Cancelled;
    });
    ASSERT_TRUE(eventually([&] { return control.waitingThreads() == 1; }));
    control.requestCancellation();
    waiter.join();
    EXPECT_TRUE(cancelled.load());
    EXPECT_TRUE(token.isCancellationRequested());
    EXPECT_TRUE(control.isCancellationRequested());
    // Once cancelled, a safe point never waits.
    RECOVERY_EXPECT_ERROR(control.waitWhilePaused(), ErrorCode::Cancelled);
}

TEST(JobControlTest, CopiesShareTheirState) {
    JobControl caller;
    const JobControl job = caller;  // NOLINT(performance-unnecessary-copy-initialization)
    caller.pause();
    EXPECT_TRUE(job.isPauseRequested());
    caller.resume();
    EXPECT_FALSE(job.isPauseRequested());
    caller.requestCancellation();
    EXPECT_TRUE(job.isCancellationRequested());
    EXPECT_TRUE(job.token().isCancellationRequested());
}

}  // namespace
}  // namespace recovery
