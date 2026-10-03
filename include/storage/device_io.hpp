#pragma once

#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace recovery::storage {

// Low-level, platform-facing device access used by PhysicalDiskSource and
// DiskImageSource. Tests substitute in-memory implementations; the Windows
// implementation lives in src/storage/windows/.
//
// There is no write operation: a device can only ever be opened for reading.

enum class DeviceKind : std::uint8_t {
    PhysicalDisk,
    RegularFile,
};

struct DeviceGeometry {
    std::uint64_t sizeBytes = 0;
    // 0 when the device has no notion of sectors (regular files).
    std::uint32_t logicalSectorSize = 0;
    std::uint32_t physicalSectorSize = 0;
    // Offset, length and buffer-address alignment the OS requires for reads.
    // 1 means no requirement.
    std::uint32_t requiredAlignment = 1;
};

struct DeviceDescription {
    std::string vendor;
    std::string product;
    std::optional<bool> removable;
};

struct DeviceReadResult {
    std::size_t bytesRead = 0;
    std::uint32_t systemErrorCode = 0;
    // False when the OS reported an error. A successful read may return fewer
    // bytes than requested (end of file).
    bool success = false;
};

class IDeviceIo {
public:
    virtual ~IDeviceIo() = default;

    [[nodiscard]] virtual Result<DeviceGeometry> queryGeometry() = 0;
    [[nodiscard]] virtual DeviceDescription describe() { return {}; }

    // One positional read. Must not depend on or modify a shared file
    // position, so concurrent calls are safe.
    [[nodiscard]] virtual DeviceReadResult readAt(std::uint64_t offset, std::span<std::byte> buffer) = 0;
};

class IDeviceOpener {
public:
    virtual ~IDeviceOpener() = default;

    // Opens `path` for reading only.
    [[nodiscard]] virtual Result<std::unique_ptr<IDeviceIo>> openReadOnly(const std::filesystem::path& path,
                                                                          DeviceKind kind) = 0;
};

// The operating-system implementation (Win32 CreateFileW with GENERIC_READ only).
[[nodiscard]] std::shared_ptr<IDeviceOpener> makePlatformDeviceOpener();

}  // namespace recovery::storage
