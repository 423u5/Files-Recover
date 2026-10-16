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
// FAT16 and FAT12 (after P20): the specification's cluster counts. Fewer
// than kMinFat16Clusters is FAT12.
inline constexpr std::uint32_t kMinFat16Clusters = 4085;
inline constexpr std::uint32_t kMaxFat16Clusters = 65524;

// The module reads FAT32 and (after P20) FAT16 and FAT12: the same directory
// entries, long names and chains; FAT16 has 16-bit FAT entries and a root
// directory of fixed size between the FATs and cluster 2, and FAT12 the same
// root with 12-bit entries (two in three bytes).
enum class FatType : std::uint8_t {
    Fat32,
    Fat16,
    Fat12,
};

// Validated FAT boot sector (BIOS parameter block) with derived layout.
struct BootSector {
    FatType type = FatType::Fat32;
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
    // FAT12 and FAT16: the root directory's fixed region (FAT32: 0; its root is the chain at rootCluster).
    std::uint16_t rootEntries = 0;
    std::uint64_t rootDirOffset = 0;
    std::uint64_t rootDirBytes = 0;

    std::vector<std::string> warnings;

    // Bits per FAT entry: 32 (FAT32, of which 28 are used), 16 or 12.
    [[nodiscard]] std::uint32_t fatEntryBits() const noexcept {
        return type == FatType::Fat32 ? 32 : (type == FatType::Fat16 ? 16 : 12);
    }
    // FAT12 and FAT16 keep the root directory in a fixed region, which
    // directory entries call cluster 0; only FAT32 entries hold the high 16
    // bits of a cluster number.
    [[nodiscard]] bool hasFixedRoot() const noexcept { return type != FatType::Fat32; }

    [[nodiscard]] std::uint32_t lastCluster() const noexcept { return clusterCount + 1; }
    // A FAT32 volume of more than 65,535 clusters: a deleted entry whose start
    // cluster lost its top 16 bits (Windows clears them) may start at any of
    // several clusters 65,536 apart (L7).
    [[nodiscard]] bool deletedStartsAmbiguous() const noexcept {
        return type == FatType::Fat32 && lastCluster() > 0xFFFF;
    }
    [[nodiscard]] bool isValidCluster(std::uint64_t cluster) const noexcept {
        return cluster >= 2 && cluster <= lastCluster();
    }
    [[nodiscard]] std::uint64_t clusterOffset(std::uint32_t cluster) const noexcept {
        return dataOffset + static_cast<std::uint64_t>(cluster - 2) * clusterSize;
    }
};

// Parses a FAT32, FAT16 or FAT12 boot sector. `sourceSize` is the size of the
// containing volume source; a boot sector describing more is accepted with a
// warning (truncated image). The type is decided as the specification says,
// by the count of data clusters. Returns UnsupportedFilesystem for other
// sectors and CorruptedFilesystem for FAT sectors with impossible values.
[[nodiscard]] Result<BootSector> parseBootSector(std::span<const std::byte> sector, std::uint64_t sourceSize);

// FSInfo sector contents, when its signatures are valid.
struct FsInfo {
    std::optional<std::uint32_t> freeClusters;
    std::optional<std::uint32_t> nextFree;
};

[[nodiscard]] std::optional<FsInfo> parseFsInfo(std::span<const std::byte> sector, const BootSector& boot);

}  // namespace recovery::filesystem::fat32
