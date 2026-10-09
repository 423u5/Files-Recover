#pragma once

// What the API (P19) takes from the platform, for tests and tools that
// simulate it: how physical disks are opened, listed and told apart, the
// playability checker, and a hook on every progress report of an operation.
//
// Not for user interfaces: unlike recovery_api.hpp and api_types.hpp, this
// header includes engine headers. A user interface calls RecoveryApi::create().

#include "api/recovery_api.hpp"
#include "storage/destination_guard.hpp"
#include "storage/device_io.hpp"
#include "storage/disk_list.hpp"
#include "validation/playability.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace recovery::api {

struct PlatformHooks {
    // Opens physical disks (null: the platform's, read-only CreateFileW).
    // Image files are always opened by the platform.
    std::shared_ptr<storage::IDeviceOpener> diskOpener;
    // Tells which physical disks a path is on (empty: the platform's).
    storage::DiskResolver diskResolver;
    // Lists the disks attached (empty: the platform's).
    storage::DiskLister diskLister;
    // The Windows folder, whose disk is the system disk (empty: %SystemRoot%).
    std::filesystem::path systemFolder;
    // Makes the checker of a scan that validates playability (empty:
    // Windows' decoders).
    std::function<std::unique_ptr<validation::IPlayabilityChecker>()> playability;
    // Told every progress report of an operation, on the operation's thread,
    // before the API tells the user interface (so a pause or a cancellation
    // asked from here takes effect at a known point). `session` is the
    // session's id, empty for an imaging.
    std::function<void(std::string_view session, const Progress& progress)> onOperationProgress;
    // Recovered files the event queue holds before it drops FilesRecovered
    // events (0: 100,000).
    std::uint64_t maxQueuedFiles = 0;
};

// RecoveryApi::create() with `hooks` in place of the platform's.
[[nodiscard]] Result<std::unique_ptr<RecoveryApi>> createRecoveryApi(ApiOptions options, PlatformHooks hooks);

}  // namespace recovery::api
