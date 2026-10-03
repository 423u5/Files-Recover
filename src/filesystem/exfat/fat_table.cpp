#include "fat_table.hpp"

#include "recovery/byte_order.hpp"

#include <vector>

namespace recovery::filesystem::exfat {

namespace {

std::vector<Extent> activeFatExtent(const BootSector& boot) {
    // parseBootSector guarantees entries 0 .. lastCluster fit inside one FAT.
    const std::uint64_t offset = boot.fatOffset + static_cast<std::uint64_t>(boot.activeFat) * boot.fatBytes;
    return {Extent{offset, (static_cast<std::uint64_t>(boot.lastCluster()) + 1) * 4}};
}

}  // namespace

FatTable::FatTable(storage::IStorageSource& volume, const BootSector& boot, std::size_t cacheBytes)
    : boot_(boot), region_(volume, activeFatExtent(boot), boot.bytesPerSector, cacheBytes) {}

FatEntry FatTable::classify(std::uint32_t raw) const noexcept {
    FatEntry entry;
    entry.value = raw;
    if (raw == kFatBadCluster) {
        entry.kind = FatEntryKind::Bad;
    } else if (raw > kFatBadCluster) {
        entry.kind = FatEntryKind::EndOfChain;
    } else if (boot_.isValidCluster(raw)) {
        entry.kind = FatEntryKind::Next;
    } else {
        entry.kind = FatEntryKind::Invalid;
    }
    return entry;
}

FatEntry FatTable::entry(std::uint32_t cluster) {
    if (!boot_.isValidCluster(cluster)) {
        return FatEntry{FatEntryKind::Invalid, 0};
    }
    const std::uint64_t offset = static_cast<std::uint64_t>(cluster) * 4;
    const PagedRegion::Page& page = region_.page(offset / PagedRegion::kPageBytes);
    const auto within = static_cast<std::size_t>(offset % PagedRegion::kPageBytes);
    if (!page.readable(within, region_.sectorSize())) {
        return FatEntry{FatEntryKind::Unreadable, 0};
    }
    return classify(loadLe32(page.bytes, within));
}

}  // namespace recovery::filesystem::exfat
