#pragma once

// Work items done in parallel, their results taken in order (P15).

#include "recovery/result.hpp"
#include "recovery/worker_pool.hpp"

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace recovery {

// Produces items [first, last) on `pool`, at most `window` at a time, and
// hands each result to `consume` in item order on the calling thread: the
// results are taken exactly as a loop over the items on one thread takes
// them, whatever order the workers finish in.
//
//   produce(std::size_t item) -> Result<T>     on a worker, concurrently
//   consume(std::size_t item, T&&) -> Status   on the calling thread, in order
//
// The first failure in item order ends the run and is returned: of
// produce() (items after it are not consumed), or of consume(). Items not
// started then are never produced; items in flight are waited for, and
// their results dropped. An exception escaping produce() is a failure
// (InternalError). With a null pool (or a window of 1 item) every item is
// produced and consumed on the calling thread, one after the other.
//
// Memory: at most `window` results exist at once. The calling thread must
// not be a worker of `pool`.
template <class T, class Produce, class Consume>
[[nodiscard]] Status runOrdered(WorkerPool* pool, std::size_t window, std::size_t first, std::size_t last,
                                Produce&& produce, Consume&& consume) {
    if (pool == nullptr || window <= 1) {
        for (std::size_t item = first; item < last; ++item) {
            Result<T> result = produce(item);
            if (!result.ok()) {
                return result.error();
            }
            if (Status consumed = consume(item, std::move(result).value()); !consumed.ok()) {
                return consumed;
            }
        }
        return success();
    }

    struct Shared {
        std::mutex mutex;
        std::condition_variable changed;
        // Results by item % window.
        std::vector<std::optional<Result<T>>> ring;
        std::size_t inFlight = 0;
    };
    Shared shared;
    shared.ring.resize(window);
    std::size_t next = first;     // next item to start
    std::size_t taken = first;    // next item to consume
    Status outcome = success();

    const auto start = [&](std::size_t item) -> Status {
        {
            const std::lock_guard lock(shared.mutex);
            ++shared.inFlight;
        }
        Status submitted = pool->submit([&shared, &produce, item, window] {
            std::optional<Result<T>> result;
            try {
                result.emplace(produce(item));
            } catch (const std::exception& error) {
                result.emplace(makeError(ErrorCode::InternalError, std::string("a work item failed: ") + error.what()));
            } catch (...) {
                result.emplace(makeError(ErrorCode::InternalError, "a work item failed"));
            }
            {
                const std::lock_guard lock(shared.mutex);
                shared.ring[item % window] = std::move(result);
                --shared.inFlight;
            }
            shared.changed.notify_all();
        });
        if (!submitted.ok()) {
            const std::lock_guard lock(shared.mutex);
            --shared.inFlight;
        }
        return submitted;
    };

    while (taken < last) {
        while (next < last && next - taken < window) {
            if (Status started = start(next); !started.ok()) {
                outcome = started;
                break;
            }
            ++next;
        }
        if (!outcome.ok()) {
            break;
        }
        std::optional<Result<T>> result;
        {
            std::unique_lock lock(shared.mutex);
            shared.changed.wait(lock, [&] { return shared.ring[taken % window].has_value(); });
            result = std::move(shared.ring[taken % window]);
            shared.ring[taken % window].reset();
        }
        if (!result->ok()) {
            outcome = result->error();
            break;
        }
        if (Status consumed = consume(taken, std::move(*result).value()); !consumed.ok()) {
            outcome = consumed;
            break;
        }
        ++taken;
    }
    // Items in flight use `shared` and `produce`: wait for them.
    std::unique_lock lock(shared.mutex);
    shared.changed.wait(lock, [&] { return shared.inFlight == 0; });
    return outcome;
}

}  // namespace recovery
