#include "directory_parser.hpp"

#include "filesystem/fat32/fat32_names.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/unicode.hpp"

#include <algorithm>
#include <chrono>

namespace recovery::filesystem::fat32 {

namespace {

constexpr std::uint8_t kDeletedMarker = 0xE5;
constexpr std::uint8_t kAttrReadOnly = 0x01;
constexpr std::uint8_t kAttrHidden = 0x02;
constexpr std::uint8_t kAttrSystem = 0x04;
constexpr std::uint8_t kAttrVolumeId = 0x08;
constexpr std::uint8_t kAttrDirectory = 0x10;
constexpr std::uint8_t kAttrArchive = 0x20;
constexpr std::uint8_t kAttrLongName = 0x0F;
constexpr std::uint8_t kLastLongEntry = 0x40;
constexpr std::size_t kMaxLongNameParts = 20;  // 255 UTF-16 units / 13

// Offsets of the 13 UTF-16 units in a long-name slot.
constexpr std::array<std::size_t, 13> kLongNameUnitOffsets = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

std::u16string unitsToName(const std::vector<std::array<char16_t, 13>>& parts) {
    std::u16string name;
    for (const auto& part : parts) {
        for (const char16_t unit : part) {
            if (unit == 0x0000) {
                return name;
            }
            name += unit;
        }
    }
    // No terminator: the name fills the last slot exactly. Strip any 0xFFFF padding defensively.
    while (!name.empty() && name.back() == 0xFFFF) {
        name.pop_back();
    }
    return name;
}

bool isDotEntry(std::span<const std::byte> e) {
    const auto at = [&](std::size_t i) { return static_cast<char>(e[i]); };
    if (at(0) != '.') {
        return false;
    }
    const std::size_t dots = at(1) == '.' ? 2 : 1;
    for (std::size_t i = dots; i < 11; ++i) {
        if (at(i) != ' ') {
            return false;
        }
    }
    return true;
}

// Characters Windows may have used as the first 8.3 character for a long
// name, in order of likelihood.
std::vector<std::uint8_t> firstCharacterCandidates(const std::u16string& longName) {
    std::vector<std::uint8_t> candidates;
    for (const char16_t unit : longName) {
        if (unit == u'.' || unit == u' ') {
            continue;  // the basis-name algorithm strips leading dots and spaces
        }
        if (unit < 0x80) {
            auto c = static_cast<std::uint8_t>(unit);
            if (c >= 'a' && c <= 'z') {
                c = static_cast<std::uint8_t>(c - 'a' + 'A');
            }
            if (isValidShortNameByte(c) && c != ' ') {
                candidates.push_back(c);
            }
        }
        break;
    }
    candidates.push_back('_');  // replacement for characters not representable in 8.3
    return candidates;
}

}  // namespace

std::optional<Timestamp> decodeTimestamp(std::uint16_t date, std::uint16_t time, std::uint8_t hundredths,
                                         bool& invalid) {
    if (date == 0) {
        return std::nullopt;
    }
    const int year = 1980 + (date >> 9);
    const unsigned month = (date >> 5) & 0x0F;
    const unsigned day = date & 0x1F;
    const unsigned hour = time >> 11;
    const unsigned minute = (time >> 5) & 0x3F;
    const unsigned seconds = (time & 0x1F) * 2U;
    const std::chrono::year_month_day ymd{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
    if (!ymd.ok() || hour > 23 || minute > 59 || seconds > 58 || hundredths > 199) {
        invalid = true;
        return std::nullopt;
    }
    Timestamp stamp;
    stamp.time = std::chrono::sys_days{ymd} + std::chrono::hours{hour} + std::chrono::minutes{minute} +
                 std::chrono::seconds{seconds} + std::chrono::milliseconds{hundredths * 10};
    stamp.local = true;
    return stamp;
}

std::optional<std::u16string> DirectoryParser::takeActiveLongName(std::uint8_t checksum, bool& mismatch) {
    std::vector<LongNamePart> parts;
    parts.swap(pending_);
    if (parts.empty() || pendingDeleted_) {
        return std::nullopt;
    }
    // On disk: ordinal N (with the last flag), N-1, ..., 1.
    const std::size_t count = parts.size();
    if (!parts.front().last || parts.front().ordinal != count) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (parts[i].ordinal != count - i || (i > 0 && parts[i].last)) {
            return std::nullopt;
        }
        if (parts[i].checksum != checksum) {
            mismatch = true;
            return std::nullopt;
        }
    }
    std::vector<std::array<char16_t, 13>> ordered;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        ordered.push_back(it->units);
    }
    return unitsToName(ordered);
}

std::optional<std::u16string> DirectoryParser::takeDeletedLongName(std::optional<std::uint8_t>& checksum) {
    std::vector<LongNamePart> parts;
    parts.swap(pending_);
    if (parts.empty() || !pendingDeleted_) {
        return std::nullopt;
    }
    // Only the slots nearest the 8.3 entry sharing its checksum belong to it.
    const std::uint8_t sum = parts.back().checksum;
    std::size_t firstOwned = parts.size();
    while (firstOwned > 0 && parts[firstOwned - 1].checksum == sum && parts.size() - firstOwned < kMaxLongNameParts) {
        --firstOwned;
    }
    std::vector<std::array<char16_t, 13>> ordered;
    for (std::size_t i = parts.size(); i-- > firstOwned;) {
        ordered.push_back(parts[i].units);  // nearest slot holds the first 13 characters
    }
    checksum = sum;
    return unitsToName(ordered);
}

bool DirectoryParser::feed(std::span<const std::byte> e, std::uint64_t volumeOffset, std::vector<DirectoryEntry>& out) {
    const std::uint8_t first = loadU8(e, 0);
    if (first == 0x00) {
        return false;  // end of directory: all later slots are unused
    }
    const bool deleted = first == kDeletedMarker;
    const std::uint8_t attributes = loadU8(e, 11);

    if ((attributes & 0x3F) == kAttrLongName) {
        if (deleted != pendingDeleted_ || pending_.size() >= kMaxLongNameParts) {
            pending_.clear();
        }
        pendingDeleted_ = deleted;
        LongNamePart part;
        part.ordinal = deleted ? 0 : static_cast<std::uint8_t>(first & 0x1F);
        part.last = !deleted && (first & kLastLongEntry) != 0;
        part.checksum = loadU8(e, 13);
        for (std::size_t i = 0; i < 13; ++i) {
            part.units[i] = static_cast<char16_t>(loadLe16(e, kLongNameUnitOffsets[i]));
        }
        if (!deleted && part.last) {
            pending_.clear();  // a new active long name starts here
        }
        pending_.push_back(part);
        return true;
    }

    if (isDotEntry(e)) {
        pending_.clear();
        return true;
    }
    if ((attributes & kAttrVolumeId) != 0 && (attributes & kAttrDirectory) == 0) {
        if (!deleted) {
            // Labels are 11 plain characters, not a name plus extension.
            std::string label;
            for (std::size_t i = 0; i < 11; ++i) {
                label += oemToUtf8(loadU8(e, i));
            }
            while (!label.empty() && label.back() == ' ') {
                label.pop_back();
            }
            volumeLabel_ = std::move(label);
        }
        pending_.clear();
        return true;
    }

    DirectoryEntry entry;
    entry.state = deleted ? EntryState::Deleted : EntryState::Active;
    entry.isDirectory = (attributes & kAttrDirectory) != 0;
    entry.attributes = {(attributes & kAttrReadOnly) != 0, (attributes & kAttrHidden) != 0,
                        (attributes & kAttrSystem) != 0, (attributes & kAttrArchive) != 0};
    entry.size = loadLe32(e, 28);
    entry.firstCluster = ClusterNumber{(static_cast<std::uint64_t>(loadLe16(e, 20)) << 16) | loadLe16(e, 26)};
    entry.metadataOffset = volumeOffset;

    // Name bytes. A deleted entry's first byte is gone.
    std::array<std::byte, 11> name{};
    std::copy_n(e.begin(), 11, name.begin());
    bool nameValid = true;
    for (std::size_t i = deleted ? 1 : 0; i < 11; ++i) {
        const auto c = static_cast<std::uint8_t>(name[i]);
        const bool valid = isValidShortNameByte(c) || (i == 0 && c == 0x05);
        if (!valid || (i == 0 && c == ' ')) {
            nameValid = false;
        }
    }

    if (deleted) {
        // Reject slots that are clearly not a former file entry (garbage).
        if (!nameValid || (attributes & 0xC0) != 0 ||
            (entry.firstCluster.value() != 0 && !boot_.isValidCluster(entry.firstCluster.value()))) {
            pending_.clear();
            return true;
        }
    } else {
        if (!nameValid) {
            entry.issues.push_back(EntryIssue::InvalidShortName);
        }
        if ((attributes & 0xC0) != 0) {
            entry.issues.push_back(EntryIssue::ReservedAttributeBits);
        }
    }
    if (entry.isDirectory && entry.size != 0) {
        entry.issues.push_back(EntryIssue::DirectoryWithSize);
    }

    const std::uint8_t ntFlags = loadU8(e, 12);
    std::optional<std::u16string> longName;
    if (deleted) {
        std::optional<std::uint8_t> checksum;
        longName = takeDeletedLongName(checksum);
        bool verified = false;
        if (longName.has_value() && checksum.has_value()) {
            for (const std::uint8_t candidate : firstCharacterCandidates(*longName)) {
                name[0] = static_cast<std::byte>(candidate);
                if (longNameChecksum(name) == *checksum) {
                    verified = true;
                    break;
                }
            }
            if (!verified) {
                entry.issues.push_back(EntryIssue::LongNameUnverified);
            }
        }
        if (!verified) {
            name[0] = std::byte{'_'};
            entry.issues.push_back(EntryIssue::NameReconstructed);
        }
    } else {
        bool mismatch = false;
        longName = takeActiveLongName(longNameChecksum(name), mismatch);
        if (mismatch) {
            entry.issues.push_back(EntryIssue::LongNameChecksumMismatch);
        }
    }

    entry.shortName = formatShortName(name, ntFlags);
    entry.name = longName.has_value() && !longName->empty() ? utf16ToUtf8(*longName) : entry.shortName;

    bool invalidTime = false;
    entry.created = decodeTimestamp(loadLe16(e, 16), loadLe16(e, 14), loadU8(e, 13), invalidTime);
    entry.modified = decodeTimestamp(loadLe16(e, 24), loadLe16(e, 22), 0, invalidTime);
    entry.accessed = decodeTimestamp(loadLe16(e, 18), 0, 0, invalidTime);
    if (invalidTime) {
        entry.issues.push_back(EntryIssue::InvalidTimestamp);
    }

    out.push_back(std::move(entry));
    return true;
}

}  // namespace recovery::filesystem::fat32
