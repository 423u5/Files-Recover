#pragma once

// Private to the FAT32 module.

#include "filesystem/fat32/fat32_boot_sector.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <span>
#include <unordered_map>
#include <vector>

namespace recovery::filesystem::fat32 {

enum class FatEntryKind : std::uint8_t {
    Free,
    Next,        // value is the next cluster (validated to be in range)
    EndOfChain,
    Bad,
    Reserved,    // value 1
    OutOfRange,  // points past the last cluster
    Unreadable,  // no FAT copy could be read here
};

struct FatEntry {
    FatEntryKind kind = FatEntryKind::Unreadable;
    std::uint32_t value = 0;  // 28-bit entry value
};

// Access to the allocation table with a bounded LRU page cache.
//
// A page that cannot be read from the active FAT is re-read sector by
// sector, each sector from the first FAT copy that can supply it. Only
// sectors unreadable in every copy report Unreadable.
class FatTable {
public:
    FatTable(storage::IStorageSource& volume, const BootSector& boot, std::size_t cacheBytes);

    // Entry for `cluster`; clusters outside [0, lastCluster] report OutOfRange.
    [[nodiscard]] FatEntry entry(std::uint32_t cluster);
    [[nodiscard]] FatEntry classify(std::uint32_t raw) const noexcept;

    // Reads raw entries [first, first + out.size()) from FAT copy `copy`
    // without caching. Returns false if the range is unreadable.
    [[nodiscard]] bool readRaw(std::uint32_t copy, std::uint32_t first, std::span<std::uint32_t> out);

    // Number of leading entries of FAT copy `copy` stored inside the volume
    // source (fewer than lastCluster + 1 on a truncated image).
    [[nodiscard]] std::uint64_t entriesInsideVolume(std::uint32_t copy) const noexcept;

    // Number of FAT sectors served from a copy other than the active FAT.
    [[nodiscard]] std::size_t fallbackReads() const noexcept { return fallbackReads_; }

private:
    struct Page {
        std::vector<std::uint32_t> entries;
        // Empty when the whole page was read; otherwise one flag per FAT sector.
        std::vector<bool> sectorReadable;
        std::list<std::uint32_t>::iterator lru;
    };

    [[nodiscard]] Page& page(std::uint32_t index);

    storage::IStorageSource& volume_;
    const BootSector& boot_;
    std::uint32_t pageEntries_;
    std::uint32_t sectorEntries_;
    std::size_t maxPages_;
    std::unordered_map<std::uint32_t, Page> pages_;
    std::list<std::uint32_t> lru_;  // most recently used first
    std::size_t fallbackReads_ = 0;
};

}  // namespace recovery::filesystem::fat32
