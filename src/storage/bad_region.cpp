#include "storage/bad_region.hpp"

#include "recovery/checked_math.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

namespace recovery::storage {

namespace {

std::uint64_t saturatingEnd(std::uint64_t offset, std::uint64_t length) noexcept {
    return checkedAdd(offset, length).value_or(std::numeric_limits<std::uint64_t>::max());
}

}  // namespace

Status BadRegionMap::add(const BadRegion& region) {
    if (region.length == 0) {
        return makeError(ErrorCode::InvalidInput, "bad region has zero length");
    }
    if (!checkedAdd(region.offset, region.length).has_value()) {
        return makeError(ErrorCode::InvalidInput, "bad region end overflows");
    }

    BadRegion merged = region;
    bool adoptedExistingCode = false;
    auto it = regions_.upper_bound(merged.offset);
    if (it != regions_.begin()) {
        const auto previous = std::prev(it);
        if (previous->second.end() >= merged.offset) {
            it = previous;
        }
    }

    while (it != regions_.end() && it->second.offset <= merged.end()) {
        const BadRegion existing = it->second;
        const bool overlaps = existing.offset < merged.end() && merged.offset < existing.end();
        if (!overlaps && existing.errorCode != merged.errorCode) {
            ++it;  // merely touching, different cause: keep separate
            continue;
        }
        const std::uint64_t start = std::min(existing.offset, merged.offset);
        const std::uint64_t end = std::max(existing.end(), merged.end());
        if (overlaps && !adoptedExistingCode) {
            merged.errorCode = existing.errorCode;
            adoptedExistingCode = true;
        }
        merged.offset = start;
        merged.length = end - start;
        totalBytes_ -= existing.length;
        it = regions_.erase(it);
    }

    regions_.emplace(merged.offset, merged);
    totalBytes_ += merged.length;
    return success();
}

bool BadRegionMap::intersects(std::uint64_t offset, std::uint64_t length) const noexcept {
    if (length == 0 || regions_.empty()) {
        return false;
    }
    const std::uint64_t end = saturatingEnd(offset, length);
    auto it = regions_.upper_bound(offset);
    if (it != regions_.begin() && std::prev(it)->second.end() > offset) {
        return true;
    }
    return it != regions_.end() && it->second.offset < end;
}

std::vector<BadRegion> BadRegionMap::overlapping(std::uint64_t offset, std::uint64_t length) const {
    std::vector<BadRegion> result;
    if (length == 0) {
        return result;
    }
    const std::uint64_t end = saturatingEnd(offset, length);
    auto it = regions_.upper_bound(offset);
    if (it != regions_.begin() && std::prev(it)->second.end() > offset) {
        --it;
    }
    for (; it != regions_.end() && it->second.offset < end; ++it) {
        result.push_back(it->second);
    }
    return result;
}

std::vector<BadRegion> BadRegionMap::regions() const {
    std::vector<BadRegion> result;
    result.reserve(regions_.size());
    for (const auto& [offset, region] : regions_) {
        result.push_back(region);
    }
    return result;
}

void BadRegionMap::clear() noexcept {
    regions_.clear();
    totalBytes_ = 0;
}

}  // namespace recovery::storage
