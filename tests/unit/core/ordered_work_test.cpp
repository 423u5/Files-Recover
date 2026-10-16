// Work done in parallel, taken in order (P15): results are consumed in item
// order whatever order the workers finish in, never more than the window is
// in flight, and the first failure in item order ends the run after the
// items in flight are waited for.

#include "recovery/ordered_work.hpp"

#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <intrin.h>
#include <stdexcept>
#include <thread>
#include <vector>

namespace recovery {
namespace {

using namespace std::chrono_literals;

std::unique_ptr<WorkerPool> makePool(std::uint32_t threads) {
    Result<std::unique_ptr<WorkerPool>> pool = WorkerPool::create(threads, 64);
    EXPECT_TRUE(pool.ok());
    return pool.ok() ? std::move(pool).value() : nullptr;
}

// Items finish out of order: later ones are quicker.
std::chrono::microseconds delayOf(std::size_t item) {
    return std::chrono::microseconds{(7 - item % 8) * 300};
}

TEST(OrderedWorkTest, ResultsAreTakenInItemOrder) {
    std::unique_ptr<WorkerPool> pool = makePool(4);
    ASSERT_NE(pool, nullptr);
    std::vector<std::size_t> taken;
    std::atomic<int> running{0};
    std::atomic<int> highest{0};
    const Status status = runOrdered<std::size_t>(
        pool.get(), 6, 3, 103,
        [&](std::size_t item) -> Result<std::size_t> {
            const int now = running.fetch_add(1) + 1;
            int seen = highest.load();
            while (now > seen && !highest.compare_exchange_weak(seen, now)) {
            }
            std::this_thread::sleep_for(delayOf(item));
            running.fetch_sub(1);
            return item * 10;
        },
        [&](std::size_t item, std::size_t&& value) -> Status {
            EXPECT_EQ(value, item * 10);
            taken.push_back(item);
            return success();
        });
    RECOVERY_ASSERT_OK(status);
    ASSERT_EQ(taken.size(), 100U);
    for (std::size_t i = 0; i < taken.size(); ++i) {
        EXPECT_EQ(taken[i], i + 3);
    }
    EXPECT_LE(highest.load(), 4);
}

TEST(OrderedWorkTest, NoMoreThanTheWindowIsInFlight) {
    std::unique_ptr<WorkerPool> pool = makePool(8);
    ASSERT_NE(pool, nullptr);
    std::atomic<int> unconsumed{0};
    std::atomic<int> highest{0};
    const Status status = runOrdered<int>(
        pool.get(), 3, 0, 60,
        [&](std::size_t item) -> Result<int> {
            const int now = unconsumed.fetch_add(1) + 1;
            int seen = highest.load();
            while (now > seen && !highest.compare_exchange_weak(seen, now)) {
            }
            std::this_thread::sleep_for(delayOf(item));
            return static_cast<int>(item);
        },
        [&](std::size_t, int&&) -> Status {
            unconsumed.fetch_sub(1);
            return success();
        });
    RECOVERY_ASSERT_OK(status);
    EXPECT_LE(highest.load(), 3);
}

TEST(OrderedWorkTest, TheFirstFailureInItemOrderEndsTheRun) {
    std::unique_ptr<WorkerPool> pool = makePool(4);
    ASSERT_NE(pool, nullptr);
    std::vector<std::size_t> taken;
    std::atomic<std::size_t> produced{0};
    const Status status = runOrdered<std::size_t>(
        pool.get(), 8, 0, 1000,
        [&](std::size_t item) -> Result<std::size_t> {
            produced.fetch_add(1);
            std::this_thread::sleep_for(delayOf(item));
            if (item == 17 || item == 19) {
                return makeError(ErrorCode::IoError, "item " + std::to_string(item));
            }
            return item;
        },
        [&](std::size_t item, std::size_t&&) -> Status {
            taken.push_back(item);
            return success();
        });
    RECOVERY_EXPECT_ERROR(status, ErrorCode::IoError);
    EXPECT_EQ(status.error().message, "item 17");
    ASSERT_EQ(taken.size(), 17U);
    // Items after the window were never started.
    EXPECT_LE(produced.load(), 17U + 8U);
}

TEST(OrderedWorkTest, AConsumerFailureEndsTheRun) {
    std::unique_ptr<WorkerPool> pool = makePool(2);
    ASSERT_NE(pool, nullptr);
    std::size_t consumed = 0;
    const Status status = runOrdered<int>(
        pool.get(), 4, 0, 50, [&](std::size_t item) -> Result<int> { return static_cast<int>(item); },
        [&](std::size_t item, int&&) -> Status {
            ++consumed;
            return item == 5 ? makeError(ErrorCode::DestinationError, "full") : success();
        });
    RECOVERY_EXPECT_ERROR(status, ErrorCode::DestinationError);
    EXPECT_EQ(consumed, 6U);
}

TEST(OrderedWorkTest, AnExceptionInAWorkItemIsAFailure) {
    std::unique_ptr<WorkerPool> pool = makePool(2);
    ASSERT_NE(pool, nullptr);
    const Status status = runOrdered<int>(
        pool.get(), 4, 0, 10,
        [&](std::size_t item) -> Result<int> {
            if (item == 3) {
                throw std::runtime_error("boom");
            }
            return static_cast<int>(item);
        },
        [&](std::size_t, int&&) -> Status { return success(); });
    RECOVERY_EXPECT_ERROR(status, ErrorCode::InternalError);
    EXPECT_EQ(pool->failedTasks(), 0U);  // caught inside the item, not by the pool
}

// True on the threads that produce items.
thread_local bool producing = false;

// A result whose move is slow on the workers only: a worker moves it in while
// it holds the run's lock, so the calling thread often comes to wait for the
// lock, and the worker's unlock wakes it.
struct SlowOnWorkers {
    int value = 0;
    SlowOnWorkers() = default;
    explicit SlowOnWorkers(int v) : value(v) {}
    SlowOnWorkers(SlowOnWorkers&& other) noexcept : value(other.value) {
        if (producing) {
            const auto until = std::chrono::steady_clock::now() + 20us;
            while (std::chrono::steady_clock::now() < until) {
            }
        }
    }
    SlowOnWorkers& operator=(SlowOnWorkers&& other) noexcept {
        value = other.value;
        return *this;
    }
    SlowOnWorkers(const SlowOnWorkers&) = delete;
    SlowOnWorkers& operator=(const SlowOnWorkers&) = delete;
    ~SlowOnWorkers() = default;
};

// Overwrites the stack below the caller, where a run that returned kept its state.
__declspec(noinline) void overwriteStack() {
    unsigned char junk[16 * 1024 - 256];
    __stosb(junk, 0xA5, sizeof junk);
    const volatile unsigned char kept = junk[sizeof junk - 1];
    (void)kept;
}

// After P20 (L245): the calling thread may return, and the run's state with
// it, the moment the last result is in; a worker must leave that state alone
// once it has handed its result over. One notified the run's condition
// variable after unlocking, which AddressSanitizer caught as an access
// violation in a scan (2026-10-16). Here, run after run, the last result is
// handed over while the calling thread waits for the lock, and the stack the
// run used is overwritten at once. It is a race: before the fix, about one
// AddressSanitizer run of this test in three failed.
TEST(OrderedWorkTest, AWorkerLeavesTheRunAloneOnceItsResultIsHandedOver) {
    std::unique_ptr<WorkerPool> pool = makePool(4);
    ASSERT_NE(pool, nullptr);
    const auto until = std::chrono::steady_clock::now() + 3s;
    std::size_t runs = 0;
    while (std::chrono::steady_clock::now() < until) {
        const Status status = runOrdered<SlowOnWorkers>(
            pool.get(), 2, 0, 2,
            [](std::size_t item) -> Result<SlowOnWorkers> {
                producing = true;
                return SlowOnWorkers(static_cast<int>(item));
            },
            [](std::size_t, SlowOnWorkers&&) -> Status { return success(); });
        RECOVERY_EXPECT_OK(status);
        overwriteStack();
        ++runs;
    }
    EXPECT_GT(runs, 0U);
}

TEST(OrderedWorkTest, WithoutAPoolItemsRunOneAfterTheOther) {
    std::vector<std::string> events;
    const Status status = runOrdered<int>(
        nullptr, 8, 0, 3,
        [&](std::size_t item) -> Result<int> {
            events.push_back("produce " + std::to_string(item));
            return static_cast<int>(item);
        },
        [&](std::size_t item, int&&) -> Status {
            events.push_back("consume " + std::to_string(item));
            return success();
        });
    RECOVERY_ASSERT_OK(status);
    const std::vector<std::string> expected = {"produce 0", "consume 0", "produce 1",
                                               "consume 1", "produce 2", "consume 2"};
    EXPECT_EQ(events, expected);
    RECOVERY_EXPECT_OK(runOrdered<int>(
        nullptr, 8, 5, 5, [](std::size_t) -> Result<int> { return 0; },
        [](std::size_t, int&&) -> Status { return success(); }));
}

}  // namespace
}  // namespace recovery
