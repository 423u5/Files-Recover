#pragma once

#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace recovery::storage {

// A byte range of the source that could not be read.
struct BadRegion {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    // Win32 error reported for the region (e.g. 23 = ERROR_CRC).
    std::uint32_t errorCode = 0;

    [[nodiscard]] std::uint64_t end() const noexcept { return offset + length; }

    friend bool operator==(const BadRegion&, const BadRegion&) = default;
};

// Sorted, non-overlapping collection of unreadable regions.
//
// Adjacent regions with the same error code are merged, so a run of failing
// sectors is stored as one region. Overlapping regions are always merged;
// the error code recorded first wins.
//
// Thread safety: none; callers synchronise.
class BadRegionMap {
public:
    // Rejects zero-length regions and ranges whose end overflows.
    [[nodiscard]] Status add(const BadRegion& region);

    [[nodiscard]] bool intersects(std::uint64_t offset, std::uint64_t length) const noexcept;
    [[nodiscard]] std::vector<BadRegion> overlapping(std::uint64_t offset, std::uint64_t length) const;
    [[nodiscard]] std::vector<BadRegion> regions() const;

    [[nodiscard]] std::uint64_t totalBytes() const noexcept { return totalBytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return regions_.size(); }
    [[nodiscard]] bool empty() const noexcept { return regions_.empty(); }
    void clear() noexcept;

private:
    std::map<std::uint64_t, BadRegion> regions_;  // keyed by offset
    std::uint64_t totalBytes_ = 0;
};

}  // namespace recovery::storage
