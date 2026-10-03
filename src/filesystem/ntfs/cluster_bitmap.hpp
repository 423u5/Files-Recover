#pragma once

// Private to the NTFS module.

#include "../exfat/paged_region.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace recovery::filesystem::ntfs {

enum class BitState : std::uint8_t {
    Free,
    Allocated,
    Unreadable,  // the bitmap could not be read here, or does not reach this cluster
};

struct BitCounts {
    std::uint64_t free = 0;
    std::uint64_t allocated = 0;
    std::uint64_t unreadable = 0;
};

// The $Bitmap file: bit n records whether cluster n is in use.
//
// Pages are read through exFAT's bounded page cache (exfat::PagedRegion).
// Bits stored beyond the end of the source (a truncated image, or a boot
// sector claiming a huge volume) are Unreadable without being visited, so
// the cost of counting is bounded by the bitmap bytes the source holds.
class ClusterBitmap {
public:
    // `extents` hold the bitmap's first bytes, in order.
    ClusterBitmap(storage::IStorageSource& volume, std::vector<Extent> extents, std::uint64_t clusterCount,
                  std::uint32_t sectorSize, std::size_t cacheBytes);

    // `cluster` must be below the volume's cluster count.
    [[nodiscard]] BitState state(std::uint64_t cluster);

    // Counts clusters [first, first + count) by state; the range must lie
    // inside the volume. With `stopAt`, counting ends at the first cluster in
    // that state (the counts then cover only the clusters examined).
    [[nodiscard]] BitCounts count(std::uint64_t first, std::uint64_t count, std::optional<BitState> stopAt = {});

    // Clusters from 0 whose bits are stored before the end of the source;
    // every later cluster is Unreadable.
    [[nodiscard]] std::uint64_t coveredClusters() const noexcept { return coveredClusters_; }

private:
    exfat::PagedRegion region_;
    // Clusters whose bits are stored before the end of the source.
    std::uint64_t coveredClusters_ = 0;
};

// Region bytes, from the start, stored inside a source of `sourceSize` bytes:
// everything before the first byte of the extents (in order) that lies at or
// beyond the end of the source.
[[nodiscard]] std::uint64_t bytesInSource(const std::vector<Extent>& extents, std::uint64_t sourceSize) noexcept;

}  // namespace recovery::filesystem::ntfs
