// Work done in parallel, taken in order (P15): results are consumed in item
// order whatever order the workers finish in, never more than the window is
// in flight, and the first failure in item order ends the run after the
// items in flight are waited for.

#include "recovery/ordered_work.hpp"

#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
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
