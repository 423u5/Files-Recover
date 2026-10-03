#include "fat_table.hpp"

#include "recovery/byte_order.hpp"

#include <algorithm>

namespace recovery::filesystem::fat32 {

namespace {
constexpr std::uint32_t kEntryMask = 0x0FFFFFFF;
constexpr std::uint32_t kBadCluster = 0x0FFFFFF7;
constexpr std::uint32_t kEndOfChainMin = 0x0FFFFFF8;
constexpr std::uint32_t kDefaultPageEntries = 16 * 1024;  // 64 KiB of FAT per page
}  // namespace

FatTable::FatTable(storage::IStorageSource& volume, const BootSector& boot, std::size_t cacheBytes)
    : volume_(volume),
      boot_(boot),
      pageEntries_(kDefaultPageEntries),
      sectorEntries_(boot.bytesPerSector / 4U),
      maxPages_(std::max<std::size_t>(2, cacheBytes / (kDefaultPageEntries * sizeof(std::uint32_t)))) {}

FatEntry FatTable::classify(std::uint32_t raw) const noexcept {
    const std::uint32_t value = raw & kEntryMask;
    FatEntry entry;
    entry.value = value;
    if (value == 0) {
        entry.kind = FatEntryKind::Free;
    } else if (value == 1) {
        entry.kind = FatEntryKind::Reserved;
    } else if (value >= kEndOfChainMin) {
        entry.kind = FatEntryKind::EndOfChain;
    } else if (value == kBadCluster) {
        entry.kind = FatEntryKind::Bad;
    } else if (value <= boot_.lastCluster()) {
        entry.kind = FatEntryKind::Next;
    } else {
        entry.kind = FatEntryKind::OutOfRange;
    }
    return entry;
}

std::uint64_t FatTable::entriesInsideVolume(std::uint32_t copy) const noexcept {
    const std::uint64_t start = boot_.fatOffset + copy * boot_.fatBytes;  // < 2^48, cannot overflow
    const std::uint64_t size = volume_.size();
    const std::uint64_t inside = start >= size ? 0 : (size - start) / 4;
    return std::min<std::uint64_t>(inside, static_cast<std::uint64_t>(boot_.lastCluster()) + 1);
}

bool FatTable::readRaw(std::uint32_t copy, std::uint32_t first, std::span<std::uint32_t> out) {
    if (copy >= boot_.fatCount) {
        return false;
    }
    // Entries up to lastCluster are guaranteed (by parseBootSector) to lie
    // inside one FAT copy.
    const std::uint64_t end = static_cast<std::uint64_t>(first) + out.size();
    if (end > static_cast<std::uint64_t>(boot_.lastCluster()) + 1) {
        return false;
    }
    const std::uint64_t offset = boot_.fatOffset + copy * boot_.fatBytes + static_cast<std::uint64_t>(first) * 4;
    std::vector<std::byte> bytes(out.size() * 4);
    if (!volume_.readExact(ByteOffset{offset}, bytes).ok()) {
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = loadLe32(bytes, i * 4);
    }
    return true;
}

FatTable::Page& FatTable::page(std::uint32_t index) {
    if (const auto found = pages_.find(index); found != pages_.end()) {
        lru_.splice(lru_.begin(), lru_, found->second.lru);
        return found->second;
    }
    if (pages_.size() >= maxPages_) {
        pages_.erase(lru_.back());
        lru_.pop_back();
    }

    const std::uint32_t first = index * pageEntries_;
    const std::uint32_t count = std::min<std::uint32_t>(pageEntries_, boot_.lastCluster() + 1 - first);
    Page loaded;
    loaded.entries.resize(count);
    if (!readRaw(boot_.activeFat, first, loaded.entries)) {
        // Narrow the failure down to single FAT sectors, trying every copy.
        const std::uint32_t sectors = (count + sectorEntries_ - 1) / sectorEntries_;
        loaded.sectorReadable.assign(sectors, false);
        for (std::uint32_t s = 0; s < sectors; ++s) {
            const std::uint32_t offset = s * sectorEntries_;
            const std::span<std::uint32_t> piece =
                std::span<std::uint32_t>(loaded.entries).subspan(offset, std::min(sectorEntries_, count - offset));
            for (std::uint32_t attempt = 0; attempt < boot_.fatCount; ++attempt) {
                const std::uint32_t copy = (boot_.activeFat + attempt) % boot_.fatCount;
                if (static_cast<std::uint64_t>(first) + offset + piece.size() > entriesInsideVolume(copy)) {
                    continue;  // this part of the copy lies beyond the end of the volume
                }
                if (readRaw(copy, first + offset, piece)) {
                    loaded.sectorReadable[s] = true;
                    if (copy != boot_.activeFat) {
                        ++fallbackReads_;
                    }
                    break;
                }
            }
        }
    }

    lru_.push_front(index);
    loaded.lru = lru_.begin();
    return pages_.emplace(index, std::move(loaded)).first->second;
}

FatEntry FatTable::entry(std::uint32_t cluster) {
    if (cluster > boot_.lastCluster()) {
        return FatEntry{FatEntryKind::OutOfRange, 0};
    }
    const Page& p = page(cluster / pageEntries_);
    const std::uint32_t index = cluster % pageEntries_;
    if (!p.sectorReadable.empty() && !p.sectorReadable[index / sectorEntries_]) {
        return FatEntry{FatEntryKind::Unreadable, 0};
    }
    return classify(p.entries[index]);
}

}  // namespace recovery::filesystem::fat32
