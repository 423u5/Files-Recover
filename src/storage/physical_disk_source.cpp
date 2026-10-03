#include "storage/physical_disk_source.hpp"

#include <cwctype>
#include <string>

namespace recovery::storage {

namespace {

constexpr std::wstring_view kDevicePrefix = L"\\\\.\\PhysicalDrive";

}  // namespace

PhysicalDiskSource::PhysicalDiskSource(std::uint32_t diskNumber, std::shared_ptr<IDeviceOpener> opener)
    : DeviceBackedSource(SourceType::PhysicalDisk, devicePathFor(diskNumber), DeviceKind::PhysicalDisk,
                         std::move(opener)),
      diskNumber_(diskNumber) {}

PhysicalDiskSource::~PhysicalDiskSource() = default;

std::filesystem::path PhysicalDiskSource::devicePathFor(std::uint32_t diskNumber) {
    return std::filesystem::path(std::wstring(kDevicePrefix) + std::to_wstring(diskNumber));
}

std::optional<std::uint32_t> PhysicalDiskSource::parseDevicePath(std::wstring_view path) {
    if (path.size() <= kDevicePrefix.size()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kDevicePrefix.size(); ++i) {
        if (std::towupper(path[i]) != std::towupper(kDevicePrefix[i])) {
            return std::nullopt;
        }
    }
    const std::wstring_view digits = path.substr(kDevicePrefix.size());
    if (digits.size() > 4 || (digits.size() > 1 && digits[0] == L'0')) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const wchar_t c : digits) {
        if (c < L'0' || c > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint32_t>(c - L'0');
    }
    if (value > kMaxDiskNumber) {
        return std::nullopt;
    }
    return value;
}

Status PhysicalDiskSource::validateConfiguration() const {
    if (diskNumber_ > kMaxDiskNumber) {
        return makeError(ErrorCode::InvalidInput, "physical disk number " + std::to_string(diskNumber_) +
                                                      " exceeds the supported maximum of " +
                                                      std::to_string(kMaxDiskNumber));
    }
    return success();
}

Result<DeviceGeometry> PhysicalDiskSource::finalizeGeometry(const DeviceGeometry& reported) const {
    if (!isValidSectorSize(reported.logicalSectorSize)) {
        return makeError(ErrorCode::IoError, "physical disk reported an invalid sector size " +
                                                 std::to_string(reported.logicalSectorSize));
    }
    if (reported.sizeBytes == 0) {
        return makeError(ErrorCode::IoError, "physical disk reported a size of zero (no media?)");
    }
    if (reported.sizeBytes % reported.logicalSectorSize != 0) {
        return makeError(ErrorCode::IoError, "physical disk size " + std::to_string(reported.sizeBytes) +
                                                 " is not a multiple of its sector size");
    }
    DeviceGeometry geometry = reported;
    // Raw disk I/O must be sector-aligned in offset, length and memory.
    if (geometry.requiredAlignment < geometry.logicalSectorSize) {
        geometry.requiredAlignment = geometry.logicalSectorSize;
    }
    return geometry;
}

void PhysicalDiskSource::decorateInfo(SourceInfo& info) const {
    info.diskNumber = diskNumber_;
}

}  // namespace recovery::storage
