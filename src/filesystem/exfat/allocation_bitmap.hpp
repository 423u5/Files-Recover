#pragma once

// Private to the exFAT module.

#include "paged_region.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace recovery::filesystem::exfat {

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

// The allocation bitmap: bit n records whether cluster n + 2 is in use.
// This, not the FAT, is what says whether an exFAT cluster is free.
class AllocationBitmap {
public:
    // `extents` hold the bitmap's first `bytes` bytes; clusters whose bits lie
    // beyond them are reported Unreadable.
    AllocationBitmap(storage::IStorageSource& volume, std::vector<Extent> extents, std::uint32_t clusterCount,
                     std::uint32_t sectorSize, std::size_t cacheBytes);

    // `cluster` must be in [2, clusterCount + 1].
    [[nodiscard]] BitState state(std::uint32_t cluster);

    // Counts clusters [first, first + count) by state; the range must lie in
    // [2, clusterCount + 1]. With `stopAt`, counting ends at the first cluster
    // in that state (the counts then cover only the clusters examined).
    // Unreadable pages are skipped whole and full bytes are counted at once,
    // so the cost is bounded by the bitmap bytes actually stored in the source.
    [[nodiscard]] BitCounts count(std::uint64_t first, std::uint64_t count, std::optional<BitState> stopAt = {});

private:
    PagedRegion region_;
    std::uint64_t coveredClusters_;  // clusters whose bits lie inside the stored bitmap
};

}  // namespace recovery::filesystem::exfat
