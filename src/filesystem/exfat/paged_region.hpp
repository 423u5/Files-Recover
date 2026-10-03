#pragma once

// Private to the exFAT module.

#include "filesystem/filesystem.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace recovery::filesystem::exfat {

// Read-only view of a byte range of the volume stored in one or more
// extents (in order), read through a bounded LRU cache of fixed-size pages.
//
// A page that cannot be read in one piece is re-read sector by sector, and
// sectors that still fail are marked unreadable. Sectors stored beyond the
// end of the source are unreadable without any read attempt, so a boot
// sector describing a huge volume cannot cause a flood of failing reads.
class PagedRegion {
public:
    static constexpr std::uint32_t kPageBytes = 64 * 1024;

    struct Page {
        // Bytes of the region in this page (kPageBytes except for the last page).
        std::size_t length = 0;
        // The page's bytes; empty when noneReadable.
        std::vector<std::byte> bytes;
        // One flag per sector; empty when every sector of the page was read.
        std::vector<bool> sectorReadable;
        bool noneReadable = false;

        [[nodiscard]] bool readable(std::size_t offset, std::uint32_t sectorSize) const noexcept {
            return !noneReadable && (sectorReadable.empty() || sectorReadable[offset / sectorSize]);
        }
    };

    // Every extent length except the last must be a multiple of `sectorSize`.
    PagedRegion(storage::IStorageSource& volume, std::vector<Extent> extents, std::uint32_t sectorSize,
                std::size_t cacheBytes);

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint64_t pageCount() const noexcept { return (size_ + kPageBytes - 1) / kPageBytes; }
    [[nodiscard]] std::uint32_t sectorSize() const noexcept { return sectorSize_; }

    // Page `index` (< pageCount()). The reference stays valid until the next call.
    [[nodiscard]] const Page& page(std::uint64_t index);

private:
    struct CachedPage {
        Page page;
        std::list<std::uint64_t>::iterator lru;
    };

    [[nodiscard]] Page load(std::uint64_t index) const;
    // Volume offset of region offset `offset`, and the bytes left in its extent.
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> locate(std::uint64_t offset) const noexcept;

    storage::IStorageSource& volume_;
    std::vector<Extent> extents_;
    std::vector<std::uint64_t> starts_;  // region offset of each extent
    std::uint64_t size_ = 0;
    std::uint32_t sectorSize_;
    std::size_t maxPages_;
    std::unordered_map<std::uint64_t, CachedPage> pages_;
    std::list<std::uint64_t> lru_;  // most recently used first
};

}  // namespace recovery::filesystem::exfat
