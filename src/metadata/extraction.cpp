#include "extraction.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"
#include "recovery/unicode.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace recovery::metadata::detail {

namespace {

using carving::IContentReader;

constexpr std::size_t kChunk = IContentReader::kMaxReadLength;

bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// `count` decimal digits at text[at], or none.
std::optional<std::uint32_t> digits(std::string_view text, std::size_t at, std::size_t count) {
    if (at > text.size() || text.size() - at < count) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!isDigit(text[at + i])) {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint32_t>(text[at + i] - '0');
    }
    return value;
}

std::string_view trimmed(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\0')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) {
        text.remove_suffix(1);
    }
    return text;
}

// Appends one decoded character: control characters become spaces, and
// spaces at the start are left out.
void putCharacter(std::string& out, char32_t c) {
    if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0)) {
        c = U' ';
    }
    if (c == U' ' && out.empty()) {
        return;
    }
    appendUtf8(out, c);
}

// The length of the valid UTF-8 sequence at bytes[at], or 0.
std::size_t utf8Sequence(std::span<const std::byte> bytes, std::size_t at, char32_t& decoded) noexcept {
    const auto lead = static_cast<std::uint8_t>(bytes[at]);
    std::size_t length = 0;
    char32_t value = 0;
    if (lead < 0x80) {
        decoded = lead;
        return 1;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        value = lead & 0x1FU;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        value = lead & 0x0FU;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        value = lead & 0x07U;
    } else {
        return 0;
    }
    if (bytes.size() - at < length) {
        return 0;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<std::uint8_t>(bytes[at + i]);
        if ((next & 0xC0U) != 0x80U) {
            return 0;
        }
        value = (value << 6) | (next & 0x3FU);
    }
    // Overlong forms, surrogates and values beyond U+10FFFF.
    if ((length == 3 && value < 0x800) || (length == 4 && (value < 0x10000 || value > 0x10FFFF)) ||
        (value >= 0xD800 && value <= 0xDFFF)) {
        return 0;
    }
    decoded = value;
    return length;
}

bool validUtf8(std::span<const std::byte> bytes) noexcept {
    for (std::size_t at = 0; at < bytes.size();) {
        char32_t ignored = 0;
        const std::size_t length = utf8Sequence(bytes, at, ignored);
        if (length == 0) {
            return false;
        }
        at += length;
    }
    return true;
}

// Ends the text: spaces at the end trimmed, cut to `maxLength` bytes at a
// character boundary.
void finishText(std::string& out, std::size_t maxLength) {
    if (out.size() > maxLength) {
        std::size_t cut = maxLength;
        while (cut > 0 && (static_cast<std::uint8_t>(out[cut]) & 0xC0U) == 0x80U) {
            --cut;
        }
        out.resize(cut);
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
}

}  // namespace

void Extraction::issue(std::optional<std::uint64_t> offset, std::string detail) {
    ++out.issueCount;
    if (out.issues.size() < MediaMetadata::kMaxIssues) {
        out.issues.push_back(MetadataIssue{offset, std::move(detail)});
    }
}

Result<std::span<const std::byte>> readUpTo(IContentReader& content, std::uint64_t offset, std::size_t length) {
    const std::uint64_t size = content.size();
    if (offset >= size || length == 0) {
        return std::span<const std::byte>{};
    }
    const auto take = static_cast<std::size_t>(std::min<std::uint64_t>({length, size - offset, kChunk}));
    return content.read(offset, take);
}

Result<std::vector<std::byte>> readBlock(IContentReader& content, std::uint64_t offset, std::uint64_t length) {
    std::vector<std::byte> block;
    const std::uint64_t size = content.size();
    if (offset >= size) {
        return block;
    }
    const std::uint64_t total = std::min(length, size - offset);
    block.reserve(static_cast<std::size_t>(total));
    for (std::uint64_t done = 0; done < total;) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(total - done, kChunk));
        Result<std::span<const std::byte>> bytes = content.read(offset + done, chunk);
        if (!bytes.ok()) {
            return bytes.error();
        }
        block.insert(block.end(), bytes->begin(), bytes->end());
        done += chunk;
    }
    return block;
}

bool startsWith(std::span<const std::byte> bytes, std::string_view text) noexcept {
    if (bytes.size() < text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (bytes[i] != static_cast<std::byte>(text[i])) {
            return false;
        }
    }
    return true;
}

Result<std::span<const std::byte>> WindowReader::read(std::uint64_t offset, std::size_t length) {
    if (length > kMaxReadLength || !rangeWithin<std::uint64_t>(offset, length, length_)) {
        return makeError(ErrorCode::InvalidInput, "read outside the preview");
    }
    return content_.read(offset_ + offset, length);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

std::string decodeText(std::span<const std::byte> bytes, TextEncoding encoding, std::size_t maxLength) {
    std::string out;
    // Decoding stops a little beyond the longest text kept.
    const std::size_t stop = maxLength + 8;
    if (encoding == TextEncoding::Utf8OrLatin1) {
        std::size_t length = 0;
        while (length < bytes.size() && bytes[length] != std::byte{0}) {
            ++length;
        }
        encoding = validUtf8(bytes.first(length)) ? TextEncoding::Utf8 : TextEncoding::Latin1;
    }
    switch (encoding) {
        case TextEncoding::Latin1:
            for (std::size_t at = 0; at < bytes.size() && bytes[at] != std::byte{0} && out.size() < stop; ++at) {
                putCharacter(out, static_cast<std::uint8_t>(bytes[at]));
            }
            break;
        case TextEncoding::Utf8:
        case TextEncoding::Utf8OrLatin1:
            for (std::size_t at = 0; at < bytes.size() && bytes[at] != std::byte{0} && out.size() < stop;) {
                char32_t decoded = 0;
                const std::size_t length = utf8Sequence(bytes, at, decoded);
                putCharacter(out, length == 0 ? kReplacementCharacter : decoded);
                at += length == 0 ? 1 : length;
            }
            break;
        case TextEncoding::Utf16:
        case TextEncoding::Utf16Be:
        case TextEncoding::Utf16Le: {
            bool bigEndian = encoding == TextEncoding::Utf16Be;
            std::size_t at = 0;
            if (encoding == TextEncoding::Utf16) {
                // Without a byte order mark: little-endian, as the writers
                // that leave it out write it.
                bigEndian = false;
                if (bytes.size() >= 2) {
                    const auto first = static_cast<std::uint8_t>(bytes[0]);
                    const auto second = static_cast<std::uint8_t>(bytes[1]);
                    if (first == 0xFE && second == 0xFF) {
                        bigEndian = true;
                        at = 2;
                    } else if (first == 0xFF && second == 0xFE) {
                        at = 2;
                    }
                }
            }
            const auto unitAt = [&](std::size_t position) -> char32_t {
                return bigEndian ? loadBe16(bytes, position) : loadLe16(bytes, position);
            };
            while (bytes.size() - at >= 2 && out.size() < stop) {
                const char32_t unit = unitAt(at);
                at += 2;
                if (unit == 0) {
                    break;
                }
                if (unit >= 0xD800 && unit <= 0xDBFF && bytes.size() - at >= 2) {
                    const char32_t low = unitAt(at);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        at += 2;
                        putCharacter(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                        continue;
                    }
                }
                // A byte order mark inside the text (between values) is dropped.
                if (unit == 0xFEFF) {
                    continue;
                }
                putCharacter(out, unit >= 0xD800 && unit <= 0xDFFF ? kReplacementCharacter : unit);
            }
            break;
        }
    }
    finishText(out, maxLength);
    return out;
}

std::size_t textEnd(std::span<const std::byte> bytes, bool wide) noexcept {
    if (!wide) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (bytes[i] == std::byte{0}) {
                return i + 1;
            }
        }
        return bytes.size();
    }
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        if (bytes[i] == std::byte{0} && bytes[i + 1] == std::byte{0}) {
            return i + 2;
        }
    }
    return bytes.size();
}

// ---------------------------------------------------------------------------
// Dates
// ---------------------------------------------------------------------------

bool validDateTime(const MediaDateTime& value) noexcept {
    using namespace std::chrono;
    if (value.year < 1 || value.year > 9999 || value.month < 1 || value.month > 12 || value.hour > 23 ||
        value.minute > 59 || value.second > 59) {
        return false;
    }
    const year_month_day_last monthEnd{year{value.year} / month{value.month} / std::chrono::last};
    if (value.day < 1 || value.day > static_cast<unsigned>(monthEnd.day())) {
        return false;
    }
    return value.zone != MediaDateTime::Zone::Offset || (value.offsetMinutes >= -1439 && value.offsetMinutes <= 1439);
}

std::optional<std::int16_t> parseUtcOffset(std::string_view text) {
    text = trimmed(text);
    if (text == "Z") {
        return std::int16_t{0};
    }
    if (text.size() < 3 || (text[0] != '+' && text[0] != '-')) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> hours = digits(text, 1, 2);
    std::optional<std::uint32_t> minutes = std::uint32_t{0};
    if (text.size() == 6 && text[3] == ':') {
        minutes = digits(text, 4, 2);
    } else if (text.size() == 5) {
        minutes = digits(text, 3, 2);
    } else if (text.size() != 3) {
        return std::nullopt;
    }
    if (!hours.has_value() || !minutes.has_value() || *hours > 23 || *minutes > 59) {
        return std::nullopt;
    }
    const auto total = static_cast<std::int16_t>(*hours * 60 + *minutes);
    return text[0] == '-' ? static_cast<std::int16_t>(-total) : total;
}

std::optional<MediaDateTime> parseIsoDateTime(std::string_view text) {
    text = trimmed(text);
    MediaDateTime value;
    const std::optional<std::uint32_t> year = digits(text, 0, 4);
    if (!year.has_value()) {
        return std::nullopt;
    }
    value.year = static_cast<std::int32_t>(*year);
    std::size_t at = 4;
    const auto part = [&](char separator, std::uint8_t& field, MediaDateTime::Precision precision) {
        if (at < text.size() && text[at] == separator) {
            if (const std::optional<std::uint32_t> number = digits(text, at + 1, 2); number.has_value()) {
                field = static_cast<std::uint8_t>(*number);
                value.precision = precision;
                at += 3;
                return true;
            }
        }
        return false;
    };
    if (part('-', value.month, MediaDateTime::Precision::Month) &&
        part('-', value.day, MediaDateTime::Precision::Day)) {
        bool hasTime = false;
        if (at < text.size() && (text[at] == 'T' || text[at] == ' ')) {
            if (const std::optional<std::uint32_t> hour = digits(text, at + 1, 2); hour.has_value()) {
                value.hour = static_cast<std::uint8_t>(*hour);
                value.precision = MediaDateTime::Precision::Hour;
                at += 3;
                hasTime = true;
                if (part(':', value.minute, MediaDateTime::Precision::Minute) &&
                    part(':', value.second, MediaDateTime::Precision::Second) && at < text.size() &&
                    (text[at] == '.' || text[at] == ',')) {
                    ++at;
                    while (at < text.size() && isDigit(text[at])) {
                        ++at;
                    }
                }
            }
        }
        if (hasTime && at < text.size()) {
            const std::optional<std::int16_t> offset = parseUtcOffset(text.substr(at));
            if (!offset.has_value()) {
                return std::nullopt;
            }
            const bool utc = text.substr(at) == "Z";
            value.zone = utc ? MediaDateTime::Zone::Utc : MediaDateTime::Zone::Offset;
            value.offsetMinutes = utc ? std::int16_t{0} : *offset;
            at = text.size();
        }
    }
    if (at != text.size() || !validDateTime(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<MediaDateTime> parseExifDateTime(std::string_view text) {
    text = trimmed(text);
    if (text.size() != 19 || (text[4] != ':' && text[4] != '-') || text[7] != text[4] ||
        (text[10] != ' ' && text[10] != 'T') || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> year = digits(text, 0, 4);
    const std::optional<std::uint32_t> month = digits(text, 5, 2);
    const std::optional<std::uint32_t> day = digits(text, 8, 2);
    const std::optional<std::uint32_t> hour = digits(text, 11, 2);
    const std::optional<std::uint32_t> minute = digits(text, 14, 2);
    const std::optional<std::uint32_t> second = digits(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second) {
        return std::nullopt;
    }
    MediaDateTime value;
    value.year = static_cast<std::int32_t>(*year);
    value.month = static_cast<std::uint8_t>(*month);
    value.day = static_cast<std::uint8_t>(*day);
    value.hour = static_cast<std::uint8_t>(*hour);
    value.minute = static_cast<std::uint8_t>(*minute);
    value.second = static_cast<std::uint8_t>(*second);
    value.precision = MediaDateTime::Precision::Second;
    if (!validDateTime(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<MediaDateTime> utcDateTime(std::int64_t unixSeconds) {
    using namespace std::chrono;
    // 0001-01-01T00:00:00Z and 9999-12-31T23:59:59Z.
    constexpr std::int64_t kFirst = -62'135'596'800;
    constexpr std::int64_t kLast = 253'402'300'799;
    if (unixSeconds < kFirst || unixSeconds > kLast) {
        return std::nullopt;
    }
    const sys_seconds instant{seconds{unixSeconds}};
    const sys_days day = floor<days>(instant);
    const year_month_day date{day};
    const hh_mm_ss<seconds> time{instant - day};
    MediaDateTime value;
    value.year = static_cast<int>(date.year());
    value.month = static_cast<std::uint8_t>(static_cast<unsigned>(date.month()));
    value.day = static_cast<std::uint8_t>(static_cast<unsigned>(date.day()));
    value.hour = static_cast<std::uint8_t>(time.hours().count());
    value.minute = static_cast<std::uint8_t>(time.minutes().count());
    value.second = static_cast<std::uint8_t>(time.seconds().count());
    value.precision = MediaDateTime::Precision::Second;
    value.zone = MediaDateTime::Zone::Utc;
    return value;
}

// ---------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------

std::string_view sniffPicture(std::span<const std::byte> head) noexcept {
    const auto at = [&](std::size_t i) { return static_cast<std::uint8_t>(head[i]); };
    if (head.size() >= 3 && at(0) == 0xFF && at(1) == 0xD8 && at(2) == 0xFF) {
        return "jpeg";
    }
    if (head.size() >= 8 && at(0) == 0x89 && startsWith(head.subspan(1), "PNG\r\n\x1A\n")) {
        return "png";
    }
    if (startsWith(head, "GIF87a") || startsWith(head, "GIF89a")) {
        return "gif";
    }
    if (head.size() >= 18 && startsWith(head, "BM")) {
        const std::uint32_t header = loadLe32(head, 14);
        if (header == 12 || header == 16 || header == 40 || header == 52 || header == 56 || header == 64 ||
            header == 108 || header == 124) {
            return "bmp";
        }
    }
    if (head.size() >= 12 && startsWith(head, "RIFF") && startsWith(head.subspan(8), "WEBP")) {
        return "webp";
    }
    return {};
}

Result<std::optional<PreviewSource>> probePicture(Extraction& x, PreviewKind kind, std::uint64_t offset,
                                                  std::uint64_t length, std::string_view what) {
    if (length == 0 || !rangeWithin(offset, length, x.content.size())) {
        x.issue(offset, std::string(what) + " lies outside the content");
        return std::optional<PreviewSource>{};
    }
    Result<std::span<const std::byte>> head = readUpTo(x.content, offset, static_cast<std::size_t>(
                                                                              std::min<std::uint64_t>(length, 32)));
    if (!head.ok()) {
        return head.error();
    }
    const std::string_view formatId = sniffPicture(*head);
    if (formatId.empty()) {
        x.issue(offset, std::string(what) + " is not a picture in a format the engine reads");
        return std::optional<PreviewSource>{};
    }
    WindowReader window(x.content, offset, length);
    MediaMetadata inner;
    Extraction nested{window, x.options, inner, true};
    Status read = success();
    if (formatId == "jpeg") {
        read = extractJpeg(nested);
    } else if (formatId == "png") {
        read = extractPng(nested);
    } else if (formatId == "gif") {
        read = extractGif(nested);
    } else if (formatId == "bmp") {
        read = extractBmp(nested);
    } else {
        read = extractWebp(nested);
    }
    if (!read.ok()) {
        return read.error();
    }
    if (!inner.image.has_value()) {
        x.issue(offset, std::string(what) + " (" + std::string(formatId) + "): its header cannot be read");
        return std::optional<PreviewSource>{};
    }
    PreviewSource preview;
    preview.kind = kind;
    preview.formatId = std::string(formatId);
    preview.mediaType = std::string(mediaTypeOfFormat(formatId));
    preview.offset = offset;
    preview.length = length;
    preview.width = inner.image->width;
    preview.height = inner.image->height;
    return std::optional<PreviewSource>{std::move(preview)};
}

// ---------------------------------------------------------------------------
// Durations
// ---------------------------------------------------------------------------

std::optional<MediaDuration> durationOf(std::uint64_t units, std::uint64_t scale) noexcept {
    constexpr std::uint64_t kMicro = 1'000'000;
    // The largest whole number of seconds a MediaDuration holds.
    constexpr auto kMaxSeconds = static_cast<std::uint64_t>(std::numeric_limits<MediaDuration::rep>::max()) / kMicro;
    if (scale == 0) {
        return std::nullopt;
    }
    const std::uint64_t seconds = units / scale;
    const std::uint64_t rest = units % scale;
    if (seconds >= kMaxSeconds) {
        return std::nullopt;
    }
    std::uint64_t fraction = 0;
    // rest < scale: exact while rest * 10^6 + scale / 2 fits.
    if (scale <= (std::uint64_t{1} << 40)) {
        fraction = (rest * kMicro + scale / 2) / scale;
    } else {
        fraction = static_cast<std::uint64_t>(std::llround(static_cast<double>(rest) / static_cast<double>(scale) *
                                                           static_cast<double>(kMicro)));
    }
    return MediaDuration{static_cast<MediaDuration::rep>(seconds * kMicro + fraction)};
}

std::uint64_t bitrateOf(std::uint64_t bytes, MediaDuration duration) noexcept {
    if (duration.count() <= 0) {
        return 0;
    }
    const double bits = static_cast<double>(bytes) * 8.0;
    const double rate = bits * 1'000'000.0 / static_cast<double>(duration.count());
    if (!(rate < 1.8e19)) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::llround(rate));
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

void parseTrackNumber(std::string_view text, MediaTags& tags) {
    text = trimmed(text);
    const std::size_t slash = text.find('/');
    const auto number = [](std::string_view part) -> std::optional<std::uint32_t> {
        part = trimmed(part);
        if (part.empty() || part.size() > 9) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (const char c : part) {
            if (!isDigit(c)) {
                return std::nullopt;
            }
            value = value * 10 + static_cast<std::uint32_t>(c - '0');
        }
        return value;
    };
    const std::optional<std::uint32_t> track = number(text.substr(0, slash));
    if (!track.has_value()) {
        return;
    }
    tags.track = *track;
    if (slash != std::string_view::npos) {
        if (const std::optional<std::uint32_t> total = number(text.substr(slash + 1)); total.has_value()) {
            tags.trackTotal = *total;
        }
    }
}

void fillTags(MediaTags& into, const MediaTags& from) {
    const auto fill = [](std::string& field, const std::string& value) {
        if (field.empty()) {
            field = value;
        }
    };
    fill(into.title, from.title);
    fill(into.artist, from.artist);
    fill(into.album, from.album);
    fill(into.genre, from.genre);
    if (!into.date.has_value()) {
        into.date = from.date;
    }
    if (!into.track.has_value()) {
        into.track = from.track;
        into.trackTotal = from.trackTotal;
    }
}

}  // namespace recovery::metadata::detail
