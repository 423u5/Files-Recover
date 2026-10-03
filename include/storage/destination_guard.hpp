#pragma once

#include "recovery/result.hpp"
#include "storage/storage_source.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace recovery::storage {

// Returns the physical disk numbers backing the volume that contains
// `existingPath` (several for spanned/striped volumes).
using DiskResolver = std::function<Result<std::vector<std::uint32_t>>(const std::filesystem::path& existingPath)>;

// Windows implementation: volume mount point -> volume GUID ->
// IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS. The volume is opened with zero
// access rights (query only).
[[nodiscard]] DiskResolver makePlatformDiskResolver();

// Verifies that writing `destination` cannot modify `source`:
//  * the destination is a regular path, not a device;
//  * for images: the destination is not the image file itself;
//  * for physical disks: the destination volume does not live on the source
//    disk. If this cannot be determined (e.g. network paths), the check
//    fails closed with DestinationError.
[[nodiscard]] Status checkDestinationSafety(const SourceInfo& source, const std::filesystem::path& destination,
                                            const DiskResolver& resolver);

}  // namespace recovery::storage
