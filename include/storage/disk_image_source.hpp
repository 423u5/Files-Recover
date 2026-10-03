#pragma once

#include "storage/device_backed_source.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace recovery::storage {

struct DiskImageOptions {
    // Images carry no sector-size information; this is the logical sector
    // size of the device the image was taken from.
    std::uint32_t logicalSectorSize = 512;
};

// A raw (dd-style) disk image file, opened read-only.
//
// Windows assumption: the file is opened with GENERIC_READ and
// FILE_SHARE_READ only, so no process can write to the image while the
// source is open.
class DiskImageSource final : public DeviceBackedSource {
public:
    explicit DiskImageSource(std::filesystem::path imagePath, DiskImageOptions options = {},
                             std::shared_ptr<IDeviceOpener> opener = makePlatformDeviceOpener());
    ~DiskImageSource() override;

protected:
    [[nodiscard]] Status validateConfiguration() const override;
    [[nodiscard]] Result<DeviceGeometry> finalizeGeometry(const DeviceGeometry& reported) const override;

private:
    DiskImageOptions options_;
};

}  // namespace recovery::storage
