// The bounded worker pool (P15): every task runs, never more at once than
// the pool has threads, a full queue makes producers wait, a task that throws
// is counted without stopping the pool, and shutdown runs what is queued.

#include "recovery/worker_pool.hpp"

#include "recovery/config.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace recovery {
namespace {

using namespace std::chrono_literals;

std::unique_ptr<WorkerPool> makePool(std::uint32_t threads, std::size_t capacity = WorkerPool::kDefaultQueueCapacity) {
    Result<std::unique_ptr<WorkerPool>> pool = WorkerPool::create(threads, capacity);
    EXPECT_TRUE(pool.ok());
    return pool.ok() ? std::move(pool).value() : nullptr;
}

// A gate that tasks wait at until it is opened.
class Gate {
public:
    void open() {
        {
            const std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }
    void wait() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

// Waits (up to 10 s) until `condition` holds.
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

TEST(WorkerPoolTest, InvalidSizesAreRefused) {
    RECOVERY_EXPECT_ERROR(WorkerPool::create(0), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(WorkerPool::create(EngineConfig::kMaxWorkerThreads + 1), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(WorkerPool::create(2, 0), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(WorkerPool::create(2, WorkerPool::kMaxQueueCapacity + 1), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_OK(WorkerPool::create(EngineConfig::kMaxWorkerThreads, WorkerPool::kMaxQueueCapacity));
}

TEST(WorkerPoolTest, EveryTaskRunsOnAWorker) {
    for (const std::uint32_t threads : {1U, 4U}) {
        std::unique_ptr<WorkerPool> pool = makePool(threads, 8);
        ASSERT_NE(pool, nullptr);
        EXPECT_EQ(pool->threadCount(), threads);
        EXPECT_FALSE(pool->isWorkerThread());
        std::atomic<int> ran{0};
        std::atomic<int> onWorker{0};
        for (int i = 0; i < 1000; ++i) {
            RECOVERY_ASSERT_OK(pool->submit([&] {
                ran.fetch_add(1);
                if (pool->isWorkerThread()) {
                    onWorker.fetch_add(1);
                }
            }));
        }
        pool->shutdown();
        EXPECT_EQ(ran.load(), 1000);
        EXPECT_EQ(onWorker.load(), 1000);
        EXPECT_EQ(pool->pending(), 0U);
    }
}

TEST(WorkerPoolTest, NoMoreTasksRunAtOnceThanThePoolHasThreads) {
    std::unique_ptr<WorkerPool> pool = makePool(3);
    ASSERT_NE(pool, nullptr);
    std::atomic<int> running{0};
    std::atomic<int> highest{0};
    for (int i = 0; i < 60; ++i) {
        RECOVERY_ASSERT_OK(pool->submit([&] {
            const int now = running.fetch_add(1) + 1;
            int seen = highest.load();
            while (now > seen && !highest.compare_exchange_weak(seen, now)) {
            }
            std::this_thread::sleep_for(100us);
            running.fetch_sub(1);
        }));
    }
    pool->shutdown();
    EXPECT_LE(highest.load(), 3);
    EXPECT_GE(highest.load(), 1);
}

TEST(WorkerPoolTest, AFullQueueMakesTheProducerWait) {
    std::unique_ptr<WorkerPool> pool = makePool(1, 2);
    ASSERT_NE(pool, nullptr);
    Gate gate;
    std::atomic<bool> started{false};
    RECOVERY_ASSERT_OK(pool->submit([&] {
        started = true;
        gate.wait();
    }));
    ASSERT_TRUE(eventually([&] { return started.load(); }));
    // The worker is busy: two tasks fill the queue, a third does not fit.
    EXPECT_TRUE(pool->trySubmit([] {}));
    EXPECT_TRUE(pool->trySubmit([] {}));
    EXPECT_FALSE(pool->trySubmit([] {}));
    EXPECT_EQ(pool->pending(), 3U);

    std::atomic<bool> submitted{false};
    std::thread producer([&] {
        EXPECT_TRUE(pool->submit([] {}).ok());
        submitted = true;
    });
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(submitted.load());  // still waiting for room
    gate.open();
    producer.join();
    EXPECT_TRUE(submitted.load());
    pool->shutdown();
    EXPECT_EQ(pool->pending(), 0U);
}

TEST(WorkerPoolTest, ATaskThatThrowsIsCountedAndThePoolGoesOn) {
    std::unique_ptr<WorkerPool> pool = makePool(2);
    ASSERT_NE(pool, nullptr);
    std::atomic<int> ran{0};
    RECOVERY_ASSERT_OK(pool->submit([] { throw std::runtime_error("task failure"); }));
    for (int i = 0; i < 10; ++i) {
        RECOVERY_ASSERT_OK(pool->submit([&] { ran.fetch_add(1); }));
    }
    pool->shutdown();
    EXPECT_EQ(pool->failedTasks(), 1U);
    EXPECT_EQ(ran.load(), 10);
}

TEST(WorkerPoolTest, ShutdownRunsWhatIsQueuedAndRefusesMore) {
    std::unique_ptr<WorkerPool> pool = makePool(1, 64);
    ASSERT_NE(pool, nullptr);
    Gate gate;
    std::atomic<int> ran{0};
    RECOVERY_ASSERT_OK(pool->submit([&] { gate.wait(); }));
    for (int i = 0; i < 50; ++i) {
        RECOVERY_ASSERT_OK(pool->submit([&] { ran.fetch_add(1); }));
    }
    std::thread opener([&] {
        std::this_thread::sleep_for(20ms);
        gate.open();
    });
    pool->shutdown();
    opener.join();
    EXPECT_EQ(ran.load(), 50);
    RECOVERY_EXPECT_ERROR(pool->submit([] {}), ErrorCode::InvalidInput);
    EXPECT_FALSE(pool->trySubmit([] {}));
    RECOVERY_EXPECT_ERROR(makePool(1)->submit(WorkerPool::Task{}), ErrorCode::InvalidInput);
    pool->shutdown();  // again: nothing to do
}

TEST(WorkerPoolTest, ShutdownFromSeveralThreadsJoinsOnce) {
    std::unique_ptr<WorkerPool> pool = makePool(4);
    ASSERT_NE(pool, nullptr);
    std::atomic<int> ran{0};
    for (int i = 0; i < 100; ++i) {
        RECOVERY_ASSERT_OK(pool->submit([&] { ran.fetch_add(1); }));
    }
    std::thread a([&] { pool->shutdown(); });
    std::thread b([&] { pool->shutdown(); });
    a.join();
    b.join();
    EXPECT_EQ(ran.load(), 100);
}

}  // namespace
}  // namespace recovery
