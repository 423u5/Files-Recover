#pragma once

// Private to the exFAT module.

#include "filesystem/exfat/exfat_boot_sector.hpp"
#include "paged_region.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>

namespace recovery::filesystem::exfat {

inline constexpr std::uint32_t kFatBadCluster = 0xFFFFFFF7;

enum class FatEntryKind : std::uint8_t {
    Next,        // value is the next cluster (validated to be in range)
    EndOfChain,  // 0xFFFFFFFF (0xFFFFFFF8-0xFFFFFFFE are treated the same way)
    Bad,         // 0xFFFFFFF7
    Invalid,     // 0, 1, or a cluster number past the last cluster
    Unreadable,  // the FAT could not be read here
};

struct FatEntry {
    FatEntryKind kind = FatEntryKind::Unreadable;
    std::uint32_t value = 0;
};

// The active FAT, read through a bounded page cache.
//
// Unlike FAT32, the exFAT FAT does not record whether a cluster is free (the
// allocation bitmap does), and entries of clusters that belong to
// "NoFatChain" files are undefined. The second FAT of a TexFAT volume is a
// transaction copy, not a mirror, so it is never used as a fallback.
class FatTable {
public:
    FatTable(storage::IStorageSource& volume, const BootSector& boot, std::size_t cacheBytes);

    // Entry for `cluster`; clusters outside [2, lastCluster] report Invalid.
    [[nodiscard]] FatEntry entry(std::uint32_t cluster);
    [[nodiscard]] FatEntry classify(std::uint32_t raw) const noexcept;

    // Raw access for bulk analysis: entry n is stored at byte 4n.
    [[nodiscard]] PagedRegion& region() noexcept { return region_; }

private:
    const BootSector& boot_;
    PagedRegion region_;
};

}  // namespace recovery::filesystem::exfat
