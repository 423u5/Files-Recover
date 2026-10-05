#pragma once

// A bounded pool of worker threads (P15).

#include "recovery/result.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace recovery {

// A fixed number of worker threads that run tasks from a bounded queue.
//
// The engine never creates a thread per file, sector or hit: work is queued
// here, and the queue is bounded, so a producer that is faster than the
// workers waits in submit() instead of growing memory.
//
// Rules for tasks: a task must not throw (an exception that escapes one is
// caught and counted in failedTasks(), never propagated), and must not wait
// for a task queued after it, nor call submit() on its own pool: with every
// worker waiting, nothing would run the task waited for.
//
// Thread safety: every member may be called from any thread. The destructor
// runs the tasks still queued, then joins the workers.
class WorkerPool {
public:
    using Task = std::function<void()>;

    static constexpr std::size_t kDefaultQueueCapacity = 256;
    static constexpr std::size_t kMaxQueueCapacity = 1 << 20;

    // `threads` workers (1 to EngineConfig::kMaxWorkerThreads) and room for
    // `queueCapacity` waiting tasks (1 to kMaxQueueCapacity). Fails with
    // InvalidInput for values outside those ranges, and with InternalError
    // when the system cannot start a thread.
    [[nodiscard]] static Result<std::unique_ptr<WorkerPool>> create(
        std::uint32_t threads, std::size_t queueCapacity = kDefaultQueueCapacity);

    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;

    // Queues `task`, waiting while the queue is full. Fails with InvalidInput
    // for an empty task, and once shutdown() has been called.
    [[nodiscard]] Status submit(Task task);
    // Queues `task` if the queue has room; false when it is full, the task is
    // empty or the pool is shut down.
    [[nodiscard]] bool trySubmit(Task task);

    // Stops accepting tasks, runs the ones queued and joins the workers.
    // Called by the destructor; calling it again does nothing.
    void shutdown();

    [[nodiscard]] std::uint32_t threadCount() const noexcept { return threadCount_; }
    [[nodiscard]] std::size_t queueCapacity() const noexcept { return capacity_; }
    // Tasks queued or running now.
    [[nodiscard]] std::size_t pending() const;
    // Tasks that threw.
    [[nodiscard]] std::uint64_t failedTasks() const noexcept { return failed_.load(std::memory_order_relaxed); }
    // Whether the calling thread is one of this pool's workers.
    [[nodiscard]] bool isWorkerThread() const noexcept;

private:
    WorkerPool(std::uint32_t threads, std::size_t queueCapacity);
    void work();

    const std::uint32_t threadCount_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<Task> queue_;
    std::size_t running_ = 0;
    bool stopping_ = false;
    std::atomic<std::uint64_t> failed_{0};
    // Held while the workers are joined.
    std::mutex joinMutex_;
    std::vector<std::thread> threads_;
    // The workers' ids, fixed once create() returns.
    std::vector<std::thread::id> ids_;
};

}  // namespace recovery
