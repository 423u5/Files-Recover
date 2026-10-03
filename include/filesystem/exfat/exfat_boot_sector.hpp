#pragma once

#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace recovery::filesystem::exfat {

// Largest cluster count the specification allows (2^32 - 11).
inline constexpr std::uint32_t kMaxClusters = 0xFFFFFFF5;
// Largest cluster: BytesPerSectorShift + SectorsPerClusterShift <= 25.
inline constexpr std::uint32_t kMaxClusterSize = 32U * 1024U * 1024U;
// Sectors in one boot region (boot sector, 8 extended boot sectors, OEM
// parameters, reserved sector, checksum sector). The backup follows the main region.
inline constexpr std::uint32_t kBootRegionSectors = 12;
// The specification's minimum volume size.
inline constexpr std::uint64_t kMinVolumeBytes = 1024 * 1024;

// Validated exFAT main (or backup) boot sector with derived layout.
struct BootSector {
    // Raw fields (sector counts are in units of bytesPerSector).
    std::uint64_t partitionOffset = 0;  // informational only
    std::uint64_t volumeLengthSectors = 0;
    std::uint32_t fatOffsetSectors = 0;
    std::uint32_t fatLengthSectors = 0;  // per FAT
    std::uint32_t clusterHeapOffsetSectors = 0;
    std::uint32_t recordedClusterCount = 0;
    std::uint32_t rootCluster = 0;
    std::uint32_t volumeSerial = 0;
    std::uint8_t revisionMajor = 0;
    std::uint8_t revisionMinor = 0;
    std::uint16_t volumeFlags = 0;
    std::uint8_t bytesPerSectorShift = 0;
    std::uint8_t sectorsPerClusterShift = 0;
    std::uint8_t fatCount = 0;
    std::uint8_t percentInUse = 0xFF;  // 0xFF: not recorded

    // Derived layout (bytes are volume offsets).
    std::uint32_t bytesPerSector = 0;
    std::uint32_t clusterSize = 0;
    std::uint64_t fatOffset = 0;    // first FAT
    std::uint64_t fatBytes = 0;     // size of one FAT
    std::uint64_t dataOffset = 0;   // cluster 2 (start of the cluster heap)
    std::uint64_t volumeBytes = 0;  // volumeLengthSectors * bytesPerSector
    // Usable clusters, numbered 2 .. clusterCount + 1. Can be lower than
    // recordedClusterCount when the heap or the FAT is too small for it.
    std::uint32_t clusterCount = 0;
    // FAT and allocation bitmap in use (VolumeFlags bit 0; only 1 with two FATs).
    std::uint32_t activeFat = 0;
    bool dirty = false;         // VolumeFlags bit 1: not cleanly unmounted
    bool mediaFailure = false;  // VolumeFlags bit 2

    std::vector<std::string> warnings;

    [[nodiscard]] std::uint32_t lastCluster() const noexcept { return clusterCount + 1; }
    [[nodiscard]] bool isValidCluster(std::uint64_t cluster) const noexcept {
        return cluster >= 2 && cluster <= lastCluster();
    }
    [[nodiscard]] std::uint64_t clusterOffset(std::uint64_t cluster) const noexcept {
        return dataOffset + (cluster - 2) * clusterSize;
    }
};

// Parses an exFAT boot sector (the first 512 bytes of it are enough).
// `sourceSize` is the size of the containing volume source; a boot sector
// describing more is accepted with a warning (truncated image). Returns
// UnsupportedFilesystem for anything that is not exFAT (including FAT and
// NTFS) or an unsupported revision, and CorruptedFilesystem for exFAT boot
// sectors with impossible values.
[[nodiscard]] Result<BootSector> parseBootSector(std::span<const std::byte> sector, std::uint64_t sourceSize);

// Boot checksum over the first 11 sectors of a boot region, skipping the
// VolumeFlags and PercentInUse bytes (exFAT specification 3.4). `region`
// must hold at least 11 * bytesPerSector bytes.
[[nodiscard]] std::uint32_t bootChecksum(std::span<const std::byte> region, std::uint32_t bytesPerSector) noexcept;

// True when sector 11 of `region` (12 sectors) repeats the checksum of sectors 0-10.
[[nodiscard]] bool bootChecksumMatches(std::span<const std::byte> region, std::uint32_t bytesPerSector) noexcept;

}  // namespace recovery::filesystem::exfat
