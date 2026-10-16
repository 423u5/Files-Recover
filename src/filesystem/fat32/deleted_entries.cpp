#include "deleted_entries.hpp"

#include "directory_parser.hpp"
#include "recovery/byte_order.hpp"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace recovery::filesystem::fat32 {

namespace {

// What the cleared top 16 bits of a cluster number are worth.
constexpr std::uint64_t kStep = 0x10000;
constexpr std::uint64_t kLowMask = 0xFFFF;
// Files of a run whose start is checked against their names, at each place.
constexpr std::size_t kMaxCheckedMembers = 4;

std::uint64_t clustersFor(std::uint64_t bytes, std::uint32_t clusterSize) noexcept {
    return bytes == 0 ? 0 : (bytes - 1) / clusterSize + 1;
}

std::uint64_t slotCluster(std::span<const std::byte> slot) {
    return (static_cast<std::uint64_t>(loadLe16(slot, 20)) << 16) | loadLe16(slot, 26);
}

}  // namespace

DeletedEntries::DeletedEntries(storage::IStorageSource& volume, const BootSector& boot, FatTable& fat,
                               StartCheck startsFile)
    : volume_(volume), boot_(boot), fat_(fat), startsFile_(std::move(startsFile)) {}

bool DeletedEntries::isAmbiguous(const DirectoryEntry& entry) const noexcept {
    return entry.state == EntryState::Deleted && (entry.firstCluster.value() >> 16) == 0 &&
           boot_.deletedStartsAmbiguous() && !entry.hasIssue(EntryIssue::StartClusterInferred);
}

std::vector<DeletedEntries::Item> DeletedEntries::itemsOf(const std::vector<DirectoryEntry>& entries) const {
    std::vector<Item> items;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const DirectoryEntry& entry = entries[i];
        if (!entry.isDirectory && entry.size == 0) {
            continue;  // no data, no place
        }
        Item item;
        item.entry = i;
        item.directory = entry.isDirectory;
        item.clusters = entry.isDirectory ? 1 : clustersFor(entry.size, boot_.clusterSize);
        const std::uint64_t recorded = entry.firstCluster.value();
        item.low = recorded & kLowMask;
        item.ambiguous = isAmbiguous(entry);
        if (!item.ambiguous) {
            if (!boot_.isValidCluster(recorded)) {
                continue;  // nothing to place a neighbour by
            }
            item.start = recorded;
        }
        items.push_back(item);
    }
    return items;
}

std::vector<std::uint64_t> DeletedEntries::placesFor(std::uint64_t low, std::uint64_t span) const {
    std::vector<std::uint64_t> places;
    // Bounded: at most 4,096 steps of 65,536 clusters on the largest volume.
    for (std::uint64_t place = low; fits(place, span) || place < 2; place += kStep) {
        if (place >= 2) {
            places.push_back(place);
        }
    }
    return places;
}

bool DeletedEntries::fits(std::uint64_t start, std::uint64_t span) const noexcept {
    return start >= 2 && span >= 1 && span <= boot_.lastCluster() && start <= boot_.lastCluster() - (span - 1);
}

bool DeletedEntries::isFree(std::uint64_t cluster) {
    return boot_.isValidCluster(cluster) && fat_.entry(static_cast<std::uint32_t>(cluster)).kind == FatEntryKind::Free;
}

std::uint64_t DeletedEntries::endOf(const Item& item, const DirectoryEntry& entry) {
    const std::uint64_t start = *item.start;
    if (entry.state == EntryState::Active && !entry.isDirectory) {
        // An active file's chain says where its data ends (bounded by its size).
        std::uint64_t current = start;
        for (std::uint64_t i = 1; i < item.clusters; ++i) {
            const FatEntry next = fat_.entry(static_cast<std::uint32_t>(current));
            if (next.kind != FatEntryKind::Next) {
                return start + item.clusters;
            }
            current = next.value;
        }
        return current + 1;
    }
    return start + item.clusters;
}

bool DeletedEntries::isFolderAt(std::uint64_t cluster, std::uint32_t parent, bool parentIsRoot) {
    std::array<std::byte, 2 * kDirectoryEntrySize> head{};
    if (!volume_.readExact(ByteOffset{boot_.clusterOffset(static_cast<std::uint32_t>(cluster))}, head).ok()) {
        return false;
    }
    const std::span<const std::byte> dot = std::span<const std::byte>(head).first(kDirectoryEntrySize);
    const std::span<const std::byte> dotDot = std::span<const std::byte>(head).subspan(kDirectoryEntrySize);
    if (!isDotSlot(dot) || loadU8(dot, 1) != ' ' || !isDotSlot(dotDot) || loadU8(dotDot, 1) != '.' ||
        slotCluster(dot) != cluster) {
        return false;
    }
    // ".." names the parent; the root as 0 (some writers put its cluster).
    const std::uint64_t up = slotCluster(dotDot);
    return parentIsRoot ? (up == 0 || up == boot_.rootCluster) : up == parent;
}

std::optional<bool> DeletedEntries::startsFileAt(const DirectoryEntry& entry, std::uint64_t cluster) {
    const std::uint64_t offset = boot_.clusterOffset(static_cast<std::uint32_t>(cluster));
    if (offset >= volume_.size()) {
        return std::nullopt;
    }
    std::vector<std::byte> head(static_cast<std::size_t>(std::min<std::uint64_t>(kStartCheckBytes, volume_.size() - offset)));
    if (!volume_.readExact(ByteOffset{offset}, head).ok()) {
        return std::nullopt;
    }
    return startsFile_(entry.name, head);
}

bool DeletedEntries::continuesDirectory(std::uint64_t cluster, const std::vector<std::uint32_t>& clusters) {
    if (!boot_.isValidCluster(cluster) || std::find(clusters.begin(), clusters.end(), cluster) != clusters.end() ||
        !isFree(cluster)) {
        return false;
    }
    std::vector<std::byte> bytes(boot_.clusterSize);
    if (!volume_.readExact(ByteOffset{boot_.clusterOffset(static_cast<std::uint32_t>(cluster))}, bytes).ok()) {
        return false;
    }
    std::size_t used = 0;
    for (std::size_t at = 0; at + kDirectoryEntrySize <= bytes.size(); at += kDirectoryEntrySize) {
        const std::span<const std::byte> slot = std::span<const std::byte>(bytes).subspan(at, kDirectoryEntrySize);
        if (loadU8(slot, 0) == 0x00) {
            break;  // the directory ends here
        }
        if (!isPossibleSlot(slot, boot_)) {
            return false;  // other data, or another folder's first cluster
        }
        ++used;
    }
    return used > 0;
}

void DeletedEntries::placeFolders(std::vector<Item>& items, std::uint32_t directory, bool isRoot) {
    for (Item& item : items) {
        if (!item.ambiguous || !item.directory) {
            continue;
        }
        std::vector<std::uint64_t> found;
        for (const std::uint64_t place : placesFor(item.low, 1)) {
            if (isFree(place) && isFolderAt(place, directory, isRoot)) {
                found.push_back(place);
            }
        }
        if (std::find(found.begin(), found.end(), item.low) != found.end()) {
            item.start = item.low;  // where it was recorded
        } else if (found.size() == 1) {
            item.start = found.front();
        }
    }
}

void DeletedEntries::placeByNeighbours(std::vector<Item>& items, const std::vector<DirectoryEntry>& entries,
                                       std::uint32_t directory, const std::vector<std::uint32_t>& clusters) {
    // The listed directory's own clusters lie between files written while it grew.
    const std::unordered_set<std::uint64_t> folder(clusters.begin(), clusters.end());
    const auto skipForward = [&](std::uint64_t cluster) {
        while (folder.contains(cluster)) {
            ++cluster;
        }
        return cluster;
    };
    const auto skipBackward = [&](std::uint64_t cluster) {
        while (cluster > 0 && folder.contains(cluster - 1)) {
            --cluster;
        }
        return cluster;
    };
    const auto agrees = [&](const Item& item, std::uint64_t place) {
        return (place & kLowMask) == item.low && fits(place, item.clusters);
    };

    // Forward: an entry starts where the one before it ends (the first one,
    // after the directory's first cluster).
    for (std::size_t i = 0; i < items.size(); ++i) {
        Item& item = items[i];
        if (!item.ambiguous || item.start.has_value()) {
            continue;
        }
        std::optional<std::uint64_t> place;
        if (i == 0) {
            place = skipForward(std::uint64_t{directory} + 1);
        } else if (items[i - 1].start.has_value()) {
            place = skipForward(endOf(items[i - 1], entries[items[i - 1].entry]));
        }
        if (place.has_value() && agrees(item, *place)) {
            item.start = place;
        }
    }
    // Backward: an entry ends where the one after it starts.
    for (std::size_t i = items.size(); i-- > 0;) {
        Item& item = items[i];
        if (!item.ambiguous || item.start.has_value() || i + 1 == items.size() || !items[i + 1].start.has_value()) {
            continue;
        }
        const std::uint64_t end = skipBackward(*items[i + 1].start);
        if (end > item.clusters && agrees(item, end - item.clusters)) {
            item.start = end - item.clusters;
        }
    }
}

void DeletedEntries::placeRuns(std::vector<Item>& items, const std::vector<DirectoryEntry>& entries) {
    const auto unplacedFile = [](const Item& item) {
        return item.ambiguous && !item.start.has_value() && !item.directory;
    };
    for (std::size_t first = 0; first < items.size();) {
        if (!unplacedFile(items[first])) {
            ++first;
            continue;
        }
        // The files after it whose low bits say each follows the one before.
        std::size_t end = first + 1;
        while (end < items.size() && unplacedFile(items[end]) &&
               items[end].low == ((items[end - 1].low + items[end - 1].clusters) & kLowMask)) {
            ++end;
        }
        placeRun(items, entries, first, end);
        first = end;
    }
}

void DeletedEntries::placeRun(std::vector<Item>& items, const std::vector<DirectoryEntry>& entries, std::size_t first,
                              std::size_t end) {
    std::vector<std::uint64_t> offsets{0};
    for (std::size_t i = first + 1; i < end; ++i) {
        offsets.push_back(offsets.back() + items[i - 1].clusters);
    }
    const std::uint64_t span = offsets.back() + items[end - 1].clusters;
    // Members whose names say what starts them (an empty head asks only that).
    std::vector<std::size_t> typed;
    for (std::size_t i = first; i < end; ++i) {
        if (startsFile_(entries[items[i].entry].name, {}).has_value()) {
            typed.push_back(i);
        }
    }
    if (typed.empty()) {
        return;  // nothing tells the places apart
    }

    std::vector<std::uint64_t> supported;
    for (const std::uint64_t place : placesFor(items[first].low, span)) {
        // A deleted run's clusters are free unless other files took them since.
        std::size_t freeStarts = 0;
        for (std::size_t i = first; i < end; ++i) {
            freeStarts += isFree(place + offsets[i - first]) ? 1 : 0;
        }
        if (2 * freeStarts < end - first) {
            continue;
        }
        std::size_t matches = 0;
        bool mismatch = false;
        for (const std::size_t i : typed) {
            if (matches == kMaxCheckedMembers) {
                break;
            }
            const std::uint64_t start = place + offsets[i - first];
            if (!isFree(start)) {
                continue;  // overwritten since: says nothing
            }
            const std::optional<bool> starts = startsFileAt(entries[items[i].entry], start);
            if (starts.has_value() && !*starts) {
                mismatch = true;
                break;
            }
            matches += starts.value_or(false) ? 1 : 0;
        }
        if (!mismatch && matches > 0) {
            supported.push_back(place);
        }
    }
    // Where it was recorded (the lowest place), when the evidence agrees; else
    // the one place it singles out.
    if (!supported.empty() && (supported.front() == items[first].low || supported.size() == 1)) {
        for (std::size_t i = first; i < end; ++i) {
            items[i].start = supported.front() + offsets[i - first];
        }
    }
}

void DeletedEntries::inferStarts(std::vector<DirectoryEntry>& entries, std::uint32_t directory, bool isRoot,
                                 const std::vector<std::uint32_t>& clusters) {
    if (!boot_.deletedStartsAmbiguous()) {
        return;
    }
    std::vector<Item> items = itemsOf(entries);
    if (std::none_of(items.begin(), items.end(), [](const Item& item) { return item.ambiguous; })) {
        return;
    }
    placeFolders(items, directory, isRoot);
    placeByNeighbours(items, entries, directory, clusters);
    if (startsFile_) {
        placeRuns(items, entries);
    }
    for (const Item& item : items) {
        if (!item.ambiguous || !item.start.has_value()) {
            continue;
        }
        DirectoryEntry& entry = entries[item.entry];
        if (*item.start != entry.firstCluster.value()) {
            entry.firstCluster = ClusterNumber{*item.start};
            entry.issues.push_back(EntryIssue::StartClusterInferred);
        }
    }
}

std::optional<std::uint32_t> DeletedEntries::nextCluster(const std::vector<DirectoryEntry>& entries,
                                                         const std::vector<std::uint32_t>& clusters) {
    if (clusters.empty()) {
        return std::nullopt;
    }
    // After the data of the last entry that has some, then right after the
    // directory's previous cluster; after that last entry's possible places
    // when its start lost its top bits, if only one of them qualifies.
    std::vector<std::uint64_t> exact;
    std::vector<std::uint64_t> possible;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (!it->isDirectory && it->size == 0) {
            continue;
        }
        const std::uint64_t span = it->isDirectory ? 1 : clustersFor(it->size, boot_.clusterSize);
        const std::uint64_t recorded = it->firstCluster.value();
        if (!isAmbiguous(*it)) {
            if (boot_.isValidCluster(recorded)) {
                exact.push_back(recorded + span);
            }
        } else {
            for (const std::uint64_t place : placesFor(recorded & kLowMask, span)) {
                possible.push_back(place + span);
            }
        }
        break;
    }
    exact.push_back(std::uint64_t{clusters.back()} + 1);
    for (const std::uint64_t cluster : exact) {
        if (continuesDirectory(cluster, clusters)) {
            return static_cast<std::uint32_t>(cluster);
        }
    }
    std::optional<std::uint32_t> found;
    for (const std::uint64_t cluster : possible) {
        if (continuesDirectory(cluster, clusters)) {
            if (found.has_value()) {
                return std::nullopt;  // two places: no telling which
            }
            found = static_cast<std::uint32_t>(cluster);
        }
    }
    return found;
}

}  // namespace recovery::filesystem::fat32
