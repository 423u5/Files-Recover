#pragma once

// Simulated physical disks by number, as Windows attaches them: opened as
// \\.\PhysicalDriveN, listed as the system lists them (storage::DiskLister),
// and moved to another number as a disk unplugged and plugged in again is
// (L147). Shared by the session, CLI and API tests; no real device is ever
// opened.

#include "storage/device_io.hpp"
#include "storage/disk_list.hpp"
#include "support/fake_device.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace recovery::test {

// The handles a rack has given out, shared with them (a handle may be closed
// after the rack is gone).
struct RackHandles {
    std::atomic<std::size_t> open{0};
    // How long closing a handle takes, in milliseconds.
    std::atomic<std::int64_t> closeDelay{0};
};

// Simulated physical disks, by number; any other number does not exist (it
// fails to open as Windows answers, ERROR_FILE_NOT_FOUND).
class DiskRack final : public storage::IDeviceOpener {
public:
    // Adds disk `number` holding `bytes` (a removable "Simulated Card
    // Reader", serial number `serial`), its reads failing (ERROR_CRC) in the
    // byte ranges `failing`.
    std::shared_ptr<FakeDeviceConfig> add(std::uint32_t number, std::vector<std::byte> bytes,
                                          std::vector<std::pair<std::uint64_t, std::uint64_t>> failing = {},
                                          std::string serial = "SIM-0001");
    // Puts disk `config` under `number` (another number's, or a new one).
    void attach(std::uint32_t number, std::shared_ptr<FakeDeviceConfig> config);
    // Takes disk `number` away; returns what it was (null when none).
    std::shared_ptr<FakeDeviceConfig> detach(std::uint32_t number);
    // Disk `from` gets number `to`, as when it is plugged in again.
    void renumber(std::uint32_t from, std::uint32_t to);
    // Opening disk `number` fails with `error` from now on.
    void failOpen(std::uint32_t number, Error error);
    // Closing a handle takes `delay` from now on, as a busy computer can make
    // it; the handle counts as open until it is closed.
    void closeSlowly(std::chrono::milliseconds delay) { handles_->closeDelay = delay.count(); }
    // The handles of the rack's disks open now.
    [[nodiscard]] std::size_t openHandles() const { return handles_->open; }
    // The numbers opened so far, in order.
    [[nodiscard]] std::vector<std::uint32_t> opened() const;

    // The disks attached, as the system's lister gives them (bus "USB").
    [[nodiscard]] storage::DiskLister lister();

    Result<std::unique_ptr<storage::IDeviceIo>> openReadOnly(const std::filesystem::path& path,
                                                             storage::DeviceKind kind) override;

private:
    struct State {
        std::mutex mutex;
        std::map<std::uint32_t, std::shared_ptr<FakeDeviceConfig>> disks;
        std::map<std::uint32_t, Error> failures;
        std::vector<std::uint32_t> opened;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::shared_ptr<RackHandles> handles_ = std::make_shared<RackHandles>();
};

}  // namespace recovery::test
