#include "cluster_bitmap.hpp"

#include "recovery/byte_order.hpp"

#include <algorithm>
#include <bit>

namespace recovery::filesystem::ntfs {

namespace {
constexpr std::uint64_t kPageBits = exfat::PagedRegion::kPageBytes * 8ULL;
}  // namespace

std::uint64_t bytesInSource(const std::vector<Extent>& extents, std::uint64_t sourceSize) noexcept {
    std::uint64_t bytes = 0;
    for (const Extent& extent : extents) {
        if (extent.offset >= sourceSize) {
            break;
        }
        if (extent.length > sourceSize - extent.offset) {
            return bytes + (sourceSize - extent.offset);
        }
        bytes += extent.length;
    }
    return bytes;
}

ClusterBitmap::ClusterBitmap(storage::IStorageSource& volume, std::vector<Extent> extents, std::uint64_t clusterCount,
                             std::uint32_t sectorSize, std::size_t cacheBytes)
    : region_(volume, extents, sectorSize, cacheBytes) {
    const std::uint64_t reachable = bytesInSource(extents, volume.size());
    // The caller trims the extents to (clusterCount + 7) / 8 bytes, and a
    // volume has fewer than 2^56 clusters, so * 8 cannot overflow.
    coveredClusters_ = std::min(clusterCount, reachable * 8);
}

BitState ClusterBitmap::state(std::uint64_t cluster) {
    if (cluster >= coveredClusters_) {
        return BitState::Unreadable;
    }
    const std::uint64_t byte = cluster / 8;
    const exfat::PagedRegion::Page& page = region_.page(byte / exfat::PagedRegion::kPageBytes);
    const auto within = static_cast<std::size_t>(byte % exfat::PagedRegion::kPageBytes);
    if (!page.readable(within, region_.sectorSize())) {
        return BitState::Unreadable;
    }
    return ((loadU8(page.bytes, within) >> (cluster % 8)) & 1U) != 0 ? BitState::Allocated : BitState::Free;
}

BitCounts ClusterBitmap::count(std::uint64_t first, std::uint64_t count, std::optional<BitState> stopAt) {
    BitCounts counts;
    const auto stopsAt = [&stopAt](BitState state) { return stopAt.has_value() && *stopAt == state; };
    const std::uint32_t sector = region_.sectorSize();
    std::uint64_t bit = first;
    const std::uint64_t end = first + count;
    const std::uint64_t coveredEnd = std::min(end, coveredClusters_);

    while (bit < coveredEnd) {
        const std::uint64_t pageIndex = bit / kPageBits;
        const exfat::PagedRegion::Page& page = region_.page(pageIndex);
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
        counts.unreadable += end - bit;  // beyond the stored (or reachable) bitmap
    }
    return counts;
}

}  // namespace recovery::filesystem::ntfs
