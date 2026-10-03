#include "storage/disk_image_source.hpp"

#include <string>

namespace recovery::storage {

DiskImageSource::DiskImageSource(std::filesystem::path imagePath, DiskImageOptions options,
                                 std::shared_ptr<IDeviceOpener> opener)
    : DeviceBackedSource(SourceType::DiskImage, std::move(imagePath), DeviceKind::RegularFile, std::move(opener)),
      options_(options) {}

DiskImageSource::~DiskImageSource() = default;

Status DiskImageSource::validateConfiguration() const {
    if (path().empty()) {
        return makeError(ErrorCode::InvalidInput, "disk image path is empty");
    }
    if (isDeviceNamespacePath(path())) {
        return makeError(ErrorCode::InvalidInput,
                         "disk image path refers to a device; use PhysicalDiskSource for physical drives");
    }
    if (!isValidSectorSize(options_.logicalSectorSize)) {
        return makeError(ErrorCode::InvalidInput,
                         "invalid image sector size " + std::to_string(options_.logicalSectorSize));
    }
    return success();
}

Result<DeviceGeometry> DiskImageSource::finalizeGeometry(const DeviceGeometry& reported) const {
    DeviceGeometry geometry = reported;
    geometry.logicalSectorSize = options_.logicalSectorSize;
    geometry.physicalSectorSize = options_.logicalSectorSize;
    // Image files have no alignment requirement and may legitimately end in
    // a partial sector (e.g. an interrupted image); only whole-sector reads
    // past the last complete sector are rejected.
    geometry.requiredAlignment = 1;
    return geometry;
}

}  // namespace recovery::storage
