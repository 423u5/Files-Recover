#pragma once

// Shared by the API tests (P19): a test's world (a temporary folder with the
// scan tests' card as an image file, a sessions folder, simulated physical
// disks, the API with its platform simulated), a recorder of the API's
// events, and what scans of the card must deliver.

#include "api/api_platform.hpp"
#include "api/recovery_api.hpp"
#include "recovery/config.hpp"
#include "support/card_fixtures.hpp"
#include "support/fake_device.hpp"
#include "support/test_files.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recovery::api {

// GoogleTest prints these by name.
void PrintTo(OperationState state, std::ostream* stream);
void PrintTo(OperationKind kind, std::ostream* stream);
void PrintTo(RunState state, std::ostream* stream);
void PrintTo(EventKind kind, std::ostream* stream);
void PrintTo(RecoveryState state, std::ostream* stream);

}  // namespace recovery::api

namespace recovery::api::test {

using Bytes = std::vector<std::byte>;
using namespace std::chrono_literals;

// How long a test waits for an operation on the card (a deep scan takes about
// a second in a debug build).
inline constexpr std::chrono::milliseconds kWait = 120s;

// Simulated physical disks, by number; any other number does not exist.
class DiskRack final : public storage::IDeviceOpener {
public:
    // Adds disk `number` holding `bytes`, its reads failing (ERROR_CRC) in
    // the byte ranges `failing`.
    std::shared_ptr<::recovery::test::FakeDeviceConfig> add(std::uint32_t number, Bytes bytes,
                                                            std::vector<std::pair<std::uint64_t, std::uint64_t>>
                                                                failing = {});
    // Opening disk `number` fails with `error` from now on.
    void failOpen(std::uint32_t number, Error error);

    Result<std::unique_ptr<storage::IDeviceIo>> openReadOnly(const std::filesystem::path& path,
                                                             storage::DeviceKind kind) override;

private:
    std::mutex mutex_;
    std::map<std::uint32_t, std::shared_ptr<::recovery::test::FakeDeviceConfig>> disks_;
    std::map<std::uint32_t, Error> failures_;
};

// Every event the API delivers, in order.
class EventLog {
public:
    // The callback to give the API.
    [[nodiscard]] EventCallback callback();
    // Runs `hook` on the event thread for every event, before it is recorded.
    void setHook(std::function<void(const Event&)> hook);

    [[nodiscard]] std::vector<Event> events() const;
    // The events of session `session` (or of imaging `imaging`).
    [[nodiscard]] std::vector<Event> eventsOf(std::string_view session) const;
    [[nodiscard]] std::vector<Event> eventsOf(ImagingId imaging) const;
    // Waits until `done` holds for the events recorded (or `timeout` passes).
    bool waitFor(const std::function<bool(const std::vector<Event>&)>& done, std::chrono::milliseconds timeout = kWait);
    // Waits for the `count`-th OperationFinished event of session `session`.
    std::optional<Event> waitForFinished(std::string_view session, std::size_t count = 1);
    void clear();

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Event> events_;
    std::function<void(const Event&)> hook_;
};

// A test's world. Simulated disks are numbered from 7; the disk resolver
// says every path is on disk 0 (the "system disk"), unless a path is below a
// folder put on another disk with placeOn().
class ApiWorld {
public:
    ApiWorld();
    ~ApiWorld();
    ApiWorld(const ApiWorld&) = delete;
    ApiWorld& operator=(const ApiWorld&) = delete;
    ApiWorld(ApiWorld&&) = delete;
    ApiWorld& operator=(ApiWorld&&) = delete;

    [[nodiscard]] const std::filesystem::path& folder() const noexcept { return folder_.path(); }
    [[nodiscard]] std::filesystem::path sessions() const { return folder() / "sessions"; }
    // The card written as an image file (once).
    [[nodiscard]] std::filesystem::path cardImage();
    [[nodiscard]] SourceRef cardSource() { return SourceRef::imageFile(cardImage()); }

    // A simulated physical disk (numbered from 7).
    std::uint32_t addDisk(Bytes bytes, std::vector<std::pair<std::uint64_t, std::uint64_t>> failing = {});
    [[nodiscard]] DiskRack& rack() noexcept { return *rack_; }
    // Paths below `folder` are on disk `disk` (for destination checks).
    // Create the folder: destination checks ask about the nearest folder of
    // a path that exists, as a real volume's would be.
    void placeOn(const std::filesystem::path& folder, std::uint32_t disk);

    // Edited before the API is made (the first api()); progressInterval is 0
    // (every report of the engine is an event) and onEvent the log's. What a
    // hook or callback captures must be declared before the world: the API's
    // threads may call it until the world is gone.
    [[nodiscard]] ApiOptions& options() noexcept { return options_; }
    [[nodiscard]] PlatformHooks& hooks() noexcept { return hooks_; }
    [[nodiscard]] EventLog& log() noexcept { return log_; }

    [[nodiscard]] RecoveryApi& api();
    // Destroys the API (its operations are cancelled, its sessions closed);
    // the next api() makes a new one with the same options and hooks.
    void restart();

    // Starts a scan of the card and returns its session.
    [[nodiscard]] std::string startCardScan(ScanSettings settings = {});
    // Waits for the session's operation to end; its progress then.
    [[nodiscard]] Progress waitIdle(std::string_view session);

private:
    ::recovery::test::TempDir folder_;
    std::shared_ptr<DiskRack> rack_ = std::make_shared<DiskRack>();
    std::uint32_t nextDisk_ = 7;
    std::shared_ptr<std::mutex> placesMutex_ = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<std::pair<std::filesystem::path, std::uint32_t>>> places_ =
        std::make_shared<std::vector<std::pair<std::filesystem::path, std::uint32_t>>>();
    ApiOptions options_;
    PlatformHooks hooks_;
    EventLog log_;
    std::unique_ptr<RecoveryApi> api_;
    bool cardWritten_ = false;
};

// What a scan of the card delivers, one line per candidate (scan::test::describe).
[[nodiscard]] const std::vector<std::string>& expectedCard(ScanMode mode = ScanMode::Deep);
// The candidates of session `id` below `root`, one line each, read from its
// journal (no API may have it open).
[[nodiscard]] std::vector<std::string> journalCandidates(const std::filesystem::path& root, const std::string& id);
// Every candidate of an open session (all pages).
[[nodiscard]] std::vector<CandidateInfo> allCandidates(RecoveryApi& api, std::string_view session);
// The candidate named `name` ("PHOTO.JPG"), which must exist.
[[nodiscard]] CandidateInfo candidateNamed(RecoveryApi& api, std::string_view session, std::string_view name);

}  // namespace recovery::api::test
