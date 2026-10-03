#pragma once

#include "storage/device_backed_source.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace recovery::storage {

// A whole physical disk (\\.\PhysicalDriveN), opened read-only.
//
// Windows assumptions:
//  * Opening a physical drive requires administrator rights.
//  * The handle is opened with GENERIC_READ only. FILE_SHARE_READ |
//    FILE_SHARE_WRITE is required because mounted volumes on the disk keep
//    their own handles; the engine never locks, dismounts or writes.
//  * Raw disk reads must be sector-aligned; unaligned requests are served
//    through an aligned bounce buffer.
class PhysicalDiskSource final : public DeviceBackedSource {
public:
    static constexpr std::uint32_t kMaxDiskNumber = 1023;

    explicit PhysicalDiskSource(std::uint32_t diskNumber,
                                std::shared_ptr<IDeviceOpener> opener = makePlatformDeviceOpener());
    ~PhysicalDiskSource() override;

    [[nodiscard]] std::uint32_t diskNumber() const noexcept { return diskNumber_; }

    // "\\.\PhysicalDrive<N>"
    [[nodiscard]] static std::filesystem::path devicePathFor(std::uint32_t diskNumber);
    // Parses "\\.\PhysicalDrive<N>" (case-insensitive); nullopt for anything else.
    [[nodiscard]] static std::optional<std::uint32_t> parseDevicePath(std::wstring_view path);

protected:
    [[nodiscard]] Status validateConfiguration() const override;
    [[nodiscard]] Result<DeviceGeometry> finalizeGeometry(const DeviceGeometry& reported) const override;
    void decorateInfo(SourceInfo& info) const override;

private:
    std::uint32_t diskNumber_;
};

}  // namespace recovery::storage
