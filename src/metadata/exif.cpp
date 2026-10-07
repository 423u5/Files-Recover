// Exif (CIPA DC-008): the TIFF structure inside JPEG APP1 segments, PNG
// eXIf chunks and WebP EXIF chunks. Only what P17 shows is read: IFD0's
// camera make, model, orientation and date, the Exif IFD's dates and their
// offsets, and IFD1's JPEG thumbnail. Every offset and count is checked
// against the block; IFDs are followed one hop from IFD0, so a loop of IFD
// pointers cannot make the walk go on.

#include "extraction.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"

#include <string>

namespace recovery::metadata::detail {

namespace {

constexpr std::size_t kMaxIfdEntries = 4096;
constexpr std::size_t kEntrySize = 12;

namespace tag {
constexpr std::uint16_t kMake = 0x010F;
constexpr std::uint16_t kModel = 0x0110;
constexpr std::uint16_t kOrientation = 0x0112;
constexpr std::uint16_t kDateTime = 0x0132;
constexpr std::uint16_t kThumbnailOffset = 0x0201;
constexpr std::uint16_t kThumbnailLength = 0x0202;
constexpr std::uint16_t kExifIfd = 0x8769;
constexpr std::uint16_t kDateTimeOriginal = 0x9003;
constexpr std::uint16_t kDateTimeDigitized = 0x9004;
constexpr std::uint16_t kOffsetTime = 0x9010;
constexpr std::uint16_t kOffsetTimeOriginal = 0x9011;
constexpr std::uint16_t kOffsetTimeDigitized = 0x9012;
}  // namespace tag

// Bytes of one value of a TIFF field type; 0 for types TIFF does not define.
std::uint64_t typeSize(std::uint16_t type) noexcept {
    switch (type) {
        case 1:  // BYTE
        case 2:  // ASCII
        case 6:  // SBYTE
        case 7:  // UNDEFINED
            return 1;
        case 3:  // SHORT
        case 8:  // SSHORT
            return 2;
        case 4:   // LONG
        case 9:   // SLONG
        case 11:  // FLOAT
        case 13:  // IFD
            return 4;
        case 5:   // RATIONAL
        case 10:  // SRATIONAL
        case 12:  // DOUBLE
            return 8;
        default:
            return 0;
    }
}

struct Entry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    // The value's bytes; empty when they lie outside the block.
    std::span<const std::byte> value;
    // Block offset of the entry.
    std::size_t at = 0;
};

class Tiff {
public:
    Tiff(std::span<const std::byte> data, bool bigEndian) noexcept : data_(data), bigEndian_(bigEndian) {}

    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] std::span<const std::byte> data() const noexcept { return data_; }
    [[nodiscard]] std::uint16_t u16(std::span<const std::byte> bytes, std::size_t at) const noexcept {
        return bigEndian_ ? loadBe16(bytes, at) : loadLe16(bytes, at);
    }
    [[nodiscard]] std::uint32_t u32(std::span<const std::byte> bytes, std::size_t at) const noexcept {
        return bigEndian_ ? loadBe32(bytes, at) : loadLe32(bytes, at);
    }

private:
    std::span<const std::byte> data_;
    bool bigEndian_;
};

struct Ifd {
    std::vector<Entry> entries;
    // The next IFD's offset (0: none), when the IFD was read whole.
    std::optional<std::uint32_t> next;

    [[nodiscard]] const Entry* find(std::uint16_t wanted) const noexcept {
        for (const Entry& entry : entries) {
            if (entry.tag == wanted) {
                return &entry;
            }
        }
        return nullptr;
    }
};

Ifd readIfd(Extraction& x, const Tiff& tiff, std::uint32_t offset, std::uint64_t base, std::string_view name) {
    Ifd ifd;
    if (offset < 8 || !rangeWithin<std::uint64_t>(offset, 2, tiff.size())) {
        x.issue(base, "Exif " + std::string(name) + " lies outside the Exif block");
        return ifd;
    }
    std::size_t count = tiff.u16(tiff.data(), offset);
    const std::size_t fits = (tiff.size() - offset - 2) / kEntrySize;
    bool whole = true;
    if (count > fits) {
        x.issue(base + offset, "Exif " + std::string(name) + ": " + std::to_string(count) + " entries, " +
                                   std::to_string(fits) + " fit in the Exif block");
        count = fits;
        whole = false;
    }
    if (count > kMaxIfdEntries) {
        x.issue(base + offset, "Exif " + std::string(name) + ": more than " + std::to_string(kMaxIfdEntries) +
                                   " entries (the rest not read)");
        count = kMaxIfdEntries;
        whole = false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        Entry entry;
        entry.at = offset + 2 + i * kEntrySize;
        entry.tag = tiff.u16(tiff.data(), entry.at);
        entry.type = tiff.u16(tiff.data(), entry.at + 2);
        entry.count = tiff.u32(tiff.data(), entry.at + 4);
        const std::uint64_t total = typeSize(entry.type) * std::uint64_t{entry.count};
        if (typeSize(entry.type) == 0) {
            continue;
        }
        if (total <= 4) {
            entry.value = tiff.data().subspan(entry.at + 8, static_cast<std::size_t>(total));
        } else {
            const std::uint32_t valueOffset = tiff.u32(tiff.data(), entry.at + 8);
            if (rangeWithin<std::uint64_t>(valueOffset, total, tiff.size())) {
                entry.value = tiff.data().subspan(valueOffset, static_cast<std::size_t>(total));
            }
        }
        ifd.entries.push_back(entry);
    }
    const std::size_t nextAt = offset + 2 + count * kEntrySize;
    if (whole && rangeWithin<std::uint64_t>(nextAt, 4, tiff.size())) {
        ifd.next = tiff.u32(tiff.data(), nextAt);
    }
    return ifd;
}

// A SHORT or LONG value (the first, for a list).
std::optional<std::uint32_t> number(const Tiff& tiff, const Entry& entry) {
    if (entry.count == 0 || entry.value.empty()) {
        return std::nullopt;
    }
    if (entry.type == 3) {
        return tiff.u16(entry.value, 0);
    }
    if (entry.type == 4) {
        return tiff.u32(entry.value, 0);
    }
    return std::nullopt;
}

std::string text(Extraction& x, const Entry* entry, std::uint64_t base, std::string_view name) {
    if (entry == nullptr) {
        return {};
    }
    if (entry->value.empty() && entry->count != 0) {
        x.issue(base + entry->at, "Exif " + std::string(name) + " lies outside the Exif block");
        return {};
    }
    // ASCII, and BYTE or UNDEFINED as some writers store text.
    if (entry->type != 2 && entry->type != 1 && entry->type != 7) {
        x.issue(base + entry->at, "Exif " + std::string(name) + " is not text");
        return {};
    }
    return decodeText(entry->value, TextEncoding::Utf8OrLatin1, x.options.maxTextLength);
}

// A date tag with its offset tag; none when absent, empty or all zeros (not
// set), an issue when it is something else that is not a date.
std::optional<MediaDateTime> date(Extraction& x, const Ifd& ifd, std::uint16_t dateTag, std::uint16_t offsetTag,
                                  std::uint64_t base, std::string_view name) {
    const Entry* entry = ifd.find(dateTag);
    const std::string value = text(x, entry, base, name);
    if (value.empty()) {
        return std::nullopt;
    }
    std::optional<MediaDateTime> parsed = parseExifDateTime(value);
    if (!parsed.has_value()) {
        if (value.find_first_not_of("0: ") != std::string::npos) {
            x.issue(base + entry->at, "Exif " + std::string(name) + " is not a date");
        }
        return std::nullopt;
    }
    if (const Entry* offset = ifd.find(offsetTag); offset != nullptr) {
        const std::string zone = text(x, offset, base, "offset time");
        if (const std::optional<std::int16_t> minutes = parseUtcOffset(zone); minutes.has_value()) {
            parsed->zone = MediaDateTime::Zone::Offset;
            parsed->offsetMinutes = *minutes;
        } else if (!zone.empty() && zone.find_first_not_of(": ") != std::string::npos) {
            x.issue(base + offset->at, "Exif offset time is not an offset from UTC");
        }
    }
    return parsed;
}

std::optional<Orientation> orientation(Extraction& x, const Tiff& tiff, const Ifd& ifd, std::uint64_t base) {
    const Entry* entry = ifd.find(tag::kOrientation);
    if (entry == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> value = number(tiff, *entry);
    if (!value.has_value() || *value < 1 || *value > 8) {
        x.issue(base + entry->at, "Exif orientation is not one of the values 1 to 8");
        return std::nullopt;
    }
    return static_cast<Orientation>(*value);
}

}  // namespace

Result<ExifData> readExif(Extraction& x, std::uint64_t offset, std::uint64_t length) {
    ExifData exif;
    if (length > x.options.maxTagBytes) {
        x.issue(offset, "the Exif block of " + std::to_string(length) +
                            " bytes is larger than the tag limit: not read");
        return exif;
    }
    Result<std::vector<std::byte>> block = readBlock(x.content, offset, length);
    if (!block.ok()) {
        return block.error();
    }
    std::span<const std::byte> bytes = *block;
    std::uint64_t base = offset;
    if (startsWith(bytes, std::string_view("Exif\0\0", 6))) {
        bytes = bytes.subspan(6);
        base += 6;
    }
    if (bytes.size() < 8) {
        x.issue(offset, "the Exif block is too short for a TIFF header");
        return exif;
    }
    bool bigEndian = false;
    if (startsWith(bytes, "MM")) {
        bigEndian = true;
    } else if (!startsWith(bytes, "II")) {
        x.issue(base, "the Exif block does not start with a TIFF header");
        return exif;
    }
    const Tiff tiff(bytes, bigEndian);
    if (tiff.u16(bytes, 2) != 42) {
        x.issue(base, "the Exif block's TIFF header is not valid");
        return exif;
    }
    const Ifd ifd0 = readIfd(x, tiff, tiff.u32(bytes, 4), base, "IFD0");
    exif.make = text(x, ifd0.find(tag::kMake), base, "make");
    exif.model = text(x, ifd0.find(tag::kModel), base, "model");
    exif.orientation = orientation(x, tiff, ifd0, base);

    Ifd exifIfd;
    if (const Entry* pointer = ifd0.find(tag::kExifIfd); pointer != nullptr) {
        if (const std::optional<std::uint32_t> at = number(tiff, *pointer); at.has_value()) {
            exifIfd = readIfd(x, tiff, *at, base, "Exif IFD");
        } else if (pointer->type != 13 || pointer->value.empty()) {
            x.issue(base + pointer->at, "the Exif IFD pointer is not valid");
        } else {
            exifIfd = readIfd(x, tiff, tiff.u32(pointer->value, 0), base, "Exif IFD");
        }
    }
    exif.dateTaken = date(x, exifIfd, tag::kDateTimeOriginal, tag::kOffsetTimeOriginal, base, "date taken");
    if (!exif.dateTaken.has_value()) {
        exif.dateTaken = date(x, exifIfd, tag::kDateTimeDigitized, tag::kOffsetTimeDigitized, base, "date digitized");
    }
    if (!exif.dateTaken.has_value()) {
        // IFD0's DateTime takes the Exif IFD's OffsetTime.
        Ifd both = ifd0;
        if (const Entry* zone = exifIfd.find(tag::kOffsetTime); zone != nullptr) {
            both.entries.push_back(*zone);
        }
        exif.dateTaken = date(x, both, tag::kDateTime, tag::kOffsetTime, base, "date and time");
    }

    if (ifd0.next.has_value() && *ifd0.next != 0) {
        const Ifd ifd1 = readIfd(x, tiff, *ifd0.next, base, "IFD1");
        const Entry* thumbnail = ifd1.find(tag::kThumbnailOffset);
        const Entry* thumbnailLength = ifd1.find(tag::kThumbnailLength);
        if (thumbnail != nullptr) {
            const std::optional<std::uint32_t> at = number(tiff, *thumbnail);
            const std::optional<std::uint32_t> size =
                thumbnailLength != nullptr ? number(tiff, *thumbnailLength) : std::nullopt;
            if (!at.has_value() || !size.has_value() || *size == 0) {
                x.issue(base + thumbnail->at, "the Exif thumbnail has no valid offset and length");
            } else if (!rangeWithin<std::uint64_t>(*at, *size, tiff.size())) {
                x.issue(base + thumbnail->at, "the Exif thumbnail runs past the Exif block");
            } else {
                exif.thumbnailOffset = base + *at;
                exif.thumbnailLength = *size;
                exif.thumbnailOrientation = orientation(x, tiff, ifd1, base);
            }
        }
        // An uncompressed thumbnail (TIFF strips, IFD1 Compression 1) is not offered.
    }
    return exif;
}

Status applyExif(Extraction& x, const ExifData& exif, ImageMetadata& image) {
    image.orientation = exif.orientation;
    image.dateTaken = exif.dateTaken;
    image.cameraMake = exif.make;
    image.cameraModel = exif.model;
    if (!exif.thumbnailOffset.has_value() || x.nested) {
        return success();
    }
    Result<std::optional<PreviewSource>> thumbnail =
        probePicture(x, PreviewKind::Thumbnail, *exif.thumbnailOffset, exif.thumbnailLength, "the Exif thumbnail");
    if (!thumbnail.ok()) {
        return thumbnail.error();
    }
    if (thumbnail->has_value()) {
        PreviewSource preview = std::move(**thumbnail);
        preview.orientation = exif.thumbnailOrientation.has_value() ? exif.thumbnailOrientation : exif.orientation;
        x.out.previews.push_back(std::move(preview));
    }
    return success();
}

}  // namespace recovery::metadata::detail
