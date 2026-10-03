#pragma once

#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace recovery::filesystem::fat32 {

// The FAT specification requires at least this many clusters for FAT32.
// Smaller FAT32 volumes exist in practice (made by non-Windows tools); they
// are accepted with a warning.
inline constexpr std::uint32_t kMinStandardClusters = 65525;
// Largest cluster count addressable by 28-bit FAT32 entries (clusters 2..0x0FFFFFF6).
inline constexpr std::uint32_t kMaxClusters = 0x0FFFFFF5;
inline constexpr std::uint32_t kMaxClusterSize = 256 * 1024;

// Validated FAT32 boot sector (BIOS parameter block) with derived layout.
struct BootSector {
    // Raw fields.
    std::string oemName;
    std::uint16_t bytesPerSector = 0;
    std::uint8_t sectorsPerCluster = 0;
    std::uint16_t reservedSectors = 0;
    std::uint8_t fatCount = 0;
    std::uint8_t media = 0;
    std::uint32_t totalSectors = 0;
    std::uint32_t fatSectors = 0;  // per FAT copy
    std::uint16_t extFlags = 0;
    std::uint32_t rootCluster = 0;
    std::uint16_t fsInfoSector = 0;
    std::uint16_t backupBootSector = 0;
    std::uint32_t volumeId = 0;
    std::string volumeLabel;  // from the BPB, trimmed; empty for "NO NAME"

    // Derived layout (bytes are volume offsets).
    std::uint32_t clusterSize = 0;
    std::uint64_t fatOffset = 0;      // first FAT copy
    std::uint64_t fatBytes = 0;       // size of one FAT copy
    std::uint64_t dataOffset = 0;     // cluster 2
    std::uint64_t volumeBytes = 0;    // totalSectors * bytesPerSector
    std::uint32_t clusterCount = 0;   // data clusters, numbered 2 .. clusterCount + 1
    std::uint32_t activeFat = 0;      // copy to read
    bool mirrored = true;             // all copies are kept identical

    std::vector<std::string> warnings;

    [[nodiscard]] std::uint32_t lastCluster() const noexcept { return clusterCount + 1; }
    [[nodiscard]] bool isValidCluster(std::uint64_t cluster) const noexcept {
        return cluster >= 2 && cluster <= lastCluster();
    }
    [[nodiscard]] std::uint64_t clusterOffset(std::uint32_t cluster) const noexcept {
        return dataOffset + static_cast<std::uint64_t>(cluster - 2) * clusterSize;
    }
};

// Parses a FAT32 boot sector. `sourceSize` is the size of the containing
// volume source; a boot sector describing more is accepted with a warning
// (truncated image). Returns UnsupportedFilesystem for non-FAT32 sectors
// (including FAT12/FAT16) and CorruptedFilesystem for FAT32 sectors with
// impossible values.
[[nodiscard]] Result<BootSector> parseBootSector(std::span<const std::byte> sector, std::uint64_t sourceSize);

// FSInfo sector contents, when its signatures are valid.
struct FsInfo {
    std::optional<std::uint32_t> freeClusters;
    std::optional<std::uint32_t> nextFree;
};

[[nodiscard]] std::optional<FsInfo> parseFsInfo(std::span<const std::byte> sector, const BootSector& boot);

}  // namespace recovery::filesystem::fat32
