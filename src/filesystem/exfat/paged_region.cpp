#include "paged_region.hpp"

#include "recovery/checked_math.hpp"

#include <algorithm>

namespace recovery::filesystem::exfat {

PagedRegion::PagedRegion(storage::IStorageSource& volume, std::vector<Extent> extents, std::uint32_t sectorSize,
                         std::size_t cacheBytes)
    : volume_(volume),
      extents_(std::move(extents)),
      sectorSize_(sectorSize),
      maxPages_(std::max<std::size_t>(2, cacheBytes / kPageBytes)) {
    starts_.reserve(extents_.size());
    for (const Extent& extent : extents_) {
        starts_.push_back(size_);
        size_ += extent.length;  // extents lie inside a volume, so the sum cannot overflow
    }
}

std::pair<std::uint64_t, std::uint64_t> PagedRegion::locate(std::uint64_t offset) const noexcept {
    const auto next = std::upper_bound(starts_.begin(), starts_.end(), offset);
    const auto index = static_cast<std::size_t>(next - starts_.begin()) - 1;  // offset < size_, so index is valid
    const std::uint64_t within = offset - starts_[index];
    return {extents_[index].offset + within, extents_[index].length - within};
}

PagedRegion::Page PagedRegion::load(std::uint64_t index) const {
    const std::uint64_t first = index * kPageBytes;
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(kPageBytes, size_ - first));
    Page loaded;
    loaded.length = count;
    const std::uint64_t sourceSize = volume_.size();

    // A page stored entirely beyond the end of the source costs nothing.
    bool anyInside = false;
    for (std::size_t done = 0; done < count && !anyInside;) {
        const auto [volumeOffset, available] = locate(first + done);
        anyInside = volumeOffset < sourceSize;
        done += static_cast<std::size_t>(std::min<std::uint64_t>(available, count - done));
    }
    if (!anyInside) {
        loaded.noneReadable = true;
        return loaded;
    }
    loaded.bytes.assign(count, std::byte{0});

    // Whole page, one extent piece at a time.
    bool complete = true;
    for (std::size_t done = 0; done < count;) {
        const auto [volumeOffset, available] = locate(first + done);
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(available, count - done));
        const std::span<std::byte> piece(loaded.bytes.data() + done, length);
        if (!rangeWithin<std::uint64_t>(volumeOffset, length, sourceSize) ||
            !volume_.readExact(ByteOffset{volumeOffset}, piece).ok()) {
            complete = false;
            break;
        }
        done += length;
    }
    if (complete) {
        return loaded;
    }

    // Narrow the failure down to single sectors.
    std::fill(loaded.bytes.begin(), loaded.bytes.end(), std::byte{0});
    const std::size_t sectors = (count + sectorSize_ - 1) / sectorSize_;
    loaded.sectorReadable.assign(sectors, false);
    bool any = false;
    for (std::size_t s = 0; s < sectors; ++s) {
        const std::size_t begin = s * sectorSize_;
        const auto [volumeOffset, available] = locate(first + begin);
        const auto length = static_cast<std::size_t>(
            std::min<std::uint64_t>({available, sectorSize_, static_cast<std::uint64_t>(count - begin)}));
        const std::span<std::byte> piece(loaded.bytes.data() + begin, length);
        if (rangeWithin<std::uint64_t>(volumeOffset, length, sourceSize) &&
            volume_.readExact(ByteOffset{volumeOffset}, piece).ok()) {
            loaded.sectorReadable[s] = true;
            any = true;
        } else {
            std::fill(piece.begin(), piece.end(), std::byte{0});
        }
    }
    if (!any) {
        loaded.noneReadable = true;
        loaded.bytes.clear();
    }
    return loaded;
}

const PagedRegion::Page& PagedRegion::page(std::uint64_t index) {
    if (const auto found = pages_.find(index); found != pages_.end()) {
        if (found->second.lru != lru_.begin()) {
            lru_.splice(lru_.begin(), lru_, found->second.lru);
        }
        return found->second.page;
    }
    if (pages_.size() >= maxPages_) {
        pages_.erase(lru_.back());
        lru_.pop_back();
    }
    Page loaded = load(index);
    lru_.push_front(index);
    return pages_.emplace(index, CachedPage{std::move(loaded), lru_.begin()}).first->second.page;
}

}  // namespace recovery::filesystem::exfat
