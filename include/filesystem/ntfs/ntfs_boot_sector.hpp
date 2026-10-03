#pragma once

#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace recovery::filesystem::ntfs {

// Largest cluster Windows supports (2 MiB).
inline constexpr std::uint32_t kMaxClusterSize = 2U * 1024U * 1024U;
// Bounds for MFT and index record sizes. Update sequence fixups protect every
// 512-byte block of a record, so a record is a whole number of blocks.
inline constexpr std::uint32_t kMinRecordSize = 512;
inline constexpr std::uint32_t kMaxRecordSize = 64U * 1024U;

// Validated NTFS boot sector with derived layout.
struct BootSector {
    // Raw fields.
    std::uint16_t bytesPerSector = 0;
    std::uint8_t sectorsPerClusterRaw = 0;  // a power of two, or 256 - log2 for clusters above 64 KiB
    std::uint64_t totalSectors = 0;         // the volume, excluding the backup boot sector after it
    std::uint64_t mftCluster = 0;
    std::uint64_t mftMirrorCluster = 0;
    std::uint8_t recordSizeRaw = 0;       // clusters per MFT record, or 2^-n bytes when negative
    std::uint8_t indexRecordSizeRaw = 0;  // the same encoding, for directory index records
    std::uint64_t volumeSerial = 0;

    // Derived layout. NTFS numbers clusters from the start of the volume:
    // cluster 0 holds the boot sector.
    std::uint32_t sectorsPerCluster = 0;
    std::uint32_t clusterSize = 0;
    std::uint32_t recordSize = 0;       // MFT (FILE) record size
    std::uint32_t indexRecordSize = 0;  // 0 when the boot sector's value is invalid (not needed yet)
    std::uint64_t volumeBytes = 0;      // totalSectors * bytesPerSector
    // Clusters 0 .. clusterCount - 1 (a partial cluster at the end is unused).
    std::uint64_t clusterCount = 0;

    std::vector<std::string> warnings;

    [[nodiscard]] bool isValidCluster(std::uint64_t cluster) const noexcept { return cluster < clusterCount; }
    // `cluster` must be valid (the product is then below volumeBytes).
    [[nodiscard]] std::uint64_t clusterOffset(std::uint64_t cluster) const noexcept { return cluster * clusterSize; }
};

// Parses an NTFS boot sector (the first 512 bytes of it are enough).
// `sourceSize` is the size of the containing volume source; a boot sector
// describing more is accepted with a warning (truncated image). Returns
// UnsupportedFilesystem for anything that is not NTFS (including FAT and
// exFAT), and CorruptedFilesystem for NTFS boot sectors with impossible values.
[[nodiscard]] Result<BootSector> parseBootSector(std::span<const std::byte> sector, std::uint64_t sourceSize);

// Decodes the "clusters per record" encoding shared by MFT and index record
// sizes: a positive value counts clusters, a negative one (as a signed byte)
// is -log2 of the size in bytes. Returns 0 when the result is not a power of
// two within [kMinRecordSize, kMaxRecordSize].
[[nodiscard]] std::uint32_t decodeRecordSize(std::uint8_t raw, std::uint32_t clusterSize) noexcept;

}  // namespace recovery::filesystem::ntfs
