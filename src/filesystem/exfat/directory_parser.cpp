#include "directory_parser.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/unicode.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <string_view>

namespace recovery::filesystem::exfat {

namespace {

// Entry type byte: bit 7 InUse, bit 6 secondary, bit 5 benign, bits 0-4 type code.
constexpr std::uint8_t kEndOfDirectory = 0x00;
constexpr std::uint8_t kInUse = 0x80;
constexpr std::uint8_t kSecondary = 0x40;
constexpr std::uint8_t kBenign = 0x20;
constexpr std::uint8_t kCategoryMask = kInUse | kSecondary;

constexpr std::uint8_t kTypeBitmap = 0x81;
constexpr std::uint8_t kTypeUpcase = 0x82;
constexpr std::uint8_t kTypeLabel = 0x83;
constexpr std::uint8_t kTypeFile = 0x85;
constexpr std::uint8_t kTypeStream = 0xC0;
constexpr std::uint8_t kTypeName = 0xC1;
constexpr std::uint8_t kDeletedFile = kTypeFile & ~kInUse;      // 0x05
constexpr std::uint8_t kDeletedStream = kTypeStream & ~kInUse;  // 0x40
constexpr std::uint8_t kDeletedName = kTypeName & ~kInUse;      // 0x41

// A File entry is followed by a Stream Extension and 1-17 File Name entries.
constexpr std::uint8_t kMinFileSecondaries = 2;
constexpr std::uint8_t kMaxFileSecondaries = 18;
constexpr std::uint8_t kMaxLabelChars = 11;

constexpr std::uint16_t kAttrReadOnly = 0x01;
constexpr std::uint16_t kAttrHidden = 0x02;
constexpr std::uint16_t kAttrSystem = 0x04;
constexpr std::uint16_t kAttrDirectory = 0x10;
constexpr std::uint16_t kAttrArchive = 0x20;
constexpr std::uint16_t kAttrReserved = 0xFFC8;

constexpr std::uint8_t kAllocationPossible = 0x01;
constexpr std::uint8_t kNoFatChain = 0x02;

std::string hex(std::uint8_t value) {
    return std::format("0x{:02X}", value);
}

std::size_t nameEntriesFor(std::uint8_t nameLength) noexcept {
    return (nameLength + kNameCharsPerEntry - 1) / kNameCharsPerEntry;
}

bool isValidName(std::u16string_view name) noexcept {
    if (name.empty() || name == u"." || name == u"..") {
        return false;
    }
    constexpr std::u16string_view kForbidden = u"\"*/:<>?\\|";
    return std::none_of(name.begin(), name.end(), [&](char16_t unit) {
        return unit < 0x20 || kForbidden.find(unit) != std::u16string_view::npos;
    });
}

// Fields shared by complete and orphaned entry sets.
void readStreamExtension(std::span<const std::byte> stream, DirectoryEntry& entry) {
    entry.contiguousData = (loadU8(stream, 1) & kNoFatChain) != 0;
    entry.validDataLength = loadLe64(stream, 8);
    entry.firstCluster = ClusterNumber{loadLe32(stream, 20)};
    entry.size = loadLe64(stream, 24);
    if (*entry.validDataLength > entry.size) {
        entry.issues.push_back(EntryIssue::ValidDataLengthExceedsSize);
    }
}

}  // namespace

std::optional<Timestamp> decodeTimestamp(std::uint32_t raw, std::uint8_t tenMilliseconds, std::uint8_t utcOffset,
                                         bool& invalid) {
    if (raw == 0) {
        return std::nullopt;
    }
    const std::uint32_t date = raw >> 16;
    const std::uint32_t time = raw & 0xFFFF;
    const int year = 1980 + static_cast<int>(date >> 9);
    const unsigned month = (date >> 5) & 0x0F;
    const unsigned day = date & 0x1F;
    const unsigned hour = time >> 11;
    const unsigned minute = (time >> 5) & 0x3F;
    const unsigned seconds = (time & 0x1F) * 2U;
    const std::chrono::year_month_day ymd{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
    if (!ymd.ok() || hour > 23 || minute > 59 || seconds > 58 || tenMilliseconds > 199) {
        invalid = true;
        return std::nullopt;
    }
    Timestamp stamp;
    stamp.time = std::chrono::sys_days{ymd} + std::chrono::hours{hour} + std::chrono::minutes{minute} +
                 std::chrono::seconds{seconds} + std::chrono::milliseconds{tenMilliseconds * 10};
    stamp.local = true;
    if ((utcOffset & 0x80) != 0) {
        // Signed 7-bit count of 15-minute increments; the specification allows -12:00 to +14:00.
        int quarters = utcOffset & 0x7F;
        if (quarters >= 0x40) {
            quarters -= 0x80;
        }
        if (quarters < -48 || quarters > 56) {
            invalid = true;  // keep the local time; the offset is unusable
            return stamp;
        }
        stamp.time -= std::chrono::minutes{quarters * 15};
        stamp.local = false;
    }
    return stamp;
}

bool DirectoryParser::feed(std::span<const std::byte> entry, std::uint64_t volumeOffset, ParseOutput& out) {
    Slot slot;
    std::copy_n(entry.begin(), std::min(entry.size(), kDirectoryEntrySize), slot.bytes.begin());
    slot.offset = volumeOffset;
    if (slot.type() == kEndOfDirectory) {
        interrupt(out);  // all later entries are unused
        return false;
    }
    process(slot, out);
    return true;
}

void DirectoryParser::interrupt(ParseOutput& out) {
    // Bounded: every abandon() consumes the first entry of the pending set.
    while (pending_ != Pending::None) {
        abandon(out);
    }
}

bool DirectoryParser::accepts(std::uint8_t type) const noexcept {
    switch (pending_) {
    case Pending::File:
    case Pending::Skip:
        return (type & kCategoryMask) == (kInUse | kSecondary);
    case Pending::DeletedFile:
    case Pending::Orphan:
        return (type & kCategoryMask) == kSecondary;
    case Pending::None:
        return false;
    }
    return false;
}

void DirectoryParser::process(const Slot& slot, ParseOutput& out) {
    while (pending_ != Pending::None && !accepts(slot.type())) {
        abandon(out);
    }
    if (pending_ == Pending::None) {
        start(slot, out);
        return;
    }
    slots_.push_back(slot);
    if (slots_.size() == expected_) {
        complete(out);
    }
}

void DirectoryParser::start(const Slot& slot, ParseOutput& out) {
    const std::uint8_t type = slot.type();
    const std::span<const std::byte> e(slot.bytes);
    const auto begin = [&](Pending kind, std::size_t entries) {
        pending_ = kind;
        expected_ = entries;
        slots_.assign(1, slot);
    };

    if ((type & kInUse) == 0) {
        // Unused entries. Only the parts of deleted files are of interest.
        if (type == kDeletedFile) {
            const std::uint8_t secondaries = loadU8(e, 1);
            if (secondaries >= kMinFileSecondaries && secondaries <= kMaxFileSecondaries) {
                begin(Pending::DeletedFile, secondaries + 1U);
            }
        } else if (type == kDeletedStream) {
            const std::uint8_t nameLength = loadU8(e, 3);
            if ((loadU8(e, 1) & kAllocationPossible) != 0 && nameLength > 0) {
                begin(Pending::Orphan, 1 + nameEntriesFor(nameLength));
            }
        }
        return;
    }
    if ((type & kSecondary) != 0) {
        out.problems.push_back(
            std::format("secondary entry {} at offset {} has no primary entry", hex(type), slot.offset));
        return;
    }

    switch (type) {
    case kTypeBitmap:
        out.bitmaps.push_back(BitmapEntryInfo{loadU8(e, 1), loadLe32(e, 20), loadLe64(e, 24)});
        return;
    case kTypeUpcase:
        out.upcaseTables.push_back(UpcaseEntryInfo{loadLe32(e, 4), loadLe32(e, 20), loadLe64(e, 24)});
        return;
    case kTypeLabel: {
        const std::uint8_t length = loadU8(e, 1);
        if (length > kMaxLabelChars) {
            out.problems.push_back(std::format("volume label at offset {} has invalid length {}", slot.offset, length));
        } else {
            out.label = utf16ToUtf8(loadUtf16Le(e.subspan(2), length));
        }
        return;
    }
    case kTypeFile: {
        const std::uint8_t secondaries = loadU8(e, 1);
        if (secondaries < kMinFileSecondaries || secondaries > kMaxFileSecondaries) {
            out.problems.push_back(std::format("file entry at offset {} has invalid secondary count {}", slot.offset,
                                               secondaries));
            return;
        }
        begin(Pending::File, secondaries + 1U);
        return;
    }
    default:
        break;
    }
    if ((type & kBenign) != 0) {
        // Benign primaries (volume GUID, TexFAT padding, ...) carry nothing needed here.
        if (const std::uint8_t secondaries = loadU8(e, 1); secondaries > 0) {
            begin(Pending::Skip, secondaries + 1U);
        }
        return;
    }
    out.problems.push_back(std::format("unknown critical entry type {} at offset {}", hex(type), slot.offset));
}

void DirectoryParser::complete(ParseOutput& out) {
    const Pending kind = pending_;
    const std::vector<Slot> slots = std::move(slots_);
    slots_.clear();
    pending_ = Pending::None;
    expected_ = 0;
    switch (kind) {
    case Pending::File:
        if (const std::string reason = buildFileEntry(slots, false, true, out); !reason.empty()) {
            out.problems.push_back(std::format("invalid entry set at offset {}: {}", slots.front().offset, reason));
        }
        break;
    case Pending::DeletedFile:
        if (!buildFileEntry(slots, true, true, out).empty()) {
            salvage(slots, out);
        }
        break;
    case Pending::Orphan:
        if (!buildOrphanEntry(slots, out)) {
            salvage(slots, out);
        }
        break;
    case Pending::Skip:
    case Pending::None:
        break;
    }
}

void DirectoryParser::abandon(ParseOutput& out) {
    const Pending kind = pending_;
    const std::vector<Slot> slots = std::move(slots_);
    slots_.clear();
    pending_ = Pending::None;
    expected_ = 0;
    switch (kind) {
    case Pending::File:
        // Cut short (a wrong secondary count, or a gap): still listed, with a
        // checksum mismatch, if the stream and name entries are all there.
        if (const std::string reason = buildFileEntry(slots, false, false, out); !reason.empty()) {
            out.problems.push_back(std::format("incomplete entry set at offset {}: {}", slots.front().offset, reason));
        }
        break;
    case Pending::DeletedFile:
    case Pending::Orphan:
        salvage(slots, out);
        break;
    case Pending::Skip:
    case Pending::None:
        break;
    }
}

void DirectoryParser::salvage(const std::vector<Slot>& slots, ParseOutput& out) {
    // Every slot after the first is a deleted secondary entry, so this can
    // only start an orphan set; recursion is bounded by the set size.
    for (std::size_t i = 1; i < slots.size(); ++i) {
        process(slots[i], out);
    }
}

void DirectoryParser::checkName(const std::u16string& units, std::uint16_t storedHash, DirectoryEntry& entry) const {
    entry.name = utf16ToUtf8(units);
    if (!isValidName(units)) {
        entry.issues.push_back(EntryIssue::InvalidName);
    }
    if (nameHash(units, upcase_) != storedHash) {
        // With the fallback table, names outside Latin-1 cannot be checked.
        const bool checkable =
            std::all_of(units.begin(), units.end(), [this](char16_t unit) { return upcase_.covers(unit); });
        entry.issues.push_back(checkable ? EntryIssue::NameHashMismatch : EntryIssue::LongNameUnverified);
    }
}

std::string DirectoryParser::buildFileEntry(const std::vector<Slot>& slots, bool deleted, bool complete,
                                            ParseOutput& out) const {
    // Types as they were while the set was in use.
    const auto typeOf = [&](std::size_t i) {
        return static_cast<std::uint8_t>(slots[i].type() | (deleted ? kInUse : 0));
    };
    if (slots.size() < 2 || typeOf(1) != kTypeStream) {
        return "no stream extension entry";
    }
    const std::span<const std::byte> file(slots[0].bytes);
    const std::span<const std::byte> stream(slots[1].bytes);
    const std::uint8_t nameLength = loadU8(stream, 3);
    if (nameLength == 0) {
        return "name length is zero";
    }
    const std::size_t nameEntries = nameEntriesFor(nameLength);
    if (slots.size() < 2 + nameEntries) {
        return "too few entries for a " + std::to_string(nameLength) + "-character name";
    }
    for (std::size_t i = 2; i < 2 + nameEntries; ++i) {
        if (typeOf(i) != kTypeName) {
            return "file name entry missing";
        }
    }
    for (std::size_t i = 2 + nameEntries; i < slots.size(); ++i) {
        if ((typeOf(i) & kBenign) == 0) {
            return "unknown critical secondary entry " + hex(typeOf(i));
        }
    }

    std::vector<std::byte> set(slots.size() * kDirectoryEntrySize);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        std::copy(slots[i].bytes.begin(), slots[i].bytes.end(),
                  set.begin() + static_cast<std::ptrdiff_t>(i * kDirectoryEntrySize));
        set[i * kDirectoryEntrySize] = static_cast<std::byte>(typeOf(i));
    }
    const bool checksumMatches = complete && entrySetChecksum(set) == loadLe16(file, 2);
    if (deleted && !checksumMatches) {
        return "entry set checksum mismatch";
    }

    DirectoryEntry entry;
    entry.state = deleted ? EntryState::Deleted : EntryState::Active;
    const std::uint16_t attributes = loadLe16(file, 4);
    entry.isDirectory = (attributes & kAttrDirectory) != 0;
    entry.attributes = {(attributes & kAttrReadOnly) != 0, (attributes & kAttrHidden) != 0,
                        (attributes & kAttrSystem) != 0, (attributes & kAttrArchive) != 0};
    if ((attributes & kAttrReserved) != 0) {
        entry.issues.push_back(EntryIssue::ReservedAttributeBits);
    }
    bool invalidTime = false;
    entry.created = decodeTimestamp(loadLe32(file, 8), loadU8(file, 20), loadU8(file, 22), invalidTime);
    entry.modified = decodeTimestamp(loadLe32(file, 12), loadU8(file, 21), loadU8(file, 23), invalidTime);
    entry.accessed = decodeTimestamp(loadLe32(file, 16), 0, loadU8(file, 24), invalidTime);
    if (invalidTime) {
        entry.issues.push_back(EntryIssue::InvalidTimestamp);
    }
    readStreamExtension(stream, entry);

    std::u16string units;
    for (std::size_t i = 0; i < nameEntries; ++i) {
        units += loadUtf16Le(std::span<const std::byte>(slots[2 + i].bytes).subspan(2), kNameCharsPerEntry);
    }
    units.resize(nameLength);
    checkName(units, loadLe16(stream, 4), entry);
    if (!checksumMatches) {
        entry.issues.push_back(EntryIssue::EntrySetChecksumMismatch);
    }
    entry.metadataOffset = slots[0].offset;
    out.entries.push_back(std::move(entry));
    return {};
}

bool DirectoryParser::buildOrphanEntry(const std::vector<Slot>& slots, ParseOutput& out) const {
    const std::span<const std::byte> stream(slots[0].bytes);
    const std::uint8_t nameLength = loadU8(stream, 3);
    std::u16string units;
    for (std::size_t i = 1; i < slots.size(); ++i) {
        if (slots[i].type() != kDeletedName) {
            return false;
        }
        units += loadUtf16Le(std::span<const std::byte>(slots[i].bytes).subspan(2), kNameCharsPerEntry);
    }
    units.resize(nameLength);

    // With no File entry and no checksum, the name hash is the only evidence
    // that these entries belong together.
    DirectoryEntry entry;
    entry.state = EntryState::Deleted;
    readStreamExtension(stream, entry);
    const std::uint64_t first = entry.firstCluster.value();
    if (entry.hasIssue(EntryIssue::ValidDataLengthExceedsSize) || (first != 0 && !boot_.isValidCluster(first)) ||
        (entry.size > 0 && first == 0)) {
        return false;
    }
    checkName(units, loadLe16(stream, 4), entry);
    if (entry.hasIssue(EntryIssue::NameHashMismatch)) {
        return false;
    }
    entry.issues.push_back(EntryIssue::MetadataIncomplete);
    entry.metadataOffset = slots[0].offset;
    out.entries.push_back(std::move(entry));
    return true;
}

}  // namespace recovery::filesystem::exfat
