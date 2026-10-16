#include "deleted_entries.hpp"

#include "directory_parser.hpp"
#include "recovery/byte_order.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <unordered_set>

namespace recovery::filesystem::fat32 {

namespace {

// What the cleared top 16 bits of a cluster number are worth.
constexpr std::uint64_t kStep = 0x10000;
constexpr std::uint64_t kLowMask = 0xFFFF;
// Files of a run whose start is checked against their names, at each place.
constexpr std::size_t kMaxCheckedMembers = 4;
// Places of a file alone that pass the first checks, beyond which none is
// taken: too many look alike (L242).
constexpr std::size_t kMaxAlternatives = 8;
// Places a file alone may start at, beyond which they are not examined (a
// volume of more than 16.7 million clusters): each costs a read (L242).
constexpr std::size_t kMaxExaminedPlaces = 256;

// Extensions of files that hold text: whether such a file can start at a
// place is told by its first bytes, with no format (L242). Not ".ts" (also
// camcorder video) or ".sub" (also binary subtitles).
constexpr auto kTextExtensions = std::to_array<std::string_view>({
    "txt", "text", "log", "csv",  "tsv", "md",  "markdown", "rst", "xml",  "json", "yaml", "yml", "ini", "cfg",
    "conf", "toml", "properties", "html", "htm", "xhtml", "css", "js", "mjs", "java", "kt", "kts", "gradle", "c",
    "h",   "cc",   "cpp", "cxx",  "hpp", "hh",  "cs",       "py",  "rb",   "go",   "rs",   "php", "pl",  "sh",
    "bat", "cmd",  "ps1", "sql",  "srt", "vtt", "ics",      "vcf", "tex",  "svg",  "rtf",  "nfo", "inf", "reg",
    "m3u", "pls",  "url", "lst",  "diz",
});

std::uint64_t clustersFor(std::uint64_t bytes, std::uint32_t clusterSize) noexcept {
    return bytes == 0 ? 0 : (bytes - 1) / clusterSize + 1;
}

std::uint64_t slotCluster(std::span<const std::byte> slot) {
    return (static_cast<std::uint64_t>(loadLe16(slot, 20)) << 16) | loadLe16(slot, 26);
}

bool isTextName(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 == name.size()) {
        return false;
    }
    std::string extension(name.substr(dot + 1));
    for (char& c : extension) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return std::find(kTextExtensions.begin(), kTextExtensions.end(), extension) != kTextExtensions.end();
}

// Text: a UTF-16 byte order mark first, or no control character but those
// text holds (tab, line breaks, form feed, escape, and the end mark of old
// text files). Other bytes are all taken: text in a code page other than
// UTF-8 is text too.
bool looksLikeText(std::span<const std::byte> head) {
    if (head.size() >= 2) {
        const auto b0 = static_cast<std::uint8_t>(head[0]);
        const auto b1 = static_cast<std::uint8_t>(head[1]);
        if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF)) {
            return true;
        }
    }
    return !head.empty() && std::none_of(head.begin(), head.end(), [](std::byte b) {
        const auto c = static_cast<std::uint8_t>(b);
        return c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0B && c != 0x0C && c != 0x0D && c != 0x1A && c != 0x1B;
    });
}

// Nothing was written there: every byte 0x00, or every byte 0xFF (erased flash).
bool isBlank(std::span<const std::byte> head) {
    return std::all_of(head.begin(), head.end(), [](std::byte b) { return b == std::byte{0x00}; }) ||
           std::all_of(head.begin(), head.end(), [](std::byte b) { return b == std::byte{0xFF}; });
}

}  // namespace

DeletedEntries::DeletedEntries(storage::IStorageSource& volume, const BootSector& boot, FatTable& fat,
                               StartCheck startsFile, EndCheck endsFile)
    : volume_(volume), boot_(boot), fat_(fat), startsFile_(std::move(startsFile)), endsFile_(std::move(endsFile)) {}

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

std::uint64_t DeletedEntries::clustersInUse(std::uint64_t start, std::uint64_t span, std::uint64_t enough) {
    std::uint64_t used = 0;
    for (std::uint64_t cluster = start; cluster < start + span && used < enough; ++cluster) {
        if (used + (start + span - cluster) < enough) {
            break;  // the clusters left cannot reach it
        }
        const FatEntryKind kind = fat_.entry(static_cast<std::uint32_t>(cluster)).kind;
        used += kind == FatEntryKind::Next || kind == FatEntryKind::EndOfChain || kind == FatEntryKind::Bad ? 1 : 0;
    }
    return used;
}

bool DeletedEntries::runIsFree(std::uint64_t start, std::uint64_t span) {
    for (std::uint64_t cluster = start; cluster < start + span; ++cluster) {
        if (!isFree(cluster)) {
            return false;
        }
    }
    return true;
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

std::optional<std::uint64_t> DeletedEntries::folderParentAt(std::uint64_t cluster) {
    if (!boot_.isValidCluster(cluster)) {
        return std::nullopt;
    }
    std::array<std::byte, 2 * kDirectoryEntrySize> head{};
    if (!volume_.readExact(ByteOffset{boot_.clusterOffset(static_cast<std::uint32_t>(cluster))}, head).ok()) {
        return std::nullopt;
    }
    const std::span<const std::byte> dot = std::span<const std::byte>(head).first(kDirectoryEntrySize);
    const std::span<const std::byte> dotDot = std::span<const std::byte>(head).subspan(kDirectoryEntrySize);
    if (!isDotSlot(dot) || loadU8(dot, 1) != ' ' || !isDotSlot(dotDot) || loadU8(dotDot, 1) != '.' ||
        slotCluster(dot) != cluster) {
        return std::nullopt;
    }
    return slotCluster(dotDot);
}

bool DeletedEntries::isFolderAt(std::uint64_t cluster, const Listing& listing) {
    const std::optional<std::uint64_t> up = folderParentAt(cluster);
    if (!up.has_value()) {
        return false;
    }
    if (!listing.parent.has_value()) {
        return *up == 0 || boot_.isValidCluster(*up);  // the folder it was in is unknown
    }
    // ".." names the parent; the root as 0 (some writers put its cluster).
    return listing.isRoot ? (*up == 0 || *up == boot_.rootCluster) : *up == *listing.parent;
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

std::optional<std::vector<std::byte>> DeletedEntries::headAt(std::uint64_t cluster, std::uint64_t size) {
    const std::uint64_t offset = boot_.clusterOffset(static_cast<std::uint32_t>(cluster));
    if (offset >= volume_.size()) {
        return std::nullopt;
    }
    std::vector<std::byte> head(
        static_cast<std::size_t>(std::min<std::uint64_t>({kStartCheckBytes, size, volume_.size() - offset})));
    if (head.empty() || !volume_.readExact(ByteOffset{offset}, head).ok()) {
        return std::nullopt;
    }
    return head;
}

std::optional<bool> DeletedEntries::fitsAt(const DirectoryEntry& entry, std::span<const std::byte> head) const {
    if (startsFile_) {
        if (const std::optional<bool> typed = startsFile_(entry.name, head)) {
            return typed;
        }
    }
    if (isTextName(entry.name)) {
        return looksLikeText(head);
    }
    return std::nullopt;
}

bool DeletedEntries::holdsOtherData(const Item& item, const DirectoryEntry& entry) {
    const std::uint64_t recorded = item.low;
    if (!fits(recorded, item.clusters)) {
        return true;  // no place there: cluster 0 or 1 is what is left of a start further on
    }
    // Most of its clusters hold other files' data.
    const std::uint64_t half = item.clusters / 2;
    if (clustersInUse(recorded, item.clusters, half + 1) > half) {
        return true;
    }
    // What starts there now cannot start it; data written there since tells nothing.
    if (!isFree(recorded)) {
        return false;
    }
    const std::optional<std::vector<std::byte>> head = headAt(recorded, entry.size);
    return head.has_value() && fitsAt(entry, *head) == false;
}

bool DeletedEntries::mayStartAt(const DirectoryEntry& entry, std::uint64_t place) {
    if (!isFree(place)) {
        return false;
    }
    const std::optional<std::vector<std::byte>> head = headAt(place, entry.size);
    return head.has_value() && !isBlank(*head) && fitsAt(entry, *head) != false;
}

bool DeletedEntries::continuesDirectory(std::uint64_t cluster, const std::vector<std::uint32_t>& clusters) {
    if (!boot_.isValidCluster(cluster) || std::find(clusters.begin(), clusters.end(), cluster) != clusters.end() ||
        !isFree(cluster)) {
        return false;
    }
    // Its first sector, then the rest: other data shows in the first one.
    const std::uint64_t offset = boot_.clusterOffset(static_cast<std::uint32_t>(cluster));
    const std::uint32_t head = std::min<std::uint32_t>(boot_.bytesPerSector, boot_.clusterSize);
    std::vector<std::byte> bytes(boot_.clusterSize);
    std::size_t used = 0;
    std::size_t done = 0;
    for (const std::uint32_t length : {head, boot_.clusterSize - head}) {
        const std::span<std::byte> part(bytes.data() + done, length);
        if (length == 0 || !volume_.readExact(ByteOffset{offset + done}, part).ok()) {
            return length == 0 && used > 0;
        }
        for (std::size_t at = done; at + kDirectoryEntrySize <= done + length; at += kDirectoryEntrySize) {
            const std::span<const std::byte> slot = std::span<const std::byte>(bytes).subspan(at, kDirectoryEntrySize);
            if (loadU8(slot, 0) == 0x00) {
                return used > 0;  // the directory ends here
            }
            if (!isPossibleSlot(slot, boot_)) {
                return false;  // other data, or another folder's first cluster
            }
            ++used;
        }
        done += length;
    }
    return used > 0;
}

void DeletedEntries::placeFolders(std::vector<Item>& items, const Listing& listing) {
    for (Item& item : items) {
        if (!item.ambiguous || !item.directory) {
            continue;
        }
        std::vector<std::uint64_t> found;
        for (const std::uint64_t place : placesFor(item.low, 1)) {
            if (isFree(place) && isFolderAt(place, listing)) {
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
                                       const Listing& listing) {
    // The listed directory's own clusters lie between files written while it grew.
    const std::unordered_set<std::uint64_t> folder(listing.clusters->begin(), listing.clusters->end());
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
    // after the cluster that starts the listing).
    for (std::size_t i = 0; i < items.size(); ++i) {
        Item& item = items[i];
        if (!item.ambiguous || item.start.has_value()) {
            continue;
        }
        std::optional<std::uint64_t> place;
        if (i == 0) {
            place = skipForward(std::uint64_t{listing.first} + 1);
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
    // Of several places, the one where the run's first typed file ends at its
    // size, if only one does (L242): its data is there, end to end.
    if (supported.size() > 1 && endsFile_) {
        const std::size_t member = typed.front();
        const DirectoryEntry& entry = entries[items[member].entry];
        std::vector<std::uint64_t> ending;
        for (const std::uint64_t place : supported) {
            const std::uint64_t start = place + offsets[member - first];
            if (isFree(start) &&
                endsFile_(entry.name, boot_.clusterOffset(static_cast<std::uint32_t>(start)), entry.size)) {
                ending.push_back(place);
            }
        }
        if (ending.size() == 1) {
            supported = std::move(ending);
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

void DeletedEntries::placeAlone(std::vector<Item>& items, const std::vector<DirectoryEntry>& entries) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        Item& item = items[i];
        if (!item.ambiguous || item.start.has_value() || item.directory) {
            continue;
        }
        const DirectoryEntry& entry = entries[item.entry];
        if (!holdsOtherData(item, entry)) {
            continue;  // it may well be where it was recorded
        }
        // The places where something that can start it was written: cheap
        // checks first, then the state of every cluster of its run.
        const std::vector<std::uint64_t> possible = placesFor(item.low, item.clusters);
        if (possible.size() > kMaxExaminedPlaces) {
            continue;
        }
        std::vector<std::uint64_t> places;
        for (const std::uint64_t place : possible) {
            if (place != item.low && mayStartAt(entry, place)) {
                places.push_back(place);
                if (places.size() > kMaxAlternatives) {
                    break;
                }
            }
        }
        if (places.size() > kMaxAlternatives) {
            continue;
        }
        std::erase_if(places, [&](std::uint64_t place) { return !runIsFree(place, item.clusters); });
        if (places.size() > 1) {
            // Of several, the one between the files around it in the listing,
            // which were written before and after it.
            std::optional<std::uint64_t> after;
            for (std::size_t j = i; j-- > 0;) {
                if (items[j].start.has_value()) {
                    after = endOf(items[j], entries[items[j].entry]);
                    break;
                }
            }
            std::optional<std::uint64_t> before;
            for (std::size_t j = i + 1; j < items.size(); ++j) {
                if (items[j].start.has_value()) {
                    before = *items[j].start;
                    break;
                }
            }
            if ((after.has_value() || before.has_value()) && (!after || !before || *after < *before)) {
                std::vector<std::uint64_t> between;
                for (const std::uint64_t place : places) {
                    if ((!after || place >= *after) && (!before || place + item.clusters <= *before)) {
                        between.push_back(place);
                    }
                }
                if (between.size() == 1) {
                    places = std::move(between);
                }
            }
        }
        if (places.size() == 1) {
            item.start = places.front();
        }
    }
}

void DeletedEntries::inferStarts(std::vector<DirectoryEntry>& entries, const Listing& listing) {
    if (!boot_.deletedStartsAmbiguous()) {
        return;
    }
    std::vector<Item> items = itemsOf(entries);
    if (std::none_of(items.begin(), items.end(), [](const Item& item) { return item.ambiguous; })) {
        return;
    }
    placeFolders(items, listing);
    placeByNeighbours(items, entries, listing);
    if (startsFile_) {
        placeRuns(items, entries);
    }
    placeAlone(items, entries);
    placeByNeighbours(items, entries, listing);
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
