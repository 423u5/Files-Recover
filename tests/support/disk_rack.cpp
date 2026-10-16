#include "support/disk_rack.hpp"

#include "storage/physical_disk_source.hpp"

#include <optional>
#include <string>
#include <thread>

namespace recovery::test {

namespace {

// A handle of a rack's disk: counted while open, closed after the rack's delay.
class RackHandle final : public storage::IDeviceIo {
public:
    RackHandle(std::unique_ptr<storage::IDeviceIo> device, std::shared_ptr<RackHandles> handles)
        : device_(std::move(device)), handles_(std::move(handles)) {
        ++handles_->open;
    }
    ~RackHandle() override {
        std::this_thread::sleep_for(std::chrono::milliseconds(handles_->closeDelay.load()));
        device_.reset();
        --handles_->open;
    }
    RackHandle(const RackHandle&) = delete;
    RackHandle& operator=(const RackHandle&) = delete;
    RackHandle(RackHandle&&) = delete;
    RackHandle& operator=(RackHandle&&) = delete;

    Result<storage::DeviceGeometry> queryGeometry() override { return device_->queryGeometry(); }
    storage::DeviceDescription describe() override { return device_->describe(); }
    storage::DeviceReadResult readAt(std::uint64_t offset, std::span<std::byte> buffer) override {
        return device_->readAt(offset, buffer);
    }
    std::optional<bool> mediaPresent() override { return device_->mediaPresent(); }

private:
    std::unique_ptr<storage::IDeviceIo> device_;
    std::shared_ptr<RackHandles> handles_;
};

}  // namespace

std::shared_ptr<FakeDeviceConfig> DiskRack::add(std::uint32_t number, std::vector<std::byte> bytes,
                                                std::vector<std::pair<std::uint64_t, std::uint64_t>> failing,
                                                std::string serial) {
    auto config = std::make_shared<FakeDeviceConfig>();
    config->geometry = diskGeometry(bytes.size());
    config->data = std::move(bytes);
    config->description.vendor = "Simulated";
    config->description.product = "Card Reader";
    config->description.removable = true;
    config->description.serialNumber = std::move(serial);
    config->failingRanges = std::move(failing);
    attach(number, config);
    return config;
}

void DiskRack::attach(std::uint32_t number, std::shared_ptr<FakeDeviceConfig> config) {
    const std::lock_guard lock(state_->mutex);
    state_->disks[number] = std::move(config);
}

std::shared_ptr<FakeDeviceConfig> DiskRack::detach(std::uint32_t number) {
    const std::lock_guard lock(state_->mutex);
    const auto found = state_->disks.find(number);
    if (found == state_->disks.end()) {
        return nullptr;
    }
    std::shared_ptr<FakeDeviceConfig> config = std::move(found->second);
    state_->disks.erase(found);
    return config;
}

void DiskRack::renumber(std::uint32_t from, std::uint32_t to) {
    if (std::shared_ptr<FakeDeviceConfig> config = detach(from); config != nullptr) {
        attach(to, std::move(config));
    }
}

void DiskRack::failOpen(std::uint32_t number, Error error) {
    const std::lock_guard lock(state_->mutex);
    state_->failures[number] = std::move(error);
}

std::vector<std::uint32_t> DiskRack::opened() const {
    const std::lock_guard lock(state_->mutex);
    return state_->opened;
}

storage::DiskLister DiskRack::lister() {
    return [state = state_]() -> Result<std::vector<storage::AttachedDisk>> {
        const std::lock_guard lock(state->mutex);
        std::vector<storage::AttachedDisk> disks;
        for (const auto& [number, config] : state->disks) {
            storage::AttachedDisk disk;
            disk.number = number;
            disk.sizeBytes = config->geometry.sizeBytes;
            disk.logicalSectorSize = config->geometry.logicalSectorSize;
            disk.vendor = config->description.vendor;
            disk.product = config->description.product;
            disk.removable = config->description.removable;
            disk.serialNumber = config->description.serialNumber;
            disk.bus = "USB";
            disks.push_back(std::move(disk));
        }
        return disks;
    };
}

Result<std::unique_ptr<storage::IDeviceIo>> DiskRack::openReadOnly(const std::filesystem::path& path,
                                                                   storage::DeviceKind kind) {
    if (kind != storage::DeviceKind::PhysicalDisk) {
        return makeError(ErrorCode::InvalidInput, "the rack holds physical disks only");
    }
    const std::optional<std::uint32_t> number = storage::PhysicalDiskSource::parseDevicePath(path.native());
    const std::lock_guard lock(state_->mutex);
    if (number.has_value()) {
        state_->opened.push_back(*number);
        if (const auto failure = state_->failures.find(*number); failure != state_->failures.end()) {
            return failure->second;
        }
        if (const auto disk = state_->disks.find(*number); disk != state_->disks.end()) {
            return std::unique_ptr<storage::IDeviceIo>(std::make_unique<RackHandle>(
                std::make_unique<MemoryDeviceIo>(disk->second, std::make_shared<FakeDeviceStats>()), handles_));
        }
    }
    // ERROR_FILE_NOT_FOUND, as Windows answers for a disk that is not there.
    return makeError(ErrorCode::IoError, "no such disk", 2);
}

}  // namespace recovery::test
