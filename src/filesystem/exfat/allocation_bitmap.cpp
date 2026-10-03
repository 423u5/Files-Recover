#include "allocation_bitmap.hpp"

#include "recovery/byte_order.hpp"

#include <algorithm>
#include <bit>

namespace recovery::filesystem::exfat {

namespace {
constexpr std::uint64_t kPageBits = PagedRegion::kPageBytes * 8ULL;
}  // namespace

AllocationBitmap::AllocationBitmap(storage::IStorageSource& volume, std::vector<Extent> extents,
                                   std::uint32_t clusterCount, std::uint32_t sectorSize, std::size_t cacheBytes)
    : region_(volume, std::move(extents), sectorSize, cacheBytes),
      coveredClusters_(std::min<std::uint64_t>(clusterCount, region_.size() * 8)) {}

BitState AllocationBitmap::state(std::uint32_t cluster) {
    const std::uint64_t bit = cluster - 2ULL;
    if (bit >= coveredClusters_) {
        return BitState::Unreadable;
    }
    const std::uint64_t byte = bit / 8;
    const PagedRegion::Page& page = region_.page(byte / PagedRegion::kPageBytes);
    const auto within = static_cast<std::size_t>(byte % PagedRegion::kPageBytes);
    if (!page.readable(within, region_.sectorSize())) {
        return BitState::Unreadable;
    }
    return ((loadU8(page.bytes, within) >> (bit % 8)) & 1U) != 0 ? BitState::Allocated : BitState::Free;
}

BitCounts AllocationBitmap::count(std::uint64_t first, std::uint64_t count, std::optional<BitState> stopAt) {
    BitCounts counts;
    const auto stopsAt = [&stopAt](BitState state) { return stopAt.has_value() && *stopAt == state; };
    const std::uint32_t sector = region_.sectorSize();
    std::uint64_t bit = first - 2;
    const std::uint64_t end = bit + count;
    const std::uint64_t coveredEnd = std::min(end, coveredClusters_);

    while (bit < coveredEnd) {
        const std::uint64_t pageIndex = bit / kPageBits;
        const PagedRegion::Page& page = region_.page(pageIndex);
        const std::uint64_t pageStart = pageIndex * kPageBits;
        const std::uint64_t pageStop = std::min<std::uint64_t>(coveredEnd, pageStart + page.length * 8ULL);
        if (page.noneReadable) {
            counts.unreadable += pageStop - bit;
            bit = pageStop;
            if (stopsAt(BitState::Unreadable)) {
                return counts;
            }
            continue;
        }
        while (bit < pageStop) {
            const auto byte = static_cast<std::size_t>((bit - pageStart) / 8);
            if (!page.readable(byte, sector)) {
                const std::uint64_t sectorStop =
                    std::min<std::uint64_t>(pageStop, pageStart + (byte / sector + 1) * sector * 8ULL);
                counts.unreadable += sectorStop - bit;
                bit = sectorStop;
                if (stopsAt(BitState::Unreadable)) {
                    return counts;
                }
                continue;
            }
            // Whole 64-bit words at once; a word never straddles a sector.
            if (bit % 64 == 0 && pageStop - bit >= 64) {
                const auto set = static_cast<std::uint64_t>(std::popcount(loadLe64(page.bytes, byte)));
                counts.allocated += set;
                counts.free += 64 - set;
                bit += 64;
                if ((set > 0 && stopsAt(BitState::Allocated)) || (set < 64 && stopsAt(BitState::Free))) {
                    return counts;
                }
                continue;
            }
            const bool set = ((loadU8(page.bytes, byte) >> (bit % 8)) & 1U) != 0;
            ++(set ? counts.allocated : counts.free);
            ++bit;
            if (stopsAt(set ? BitState::Allocated : BitState::Free)) {
                return counts;
            }
        }
    }
    if (bit < end) {
        counts.unreadable += end - bit;  // beyond the stored bitmap
    }
    return counts;
}

}  // namespace recovery::filesystem::exfat
