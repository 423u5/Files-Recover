#include "recovery/worker_pool.hpp"

#include "recovery/config.hpp"

#include <string>
#include <system_error>
#include <utility>

namespace recovery {

Result<std::unique_ptr<WorkerPool>> WorkerPool::create(std::uint32_t threads, std::size_t queueCapacity) {
    if (threads == 0 || threads > EngineConfig::kMaxWorkerThreads) {
        return makeError(ErrorCode::InvalidInput, "a worker pool needs 1 to " +
                                                      std::to_string(EngineConfig::kMaxWorkerThreads) + " threads");
    }
    if (queueCapacity == 0 || queueCapacity > kMaxQueueCapacity) {
        return makeError(ErrorCode::InvalidInput,
                         "a worker pool's queue holds 1 to " + std::to_string(kMaxQueueCapacity) + " tasks");
    }
    std::unique_ptr<WorkerPool> pool(new WorkerPool(threads, queueCapacity));
    try {
        for (std::uint32_t i = 0; i < threads; ++i) {
            pool->threads_.emplace_back([raw = pool.get()] { raw->work(); });
        }
    } catch (const std::system_error& error) {
        pool->shutdown();
        return makeError(ErrorCode::InternalError, std::string("cannot start a worker thread: ") + error.what());
    }
    // Fixed from here on, so isWorkerThread() reads it without a lock.
    for (const std::thread& thread : pool->threads_) {
        pool->ids_.push_back(thread.get_id());
    }
    return pool;
}

WorkerPool::WorkerPool(std::uint32_t threads, std::size_t queueCapacity)
    : threadCount_(threads), capacity_(queueCapacity) {}

WorkerPool::~WorkerPool() {
    shutdown();
}

Status WorkerPool::submit(Task task) {
    if (!task) {
        return makeError(ErrorCode::InvalidInput, "an empty task cannot be queued");
    }
    std::unique_lock lock(mutex_);
    notFull_.wait(lock, [this] { return stopping_ || queue_.size() < capacity_; });
    if (stopping_) {
        return makeError(ErrorCode::InvalidInput, "the worker pool is shut down");
    }
    queue_.push_back(std::move(task));
    lock.unlock();
    notEmpty_.notify_one();
    return success();
}

bool WorkerPool::trySubmit(Task task) {
    if (!task) {
        return false;
    }
    {
        const std::lock_guard lock(mutex_);
        if (stopping_ || queue_.size() >= capacity_) {
            return false;
        }
        queue_.push_back(std::move(task));
    }
    notEmpty_.notify_one();
    return true;
}

void WorkerPool::shutdown() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    notEmpty_.notify_all();
    notFull_.notify_all();
    // A second caller waits here until the first has joined every worker.
    const std::lock_guard joining(joinMutex_);
    for (std::thread& thread : threads_) {
        if (!thread.joinable()) {
            continue;
        }
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();  // a task shutting its own pool down: it ends when the task returns
        } else {
            thread.join();
        }
    }
    threads_.clear();
}

std::size_t WorkerPool::pending() const {
    const std::lock_guard lock(mutex_);
    return queue_.size() + running_;
}

bool WorkerPool::isWorkerThread() const noexcept {
    const std::thread::id self = std::this_thread::get_id();
    for (const std::thread::id id : ids_) {
        if (id == self) {
            return true;
        }
    }
    return false;
}

void WorkerPool::work() {
    std::unique_lock lock(mutex_);
    for (;;) {
        notEmpty_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) {
            return;  // stopping, and nothing left to run
        }
        Task task = std::move(queue_.front());
        queue_.pop_front();
        ++running_;
        lock.unlock();
        notFull_.notify_one();
        try {
            task();
        } catch (...) {
            failed_.fetch_add(1, std::memory_order_relaxed);
        }
        task = nullptr;  // release what the task holds before taking the lock
        lock.lock();
        --running_;
    }
}

}  // namespace recovery
