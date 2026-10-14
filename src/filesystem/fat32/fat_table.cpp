#include "fat_table.hpp"

#include "recovery/byte_order.hpp"

#include <algorithm>

namespace recovery::filesystem::fat32 {

namespace {
constexpr std::uint32_t kEntryMask = 0x0FFFFFFF;
constexpr std::uint32_t kBadCluster = 0x0FFFFFF7;
constexpr std::uint32_t kEndOfChainMin = 0x0FFFFFF8;
// 64 KiB of FAT per page (FAT16: 32 KiB, FAT12: 24 KiB). Every page's bytes
// start at a multiple of 4 KiB, so at a sector boundary.
constexpr std::uint32_t kDefaultPageEntries = 16 * 1024;
}  // namespace

FatTable::FatTable(storage::IStorageSource& volume, const BootSector& boot, std::size_t cacheBytes)
    : volume_(volume),
      boot_(boot),
      pageEntries_(kDefaultPageEntries),
      maxPages_(std::max<std::size_t>(2, cacheBytes / (kDefaultPageEntries * sizeof(std::uint32_t)))) {}

namespace {

// A FAT16 or FAT12 entry as the FAT32 value of the same meaning: free, next
// cluster, bad (0xFFF7, 0xFF7) and end of chain (0xFFF8, 0xFF8 and above).
std::uint32_t widen(std::uint32_t raw, std::uint32_t bad) noexcept {
    if (raw > bad) {
        return kEndOfChainMin;
    }
    if (raw == bad) {
        return kBadCluster;
    }
    return raw;
}

}  // namespace

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

std::uint64_t FatTable::copyOffset(std::uint32_t copy) const noexcept {
    return boot_.fatOffset + copy * boot_.fatBytes;  // < 2^48, cannot overflow
}

std::pair<std::uint64_t, std::uint64_t> FatTable::byteRange(std::uint64_t first, std::uint64_t count) const noexcept {
    const std::uint64_t bits = boot_.fatEntryBits();
    return {first * bits / 8, ((first + count) * bits + 7) / 8};
}

std::uint64_t FatTable::entriesInsideVolume(std::uint32_t copy) const noexcept {
    const std::uint64_t start = copyOffset(copy);
    const std::uint64_t size = volume_.size();
    const std::uint64_t bytes = start >= size ? 0 : size - start;
    // Entries whose bytes all lie in the first `bytes` bytes of the copy.
    const std::uint64_t inside = boot_.type == FatType::Fat12 ? bytes / 3 * 2 + (bytes % 3 == 2 ? 1 : 0)
                                                               : bytes / (boot_.fatEntryBits() / 8);
    return std::min<std::uint64_t>(inside, static_cast<std::uint64_t>(boot_.lastCluster()) + 1);
}

void FatTable::decode(std::span<const std::byte> bytes, std::uint32_t first, std::span<std::uint32_t> out) const {
    switch (boot_.type) {
    case FatType::Fat32:
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = loadLe32(bytes, i * 4);
        }
        return;
    case FatType::Fat16:
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = widen(loadLe16(bytes, i * 2), 0xFFF7);
        }
        return;
    case FatType::Fat12: {
        // Two entries in three bytes: an even entry is the low 12 bits of the
        // 16 at its first byte, an odd one the high 12.
        const std::uint64_t base = byteRange(first, 0).first;
        for (std::size_t i = 0; i < out.size(); ++i) {
            const std::uint64_t cluster = static_cast<std::uint64_t>(first) + i;
            const std::uint16_t pair = loadLe16(bytes, static_cast<std::size_t>(cluster * 3 / 2 - base));
            const auto raw = static_cast<std::uint32_t>((cluster & 1) != 0 ? pair >> 4 : pair & 0x0FFF);
            out[i] = widen(raw, 0xFF7);
        }
        return;
    }
    }
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
    if (out.empty()) {
        return true;
    }
    const auto [begin, stop] = byteRange(first, out.size());
    std::vector<std::byte> bytes(static_cast<std::size_t>(stop - begin));
    if (!volume_.readExact(ByteOffset{copyOffset(copy) + begin}, bytes).ok()) {
        return false;
    }
    decode(bytes, first, out);
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
        // Put the page's bytes together sector by sector, each from the first
        // copy that can supply it. The page starts at a sector boundary and a
        // FAT copy is whole sectors, so whole sectors are read.
        const auto [begin, end] = byteRange(first, count);
        const std::uint32_t bps = boot_.bytesPerSector;
        const auto sectors = static_cast<std::uint32_t>((end - begin + bps - 1) / bps);
        std::vector<std::byte> bytes(static_cast<std::size_t>(sectors) * bps);
        loaded.sectorReadable.assign(sectors, false);
        for (std::uint32_t s = 0; s < sectors; ++s) {
            const std::span<std::byte> sector =
                std::span<std::byte>(bytes).subspan(static_cast<std::size_t>(s) * bps, bps);
            for (std::uint32_t attempt = 0; attempt < boot_.fatCount; ++attempt) {
                const std::uint32_t copy = (boot_.activeFat + attempt) % boot_.fatCount;
                const std::uint64_t offset = copyOffset(copy) + begin + static_cast<std::uint64_t>(s) * bps;
                if (offset + bps > volume_.size()) {
                    continue;  // this part of the copy lies beyond the end of the volume
                }
                if (volume_.readExact(ByteOffset{offset}, sector).ok()) {
                    loaded.sectorReadable[s] = true;
                    if (copy != boot_.activeFat) {
                        ++fallbackReads_;
                    }
                    break;
                }
            }
        }
        decode(bytes, first, loaded.entries);
    }

    lru_.push_front(index);
    loaded.lru = lru_.begin();
    return pages_.emplace(index, std::move(loaded)).first->second;
}

FatEntry FatTable::entry(std::uint32_t cluster) {
    if (cluster > boot_.lastCluster()) {
        return FatEntry{FatEntryKind::OutOfRange, 0};
    }
    const std::uint32_t index = cluster / pageEntries_;
    const Page& p = page(index);
    if (!p.sectorReadable.empty()) {
        // Every sector the entry's bytes lie in must have been read.
        const std::uint64_t pageBegin = byteRange(static_cast<std::uint64_t>(index) * pageEntries_, 0).first;
        const auto [begin, end] = byteRange(cluster, 1);
        const std::uint32_t bps = boot_.bytesPerSector;
        for (std::uint64_t s = (begin - pageBegin) / bps; s <= (end - 1 - pageBegin) / bps; ++s) {
            if (!p.sectorReadable[static_cast<std::size_t>(s)]) {
                return FatEntry{FatEntryKind::Unreadable, 0};
            }
        }
    }
    return classify(p.entries[cluster % pageEntries_]);
}

}  // namespace recovery::filesystem::fat32
