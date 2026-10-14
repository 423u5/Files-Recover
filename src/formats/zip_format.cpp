#include "formats/zip_format.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"
#include "recovery/crc32.hpp"
#include "structure_walk.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recovery::formats {

namespace {

using carving::EndDetection;
using carving::HeaderCheck;
using carving::IContentReader;
using carving::ValidationResult;
using carving::ValidationStatus;
using detail::Depth;
using detail::Walk;
using detail::WalkStatus;

constexpr std::uint32_t kLocalSignature = 0x04034B50;
constexpr std::uint32_t kCentralSignature = 0x02014B50;
constexpr std::uint32_t kEndSignature = 0x06054B50;
constexpr std::uint32_t kZip64EndSignature = 0x06064B50;
constexpr std::uint32_t kZip64LocatorSignature = 0x07064B50;
constexpr std::uint32_t kDescriptorSignature = 0x08074B50;
constexpr std::uint32_t kDigitalSignature = 0x05054B50;
constexpr std::uint32_t kArchiveExtraSignature = 0x08064B50;

constexpr std::size_t kLocalHeaderSize = 30;
constexpr std::size_t kCentralHeaderSize = 46;
constexpr std::size_t kEndSize = 22;
// The ZIP64 end of central directory record without its extensible data.
constexpr std::size_t kZip64EndSize = 56;
constexpr std::size_t kZip64LocatorSize = 20;
// An APK signing block ends with its size (again) and this magic.
constexpr std::size_t kApkBlockTrailer = 8 + 16;
constexpr std::string_view kApkSigningMagic = "APK Sig Block 42";

constexpr std::uint16_t kZip64ExtraTag = 0x0001;
constexpr std::uint32_t kMarker32 = 0xFFFFFFFF;
constexpr std::uint16_t kMarker16 = 0xFFFF;

// General purpose flags (APPNOTE 4.4.4).
constexpr std::uint16_t kEncrypted = 0x0001;
constexpr std::uint16_t kDataDescriptor = 0x0008;
constexpr std::uint16_t kStrongEncryption = 0x0040;
// Bits 7 to 10 are unused, 14 and 15 reserved: no writer sets them.
constexpr std::uint16_t kUnusedFlags = 0xC780;

constexpr std::uint16_t kStored = 0;
constexpr std::uint16_t kDeflated = 8;
constexpr std::uint16_t kAesEncrypted = 99;

// The highest "version needed to extract" (APPNOTE 6.3).
constexpr std::uint8_t kMaxVersion = 63;

// Data is read in pieces of this size for its CRC-32 and for data descriptors.
constexpr std::size_t kPiece = 64 * kKiB;

// The compression methods of APPNOTE 4.4.5.
bool knownMethod(std::uint16_t method) noexcept {
    switch (method) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 8:
    case 9:
    case 10:
    case 12:
    case 14:
    case 16:
    case 18:
    case 19:
    case 20:
    case 93:
    case 94:
    case 95:
    case 96:
    case 97:
    case 98:
    case 99:
        return true;
    default:
        return false;
    }
}

std::string methodName(std::uint16_t method) {
    switch (method) {
    case 9:
        return "Deflate64";
    case 12:
        return "bzip2";
    case 14:
        return "LZMA";
    case 93:
        return "Zstandard";
    case 95:
        return "XZ";
    case 98:
        return "PPMd";
    case 99:
        return "AES encryption";
    default:
        return "compression method " + std::to_string(method);
    }
}

std::string count(std::uint64_t n, std::string_view one, std::string_view many) {
    return std::to_string(n) + " " + std::string(n == 1 ? one : many);
}

// The data of the ZIP64 extended information extra field among `extra`'s
// fields, if there is one. Fields that do not fit end the search: some
// writers pad the extra fields with zeros (older zipalign).
std::optional<std::span<const std::byte>> zip64Field(std::span<const std::byte> extra) noexcept {
    std::size_t at = 0;
    while (extra.size() - at >= 4) {
        const std::uint16_t tag = loadLe16(extra, at);
        const std::uint16_t size = loadLe16(extra, at + 2);
        if (size > extra.size() - at - 4) {
            return std::nullopt;
        }
        if (tag == kZip64ExtraTag) {
            return extra.subspan(at + 4, size);
        }
        at += 4 + std::size_t{size};
    }
    return std::nullopt;
}

// The values of a ZIP64 extra field, in order: there is one for each header
// field set to its maximum (APPNOTE 4.5.3).
class Zip64Values {
public:
    explicit Zip64Values(std::optional<std::span<const std::byte>> field) noexcept : field_(field) {}

    // The header's value, or the next ZIP64 value when the header holds the
    // marker; nothing when the ZIP64 field ends first.
    std::optional<std::uint64_t> resolve(std::uint64_t header, std::uint64_t marker, std::size_t width) noexcept {
        if (header != marker) {
            return header;
        }
        if (!field_.has_value() || at_ > field_->size() || field_->size() - at_ < width) {
            return std::nullopt;
        }
        const std::uint64_t value = width == 8 ? loadLe64(*field_, at_) : loadLe32(*field_, at_);
        at_ += width;
        return value;
    }

private:
    std::optional<std::span<const std::byte>> field_;
    std::size_t at_ = 0;
};

struct Member {
    // Offset of the local header.
    std::uint64_t offset = 0;
    std::uint64_t compressed = 0;
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
    // The name, for the central directory's entry: its length and CRC-32.
    std::uint32_t nameCrc = 0;
    std::uint16_t nameLength = 0;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    // A central directory entry names it.
    bool listed = false;
};

// Walks an archive from its first local header to the end of its end of
// central directory record. See structure_walk.hpp for what a walk reports.
//
// Depth::Layout follows the recorded sizes, and decodes only the DEFLATE
// data whose size only a data descriptor records. Depth::Full also decodes
// every member's data for its CRC-32 and size, and checks every central
// directory entry against its member.
class ZipWalker {
public:
    ZipWalker(IContentReader& content, Depth depth, const IDeflateDecoder& decoder,
              const ZipFormat::Limits& limits) noexcept
        : content_(content), depth_(depth), decoder_(decoder), limits_(limits) {}

    Result<Walk> run();

    // Depth::Full: the members whose data was not checked, and why the first was not.
    [[nodiscard]] std::uint64_t unchecked() const noexcept { return unchecked_; }
    [[nodiscard]] const std::string& uncheckedReason() const noexcept { return uncheckedReason_; }

private:
    using Next = Result<std::optional<std::uint64_t>>;

    // The member at `at`: where the next record starts, or nothing when the walk ended.
    Next member(std::uint64_t at);
    // The data of a member whose size a data descriptor records, and the
    // descriptor: where the next record starts, or nothing when the walk ended.
    Next describedData(Member& member, std::uint64_t dataOffset, bool encrypted, bool zip64,
                       const std::string& which);
    // The length of data that a data descriptor with its signature follows,
    // or nothing when there is none within the unpack limit.
    Next findDescriptor(std::uint64_t dataOffset, bool zip64);
    // The end of the APK signing block at `at`, or nothing when there is none.
    Next apkSigningBlock(std::uint64_t at);
    Result<Walk> centralDirectory(std::uint64_t start);
    Status matchEntries(std::uint64_t start, std::uint64_t end);

    // Depth::Full: decodes the data of a member whose sizes are known.
    Status checkData(const Member& member, std::uint64_t dataOffset, bool encrypted, const std::string& which);
    void noteDecoded(const IDeflateDecoder::Outcome& decoded, const Member& member, std::uint64_t dataOffset,
                     const std::string& which);
    Result<std::uint32_t> crcOf(std::uint64_t offset, std::uint64_t length);
    void skip(std::string reason);
    // Unpacked bytes left within the limit.
    [[nodiscard]] std::uint64_t budget() const noexcept {
        return limits_.maxUnpacked > unpacked_ ? limits_.maxUnpacked - unpacked_ : 0;
    }

    IContentReader& content_;
    Depth depth_;
    const IDeflateDecoder& decoder_;
    const ZipFormat::Limits& limits_;
    Walk walk_;
    // In the order of their offsets (the order walked).
    std::vector<Member> members_;
    std::uint64_t unpacked_ = 0;
    std::uint64_t unchecked_ = 0;
    std::string uncheckedReason_;
    bool zip64_ = false;
    bool signingBlock_ = false;
};

Result<Walk> ZipWalker::run() {
    std::uint64_t position = 0;
    for (;;) {
        Result<std::optional<std::span<const std::byte>>> head = detail::readIfAvailable(content_, position, 4);
        if (!head.ok()) {
            return head.error();
        }
        if (!head->has_value()) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends before the central directory");
        }
        const std::uint32_t signature = loadLe32(**head, 0);
        if (signature == kLocalSignature && !signingBlock_) {
            if (members_.size() >= limits_.maxMembers) {
                return walk_.finish(WalkStatus::Broken, position,
                                    "more than " + count(limits_.maxMembers, "member", "members"));
            }
            Next next = member(position);
            if (!next.ok()) {
                return next.error();
            }
            if (!next->has_value()) {
                return walk_;
            }
            position = **next;
            continue;
        }
        if (signature == kCentralSignature) {
            return centralDirectory(position);
        }
        if (signature == kArchiveExtraSignature) {
            return walk_.finish(WalkStatus::Broken, position,
                                "an encrypted central directory (archive extra data record) is not supported");
        }
        if (!signingBlock_) {
            Next block = apkSigningBlock(position);
            if (!block.ok()) {
                return block.error();
            }
            if (block->has_value()) {
                signingBlock_ = true;
                position = **block;
                continue;
            }
        }
        return walk_.finish(WalkStatus::Broken, position,
                            "no local header or central directory after " +
                                count(members_.size(), "member", "members"));
    }
}

ZipWalker::Next ZipWalker::member(std::uint64_t at) {
    const std::string which = "member " + std::to_string(members_.size() + 1);
    Result<std::optional<std::span<const std::byte>>> read = detail::readIfAvailable(content_, at, kLocalHeaderSize);
    if (!read.ok()) {
        return read.error();
    }
    if (!read->has_value()) {
        walk_.finish(WalkStatus::Truncated, at, "the data ends inside the local header of " + which);
        return std::optional<std::uint64_t>{};
    }
    const std::span<const std::byte> header = **read;
    Member entry;
    entry.offset = at;
    const std::uint8_t version = loadU8(header, 4);
    entry.flags = loadLe16(header, 6);
    entry.method = loadLe16(header, 8);
    entry.crc = loadLe32(header, 14);
    const std::uint32_t compressed32 = loadLe32(header, 18);
    const std::uint32_t size32 = loadLe32(header, 22);
    entry.nameLength = loadLe16(header, 26);
    const std::uint16_t extraLength = loadLe16(header, 28);
    if (version > kMaxVersion || (entry.flags & kUnusedFlags) != 0 || !knownMethod(entry.method)) {
        walk_.finish(WalkStatus::Broken, at,
                     "the local header of " + which + " has a version, flags or method no ZIP writer uses");
        return std::optional<std::uint64_t>{};
    }

    const std::size_t variable = std::size_t{entry.nameLength} + extraLength;
    Result<std::optional<std::span<const std::byte>>> fields =
        detail::readIfAvailable(content_, at + kLocalHeaderSize, variable);
    if (!fields.ok()) {
        return fields.error();
    }
    if (!fields->has_value()) {
        walk_.finish(WalkStatus::Truncated, at, "the data ends inside the local header of " + which);
        return std::optional<std::uint64_t>{};
    }
    entry.nameCrc = crc32((**fields).first(entry.nameLength));
    if (entry.nameLength == 0) {
        walk_.noteProblem(at, which + " has no name");
    }
    const std::optional<std::span<const std::byte>> zip64 = zip64Field((**fields).subspan(entry.nameLength));
    zip64_ = zip64_ || zip64.has_value();
    entry.compressed = compressed32;
    entry.size = size32;
    const bool described = (entry.flags & kDataDescriptor) != 0;
    if (compressed32 == kMarker32 || size32 == kMarker32) {
        // The local header's ZIP64 field holds both sizes (APPNOTE 4.5.3).
        if (zip64.has_value() && zip64->size() >= 16) {
            entry.size = loadLe64(*zip64, 0);
            entry.compressed = loadLe64(*zip64, 8);
        } else if (!described) {
            walk_.finish(WalkStatus::Broken, at,
                         "the sizes of " + which + " are in a ZIP64 extra field its local header does not have");
            return std::optional<std::uint64_t>{};
        }
    }
    const std::uint64_t dataOffset = at + kLocalHeaderSize + variable;
    const bool encrypted =
        (entry.flags & (kEncrypted | kStrongEncryption)) != 0 || entry.method == kAesEncrypted;

    std::uint64_t next = 0;
    if (described) {
        Next after = describedData(entry, dataOffset, encrypted, zip64.has_value(), which);
        if (!after.ok() || !after->has_value()) {
            return after;
        }
        next = **after;
    } else {
        const std::optional<std::uint64_t> dataEnd = checkedAdd(dataOffset, entry.compressed);
        if (!dataEnd.has_value() || *dataEnd > content_.size()) {
            // Broken, not Truncated: a damaged size would make the archive
            // reach the end of the source, and the member headers of the
            // archives after it would all be skipped as its own.
            walk_.finish(WalkStatus::Broken, at,
                         "the recorded size of " + which + " (" + std::to_string(entry.compressed) +
                             " bytes) reaches beyond the data");
            return std::optional<std::uint64_t>{};
        }
        if (entry.method == kStored && !encrypted && entry.compressed != entry.size) {
            walk_.noteProblem(at, which + " is stored, but its compressed and uncompressed sizes differ");
        }
        if (depth_ == Depth::Full) {
            if (Status checked = checkData(entry, dataOffset, encrypted, which); !checked.ok()) {
                return checked.error();
            }
        }
        next = *dataEnd;
    }
    members_.push_back(entry);
    return std::optional<std::uint64_t>(next);
}

ZipWalker::Next ZipWalker::describedData(Member& entry, std::uint64_t dataOffset, bool encrypted, bool zip64,
                                         const std::string& which) {
    std::uint64_t length = 0;
    std::optional<IDeflateDecoder::Outcome> decoded;
    if (entry.method == kDeflated && !encrypted) {
        // DEFLATE data ends itself: decoding finds the descriptor.
        Result<IDeflateDecoder::Outcome> outcome = decoder_.decode(content_, dataOffset, content_.size(), budget());
        if (!outcome.ok()) {
            return outcome.error();
        }
        switch (outcome->kind) {
        case IDeflateDecoder::Outcome::Kind::Done:
            break;
        case IDeflateDecoder::Outcome::Kind::Truncated:
            walk_.finish(WalkStatus::Truncated, entry.offset, "the data ends inside the DEFLATE data of " + which);
            return std::optional<std::uint64_t>{};
        case IDeflateDecoder::Outcome::Kind::Invalid:
            walk_.finish(WalkStatus::Broken, std::max(outcome->offset, dataOffset),
                         "the DEFLATE data of " + which + " is invalid: " + outcome->detail);
            return std::optional<std::uint64_t>{};
        case IDeflateDecoder::Outcome::Kind::TooLarge:
            walk_.finish(WalkStatus::Broken, dataOffset,
                         "the members unpack to more than " + std::to_string(limits_.maxUnpacked) +
                             " bytes: the end of " + which + "'s data is not known");
            return std::optional<std::uint64_t>{};
        }
        unpacked_ += outcome->output;
        length = outcome->length;
        decoded = std::move(*outcome);
    } else {
        Next found = findDescriptor(dataOffset, zip64);
        if (!found.ok()) {
            return found.error();
        }
        if (!found->has_value()) {
            walk_.finish(WalkStatus::Broken, dataOffset,
                         "no data descriptor follows the data of " + which + ", whose size only it records");
            return std::optional<std::uint64_t>{};
        }
        length = **found;
    }

    // The descriptor: its signature (optional), the CRC-32 and both sizes.
    const std::uint64_t descriptorAt = dataOffset + length;
    std::uint64_t fieldsAt = descriptorAt;
    Result<std::optional<std::span<const std::byte>>> head = detail::readIfAvailable(content_, descriptorAt, 4);
    if (!head.ok()) {
        return head.error();
    }
    if (head->has_value() && loadLe32(**head, 0) == kDescriptorSignature) {
        fieldsAt += 4;
    }
    const std::size_t width = zip64 ? 8 : 4;
    Result<std::optional<std::span<const std::byte>>> fields =
        detail::readIfAvailable(content_, fieldsAt, 4 + 2 * width);
    if (!fields.ok()) {
        return fields.error();
    }
    if (!fields->has_value()) {
        walk_.finish(WalkStatus::Truncated, entry.offset, "the data ends inside the data descriptor of " + which);
        return std::optional<std::uint64_t>{};
    }
    entry.crc = loadLe32(**fields, 0);
    entry.compressed = zip64 ? loadLe64(**fields, 4) : loadLe32(**fields, 4);
    entry.size = zip64 ? loadLe64(**fields, 12) : loadLe32(**fields, 8);
    if (entry.compressed != length) {
        walk_.noteProblem(descriptorAt, "the data descriptor of " + which + " records " +
                                            std::to_string(entry.compressed) + " bytes of data, not " +
                                            std::to_string(length));
    }
    if (decoded.has_value()) {
        if (decoded->output != entry.size) {
            walk_.noteProblem(dataOffset, which + " unpacks to " + std::to_string(decoded->output) +
                                              " bytes, not the " + std::to_string(entry.size) + " recorded");
        } else if (decoded->crc32 != entry.crc) {
            walk_.noteProblem(dataOffset, "the CRC-32 of " + which + " is wrong");
        }
    } else if (depth_ == Depth::Full) {
        Member described = entry;
        described.compressed = length;
        if (entry.method == kStored && !encrypted && length != entry.size) {
            walk_.noteProblem(dataOffset, which + " is stored, but its data descriptor records another size");
        } else if (Status checked = checkData(described, dataOffset, encrypted, which); !checked.ok()) {
            return checked.error();
        }
    }
    return std::optional<std::uint64_t>(fieldsAt + 4 + 2 * width);
}

ZipWalker::Next ZipWalker::findDescriptor(std::uint64_t dataOffset, bool zip64) {
    const std::size_t fields = 4 + (zip64 ? 16 : 8);
    const std::uint64_t limit =
        dataOffset + std::min(content_.size() - std::min(dataOffset, content_.size()), limits_.maxUnpacked);
    std::uint64_t position = dataOffset;
    std::vector<std::uint64_t> candidates;
    while (position < limit && limit - position >= 4) {
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(kPiece, limit - position));
        Result<std::span<const std::byte>> piece = content_.read(position, length);
        if (!piece.ok()) {
            return piece.error();
        }
        candidates.clear();
        for (std::size_t i = 0; i + 4 <= piece->size(); ++i) {
            if ((*piece)[i] == std::byte{'P'} && loadLe32(*piece, i) == kDescriptorSignature) {
                candidates.push_back(position + i);
            }
        }
        for (const std::uint64_t at : candidates) {
            Result<std::optional<std::span<const std::byte>>> descriptor =
                detail::readIfAvailable(content_, at + 4, fields);
            if (!descriptor.ok()) {
                return descriptor.error();
            }
            if (!descriptor->has_value()) {
                continue;
            }
            const std::uint64_t recorded = zip64 ? loadLe64(**descriptor, 4) : loadLe32(**descriptor, 4);
            if (recorded == at - dataOffset) {
                return std::optional<std::uint64_t>(at - dataOffset);
            }
        }
        if (length < kPiece) {
            break;
        }
        // The next piece overlaps this one by 3 bytes: a signature may span them.
        position += length - 3;
    }
    return std::optional<std::uint64_t>{};
}

ZipWalker::Next ZipWalker::apkSigningBlock(std::uint64_t at) {
    Result<std::optional<std::span<const std::byte>>> sizeField = detail::readIfAvailable(content_, at, 8);
    if (!sizeField.ok()) {
        return sizeField.error();
    }
    if (members_.empty() || !sizeField->has_value()) {
        return std::optional<std::uint64_t>{};
    }
    const std::uint64_t size = loadLe64(**sizeField, 0);
    if (size < kApkBlockTrailer || at + 8 > content_.size() || size > content_.size() - at - 8) {
        return std::optional<std::uint64_t>{};
    }
    const std::uint64_t end = at + 8 + size;
    Result<std::span<const std::byte>> trailer = content_.read(end - kApkBlockTrailer, kApkBlockTrailer);
    if (!trailer.ok()) {
        return trailer.error();
    }
    for (std::size_t i = 0; i < kApkSigningMagic.size(); ++i) {
        if ((*trailer)[8 + i] != static_cast<std::byte>(kApkSigningMagic[i])) {
            return std::optional<std::uint64_t>{};
        }
    }
    if (loadLe64(*trailer, 0) != size) {
        return std::optional<std::uint64_t>{};
    }
    return std::optional<std::uint64_t>(end);
}

Result<Walk> ZipWalker::centralDirectory(std::uint64_t start) {
    // Its extent first: the entries' lengths.
    std::uint64_t position = start;
    std::uint64_t entries = 0;
    std::uint32_t signature = 0;
    const auto readSignature = [&]() -> Result<bool> {
        Result<std::optional<std::span<const std::byte>>> head = detail::readIfAvailable(content_, position, 4);
        if (!head.ok()) {
            return head.error();
        }
        if (!head->has_value()) {
            return false;
        }
        signature = loadLe32(**head, 0);
        return true;
    };
    for (;;) {
        Result<bool> read = readSignature();
        if (!read.ok()) {
            return read.error();
        }
        if (!*read) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends inside the central directory");
        }
        if (signature != kCentralSignature) {
            break;
        }
        Result<std::optional<std::span<const std::byte>>> header =
            detail::readIfAvailable(content_, position, kCentralHeaderSize);
        if (!header.ok()) {
            return header.error();
        }
        if (!header->has_value()) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends inside the central directory");
        }
        position += kCentralHeaderSize + std::uint64_t{loadLe16(**header, 28)} + loadLe16(**header, 30) +
                    loadLe16(**header, 32);
        ++entries;
    }
    const std::uint64_t end = position;

    if (signature == kDigitalSignature) {
        Result<std::optional<std::span<const std::byte>>> record = detail::readIfAvailable(content_, position, 6);
        if (!record.ok()) {
            return record.error();
        }
        if (!record->has_value()) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends inside the digital signature");
        }
        position += 6 + std::uint64_t{loadLe16(**record, 4)};
        Result<bool> read = readSignature();
        if (!read.ok()) {
            return read.error();
        }
        if (!*read) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends after the digital signature");
        }
    }

    // ZIP64: the end record and its locator.
    struct Zip64End {
        std::uint64_t at = 0;
        std::uint32_t disk = 0;
        std::uint32_t directoryDisk = 0;
        std::uint64_t entriesOnDisk = 0;
        std::uint64_t entries = 0;
        std::uint64_t size = 0;
        std::uint64_t offset = 0;
    };
    std::optional<Zip64End> zip64;
    if (signature == kZip64EndSignature) {
        Result<std::optional<std::span<const std::byte>>> record =
            detail::readIfAvailable(content_, position, kZip64EndSize);
        if (!record.ok()) {
            return record.error();
        }
        if (!record->has_value()) {
            return walk_.finish(WalkStatus::Truncated, position,
                                "the data ends inside the ZIP64 end of central directory record");
        }
        const std::uint64_t recordSize = loadLe64(**record, 4);
        zip64 = Zip64End{position,
                         loadLe32(**record, 16),
                         loadLe32(**record, 20),
                         loadLe64(**record, 24),
                         loadLe64(**record, 32),
                         loadLe64(**record, 40),
                         loadLe64(**record, 48)};
        const std::optional<std::uint64_t> after = checkedAdd<std::uint64_t>(position + 12, recordSize);
        if (recordSize < kZip64EndSize - 12 || !after.has_value() || *after > content_.size()) {
            return walk_.finish(WalkStatus::Broken, position,
                                "the ZIP64 end of central directory record has an impossible size");
        }
        position = *after;
        Result<bool> read = readSignature();
        if (!read.ok()) {
            return read.error();
        }
        if (!*read) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends after the ZIP64 end record");
        }
        if (signature == kZip64LocatorSignature) {
            Result<std::optional<std::span<const std::byte>>> locator =
                detail::readIfAvailable(content_, position, kZip64LocatorSize);
            if (!locator.ok()) {
                return locator.error();
            }
            if (!locator->has_value()) {
                return walk_.finish(WalkStatus::Truncated, position, "the data ends inside the ZIP64 locator");
            }
            if (loadLe64(**locator, 8) != zip64->at) {
                walk_.noteProblem(position, "the ZIP64 locator does not point at the ZIP64 end record");
            }
            if (loadLe32(**locator, 4) != 0 || loadLe32(**locator, 16) > 1) {
                return walk_.finish(WalkStatus::Broken, position, "a multi-disk (spanned) archive");
            }
            position += kZip64LocatorSize;
            Result<bool> again = readSignature();
            if (!again.ok()) {
                return again.error();
            }
            if (!*again) {
                return walk_.finish(WalkStatus::Truncated, position, "the data ends after the ZIP64 locator");
            }
        } else {
            walk_.noteProblem(position, "the ZIP64 end of central directory record has no locator");
        }
    }

    if (signature != kEndSignature) {
        return walk_.finish(WalkStatus::Broken, position,
                            "no end of central directory record after the central directory");
    }
    const std::uint64_t endAt = position;
    Result<std::optional<std::span<const std::byte>>> record = detail::readIfAvailable(content_, endAt, kEndSize);
    if (!record.ok()) {
        return record.error();
    }
    if (!record->has_value()) {
        return walk_.finish(WalkStatus::Truncated, endAt, "the data ends inside the end of central directory record");
    }
    const std::uint16_t disk16 = loadLe16(**record, 4);
    const std::uint16_t directoryDisk16 = loadLe16(**record, 6);
    const std::uint16_t onDisk16 = loadLe16(**record, 8);
    const std::uint16_t entries16 = loadLe16(**record, 10);
    const std::uint32_t size32 = loadLe32(**record, 12);
    const std::uint32_t offset32 = loadLe32(**record, 16);
    const std::uint16_t commentLength = loadLe16(**record, 20);
    const bool needsZip64 = disk16 == kMarker16 || directoryDisk16 == kMarker16 || onDisk16 == kMarker16 ||
                            entries16 == kMarker16 || size32 == kMarker32 || offset32 == kMarker32;
    if (needsZip64 && !zip64.has_value()) {
        return walk_.finish(WalkStatus::Broken, endAt,
                            "the end of central directory record needs a ZIP64 record the archive does not have");
    }
    const auto pick = [&](std::uint64_t value, std::uint64_t marker, std::uint64_t wide) {
        return value == marker && zip64.has_value() ? wide : value;
    };
    const std::uint64_t disk = pick(disk16, kMarker16, zip64 ? zip64->disk : 0);
    const std::uint64_t directoryDisk = pick(directoryDisk16, kMarker16, zip64 ? zip64->directoryDisk : 0);
    const std::uint64_t onDisk = pick(onDisk16, kMarker16, zip64 ? zip64->entriesOnDisk : 0);
    const std::uint64_t total = pick(entries16, kMarker16, zip64 ? zip64->entries : 0);
    const std::uint64_t size = pick(size32, kMarker32, zip64 ? zip64->size : 0);
    const std::uint64_t offset = pick(offset32, kMarker32, zip64 ? zip64->offset : 0);
    if (disk != 0 || directoryDisk != 0) {
        return walk_.finish(WalkStatus::Broken, endAt, "a multi-disk (spanned) archive");
    }
    if (offset != start) {
        // A member of an archive that starts earlier (whose start is not
        // here), or a directory that is not this archive's.
        return walk_.finish(WalkStatus::Broken, start,
                            offset > start ? "the central directory says the archive starts " +
                                                 std::to_string(offset - start) +
                                                 " bytes before its first member here: its first members are missing"
                                           : "the end of central directory record places the central directory at "
                                             "offset " + std::to_string(offset) + ", not " + std::to_string(start));
    }
    if (size != end - start) {
        walk_.noteProblem(endAt, "the end of central directory record records a central directory of " +
                                     std::to_string(size) + " bytes, not " + std::to_string(end - start));
    }
    if (onDisk != total || total != entries) {
        walk_.noteProblem(endAt, "the end of central directory record counts " + std::to_string(total) +
                                     " entries, the central directory holds " + std::to_string(entries));
    }
    const std::uint64_t fileEnd = endAt + kEndSize + commentLength;
    if (fileEnd > content_.size()) {
        return walk_.finish(WalkStatus::Truncated, endAt, "the data ends inside the archive comment");
    }
    if (depth_ == Depth::Full) {
        if (Status matched = matchEntries(start, end); !matched.ok()) {
            return matched.error();
        }
    }

    std::string summary = count(members_.size(), "member", "members");
    if (depth_ == Depth::Full) {
        summary += ", " + std::to_string(unpacked_) + " bytes unpacked";
        if (unchecked_ == 0) {
            summary += ", every member's CRC-32 right";
        }
    }
    if (zip64.has_value() || zip64_) {
        summary += ", ZIP64";
    }
    if (signingBlock_) {
        summary += ", an APK signing block";
    }
    return walk_.finish(WalkStatus::Complete, fileEnd, summary);
}

Status ZipWalker::matchEntries(std::uint64_t start, std::uint64_t end) {
    std::uint64_t position = start;
    std::uint64_t index = 0;
    while (position < end) {
        ++index;
        const std::string which = "central directory entry " + std::to_string(index);
        Result<std::span<const std::byte>> read = content_.read(position, kCentralHeaderSize);
        if (!read.ok()) {
            return read.error();
        }
        const std::span<const std::byte> header = *read;
        const std::uint16_t flags = loadLe16(header, 8);
        const std::uint16_t method = loadLe16(header, 10);
        const std::uint32_t crc = loadLe32(header, 16);
        const std::uint32_t compressed32 = loadLe32(header, 20);
        const std::uint32_t size32 = loadLe32(header, 24);
        const std::uint16_t nameLength = loadLe16(header, 28);
        const std::uint16_t extraLength = loadLe16(header, 30);
        const std::uint16_t commentLength = loadLe16(header, 32);
        const std::uint16_t disk16 = loadLe16(header, 34);
        const std::uint32_t offset32 = loadLe32(header, 42);
        Result<std::span<const std::byte>> fields =
            content_.read(position + kCentralHeaderSize, std::size_t{nameLength} + extraLength);
        if (!fields.ok()) {
            return fields.error();
        }
        const std::uint32_t nameCrc = crc32(fields->first(nameLength));
        Zip64Values values(zip64Field(fields->subspan(nameLength)));
        const std::optional<std::uint64_t> size = values.resolve(size32, kMarker32, 8);
        const std::optional<std::uint64_t> compressed = values.resolve(compressed32, kMarker32, 8);
        const std::optional<std::uint64_t> offset = values.resolve(offset32, kMarker32, 8);
        const std::optional<std::uint64_t> disk = values.resolve(disk16, kMarker16, 4);
        if (!size || !compressed || !offset || !disk) {
            walk_.noteProblem(position, which + " lacks the ZIP64 values its fields call for");
        } else if (*disk != 0) {
            walk_.noteProblem(position, which + " names disk " + std::to_string(*disk));
        } else {
            const auto found = std::lower_bound(members_.begin(), members_.end(), *offset,
                                                [](const Member& m, std::uint64_t at) { return m.offset < at; });
            if (found == members_.end() || found->offset != *offset) {
                walk_.noteProblem(position, which + " names offset " + std::to_string(*offset) +
                                                ", where no member starts");
            } else if (found->listed) {
                walk_.noteProblem(position, which + " names the member at offset " + std::to_string(*offset) +
                                                " a second time");
            } else {
                found->listed = true;
                std::string_view field;
                if (method != found->method) {
                    field = "compression method";
                } else if (((flags ^ found->flags) & kEncrypted) != 0) {
                    field = "encryption";
                } else if (crc != found->crc) {
                    field = "CRC-32";
                } else if (*compressed != found->compressed || *size != found->size) {
                    field = "sizes";
                } else if (nameLength != found->nameLength || nameCrc != found->nameCrc) {
                    field = "name";
                }
                if (!field.empty()) {
                    walk_.noteProblem(position, which + " does not match the member at offset " +
                                                    std::to_string(*offset) + ": its " + std::string(field) +
                                                    " differs");
                }
            }
        }
        position += kCentralHeaderSize + std::uint64_t{nameLength} + extraLength + commentLength;
    }
    for (const Member& entry : members_) {
        if (!entry.listed) {
            walk_.noteProblem(entry.offset, "the member at offset " + std::to_string(entry.offset) +
                                                " is not in the central directory");
            break;
        }
    }
    return success();
}

Status ZipWalker::checkData(const Member& entry, std::uint64_t dataOffset, bool encrypted, const std::string& which) {
    if (encrypted) {
        skip(which + " is encrypted");
        return success();
    }
    if (entry.method != kStored && entry.method != kDeflated) {
        skip(which + " uses " + methodName(entry.method));
        return success();
    }
    if (entry.size > budget()) {
        skip("the members unpack to more than " + std::to_string(limits_.maxUnpacked) + " bytes");
        return success();
    }
    if (entry.method == kStored) {
        Result<std::uint32_t> crc = crcOf(dataOffset, entry.compressed);
        if (!crc.ok()) {
            return crc.error();
        }
        unpacked_ += entry.compressed;
        if (*crc != entry.crc) {
            walk_.noteProblem(dataOffset, "the CRC-32 of " + which + " is wrong");
        }
        return success();
    }
    Result<IDeflateDecoder::Outcome> decoded =
        decoder_.decode(content_, dataOffset, dataOffset + entry.compressed, entry.size);
    if (!decoded.ok()) {
        return decoded.error();
    }
    unpacked_ += decoded->output;
    noteDecoded(*decoded, entry, dataOffset, which);
    return success();
}

void ZipWalker::noteDecoded(const IDeflateDecoder::Outcome& decoded, const Member& entry, std::uint64_t dataOffset,
                            const std::string& which) {
    switch (decoded.kind) {
    case IDeflateDecoder::Outcome::Kind::Done:
        if (decoded.length != entry.compressed) {
            walk_.noteProblem(dataOffset + decoded.length,
                              "the DEFLATE data of " + which + " ends " +
                                  std::to_string(entry.compressed - decoded.length) +
                                  " bytes before its recorded size");
        } else if (decoded.output != entry.size) {
            walk_.noteProblem(dataOffset, which + " unpacks to " + std::to_string(decoded.output) +
                                              " bytes, not the " + std::to_string(entry.size) + " recorded");
        } else if (decoded.crc32 != entry.crc) {
            walk_.noteProblem(dataOffset, "the CRC-32 of " + which + " is wrong");
        }
        return;
    case IDeflateDecoder::Outcome::Kind::Truncated:
        walk_.noteProblem(decoded.offset, "the DEFLATE data of " + which + " does not end within its recorded size");
        return;
    case IDeflateDecoder::Outcome::Kind::Invalid:
        walk_.noteProblem(decoded.offset, "the DEFLATE data of " + which + " is invalid: " + decoded.detail);
        return;
    case IDeflateDecoder::Outcome::Kind::TooLarge:
        walk_.noteProblem(dataOffset, which + " unpacks to more than the " + std::to_string(entry.size) +
                                          " bytes recorded");
        return;
    }
}

Result<std::uint32_t> ZipWalker::crcOf(std::uint64_t offset, std::uint64_t length) {
    std::uint32_t crc = 0;
    for (std::uint64_t done = 0; done < length;) {
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(kPiece, length - done));
        Result<std::span<const std::byte>> piece = content_.read(offset + done, take);
        if (!piece.ok()) {
            return piece.error();
        }
        crc = crc32Update(crc, *piece);
        done += take;
    }
    return crc;
}

void ZipWalker::skip(std::string reason) {
    if (unchecked_++ == 0) {
        uncheckedReason_ = std::move(reason);
    }
}

}  // namespace

Result<std::shared_ptr<const ZipFormat>> ZipFormat::make(std::shared_ptr<const IDeflateDecoder> decoder,
                                                         Limits limits) {
    if (decoder == nullptr) {
        return makeError(ErrorCode::InvalidInput, "the ZIP format needs a DEFLATE decoder");
    }
    if (limits.maxMembers == 0 || limits.maxUnpacked == 0) {
        return makeError(ErrorCode::InvalidInput, "the ZIP format's limits must not be 0");
    }
    return std::shared_ptr<const ZipFormat>(new ZipFormat(std::move(decoder), limits));
}

ZipFormat::ZipFormat(std::shared_ptr<const IDeflateDecoder> decoder, Limits limits)
    : decoder_(std::move(decoder)), limits_(limits) {
    descriptor_.id = "zip";
    descriptor_.name = "ZIP archive";
    descriptor_.extension = "zip";
    descriptor_.signatures = {carving::byteSignature("ZIP local header", {'P', 'K', 0x03, 0x04})};
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::StructureWalk;
    descriptor_.selfSynchronizing = true;
    // The central directory places every member from the archive's start:
    // an archive whose end was found is in one piece, and a bad CRC-32 in it
    // is damage, not a fragment boundary.
    descriptor_.foundEndMeansContiguous = true;
}

HeaderCheck ZipFormat::checkHeader(std::span<const std::byte> header) const {
    if (header.size() < kLocalHeaderSize || loadLe32(header, 0) != kLocalSignature) {
        return HeaderCheck::reject("no ZIP local header");
    }
    const std::uint8_t version = loadU8(header, 4);
    const std::uint16_t flags = loadLe16(header, 6);
    const std::uint16_t method = loadLe16(header, 8);
    const std::uint32_t compressed = loadLe32(header, 18);
    const std::uint32_t size = loadLe32(header, 22);
    const std::uint16_t nameLength = loadLe16(header, 26);
    if (version > kMaxVersion) {
        return HeaderCheck::reject("version needed to extract " + std::to_string(version / 10) + "." +
                                   std::to_string(version % 10) + " is beyond ZIP 6.3");
    }
    if ((flags & kUnusedFlags) != 0) {
        return HeaderCheck::reject("flags that no ZIP writer sets");
    }
    if (!knownMethod(method)) {
        return HeaderCheck::reject("unknown compression method " + std::to_string(method));
    }
    if (nameLength == 0) {
        return HeaderCheck::reject("a member without a name");
    }
    const std::span<const std::byte> name =
        header.subspan(kLocalHeaderSize, std::min<std::size_t>(nameLength, header.size() - kLocalHeaderSize));
    if (std::find(name.begin(), name.end(), std::byte{0}) != name.end()) {
        return HeaderCheck::reject("a NUL byte in the member's name");
    }
    const bool encrypted = (flags & (kEncrypted | kStrongEncryption)) != 0;
    if (method == kStored && !encrypted && (flags & kDataDescriptor) == 0 && compressed != size &&
        compressed != kMarker32 && size != kMarker32) {
        return HeaderCheck::reject("a stored member whose compressed and uncompressed sizes differ");
    }
    return HeaderCheck::accept();
}

Result<EndDetection> ZipFormat::findEnd(IContentReader& content) const {
    ZipWalker walker(content, Depth::Layout, *decoder_, limits_);
    Result<Walk> walk = walker.run();
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::endOf(*walk, content.size());
}

Result<ValidationResult> ZipFormat::validate(IContentReader& content) const {
    ZipWalker walker(content, Depth::Full, *decoder_, limits_);
    Result<Walk> walk = walker.run();
    if (!walk.ok()) {
        return walk.error();
    }
    ValidationResult verdict = detail::verdictOf(*walk, content.size());
    if (verdict.status == ValidationStatus::Valid && walker.unchecked() > 0) {
        // Consistent, but not every member could be checked: never trusted.
        verdict.status = ValidationStatus::NotValidated;
        verdict.detail += "; " + count(walker.unchecked(), "member was", "members were") +
                          " not checked (" + walker.uncheckedReason() + ")";
    }
    return verdict;
}

}  // namespace recovery::formats
