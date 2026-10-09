#pragma once

// The API's event thread (P19): events posted by the operations' threads are
// delivered to the user interface's callback on one thread, one at a time,
// in the order they were posted, with no lock of the API held.
//
// The queue never makes an operation wait, and its memory is bounded:
//  * a Progress event replaces the one of the same operation still queued
//    (and goes last: the latest state, after every earlier event);
//  * CandidatesFound events of a session that follow each other merge;
//  * FilesRecovered events are dropped once `maxQueuedFiles` files wait; the
//    next FilesRecovered or OperationFinished event of that session says how
//    many (Event::filesDropped).

#include "api/api_types.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace recovery::api::detail {

class EventDispatcher {
public:
    static constexpr std::uint64_t kDefaultMaxQueuedFiles = 100'000;

    // No callback: posted events are discarded, and no thread runs.
    explicit EventDispatcher(EventCallback callback, std::uint64_t maxQueuedFiles = kDefaultMaxQueuedFiles);
    ~EventDispatcher();
    EventDispatcher(const EventDispatcher&) = delete;
    EventDispatcher& operator=(const EventDispatcher&) = delete;
    EventDispatcher(EventDispatcher&&) = delete;
    EventDispatcher& operator=(EventDispatcher&&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return static_cast<bool>(callback_); }

    // Queues `event` (from any thread; never blocks for long).
    void post(Event event);

    // Discards what is queued and stops the thread, after the event being
    // delivered (if any). Must not be called from the callback.
    void stop();

private:
    void run();
    // The queue's key of an event's operation: its session, or its imaging.
    [[nodiscard]] static std::string keyOf(const Event& event);

    EventCallback callback_;
    const std::uint64_t maxQueuedFiles_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Event> queue_;
    // Files in the queued FilesRecovered events.
    std::uint64_t queuedFiles_ = 0;
    // Files dropped, by key, not yet told.
    std::map<std::string, std::uint64_t> dropped_;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace recovery::api::detail
