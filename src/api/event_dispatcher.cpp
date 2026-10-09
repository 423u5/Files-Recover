#include "event_dispatcher.hpp"

#include <algorithm>
#include <utility>

namespace recovery::api::detail {

EventDispatcher::EventDispatcher(EventCallback callback, std::uint64_t maxQueuedFiles)
    : callback_(std::move(callback)), maxQueuedFiles_(maxQueuedFiles) {
    if (callback_) {
        thread_ = std::thread([this] { run(); });
    }
}

EventDispatcher::~EventDispatcher() {
    stop();
}

std::string EventDispatcher::keyOf(const Event& event) {
    if (event.imaging.has_value()) {
        return "imaging " + std::to_string(event.imaging->value);
    }
    return "session " + event.session;
}

void EventDispatcher::post(Event event) {
    if (!callback_) {
        return;
    }
    const std::string key = keyOf(event);
    {
        const std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        switch (event.kind) {
        case EventKind::Progress: {
            const auto same = std::find_if(queue_.begin(), queue_.end(), [&](const Event& queued) {
                return queued.kind == EventKind::Progress && keyOf(queued) == key;
            });
            if (same != queue_.end()) {
                queue_.erase(same);
            }
            break;
        }
        case EventKind::CandidatesFound:
            if (!queue_.empty()) {
                Event& last = queue_.back();
                if (last.kind == EventKind::CandidatesFound && last.session == event.session &&
                    last.firstCandidate.value + last.candidateCount == event.firstCandidate.value) {
                    last.candidateCount += event.candidateCount;
                    last.progress = std::move(event.progress);
                    wake_.notify_one();
                    return;
                }
            }
            break;
        case EventKind::FilesRecovered:
            if (queuedFiles_ + event.files.size() > maxQueuedFiles_) {
                dropped_[key] += event.files.size();
                return;
            }
            queuedFiles_ += event.files.size();
            break;
        default:
            break;
        }
        if (event.kind == EventKind::FilesRecovered || event.kind == EventKind::OperationFinished) {
            if (const auto found = dropped_.find(key); found != dropped_.end()) {
                event.filesDropped = found->second;
                dropped_.erase(found);
            }
        }
        queue_.push_back(std::move(event));
    }
    wake_.notify_one();
}

void EventDispatcher::stop() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        queue_.clear();
        queuedFiles_ = 0;
        dropped_.clear();
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void EventDispatcher::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) {
            return;
        }
        Event event = std::move(queue_.front());
        queue_.pop_front();
        if (event.kind == EventKind::FilesRecovered) {
            queuedFiles_ -= std::min<std::uint64_t>(queuedFiles_, event.files.size());
        }
        lock.unlock();
        try {
            callback_(event);
        } catch (...) {
            // The callback must not throw; an exception is not the API's to handle.
        }
        lock.lock();
    }
}

}  // namespace recovery::api::detail
