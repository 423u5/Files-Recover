#include "audio_tags.hpp"

#include "recovery/byte_order.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace recovery::formats::detail {

namespace {

using carving::IContentReader;

constexpr std::string_view kId3v1 = "TAG";
constexpr std::string_view kId3v1Extended = "TAG+";
constexpr std::size_t kId3v1ExtendedSize = 227;
constexpr std::string_view kApe = "APETAGEX";
constexpr std::size_t kApeHeaderSize = 32;
constexpr std::uint32_t kApeIsHeader = 1U << 29;
constexpr std::uint32_t kMaxApeSize = 16 * 1024 * 1024;
constexpr std::string_view kLyricsBegin = "LYRICSBEGIN";
constexpr std::string_view kLyrics2End = "LYRICS200";
constexpr std::string_view kLyrics1End = "LYRICSEND";
constexpr std::size_t kLyricsSizeDigits = 6;
// Lyrics3 v1 holds at most 5100 bytes of lyrics; v2 at most 999999 bytes in all.
constexpr std::uint64_t kMaxLyrics1 = 5100;
constexpr std::uint64_t kMaxLyrics2 = 999'999;
constexpr std::uint32_t kMaxTags = 16;

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

// True when all of `bytes` (fewer than text.size()) are the start of `text`: a
// marker cut off by the end of the data.
bool isCutMarker(std::span<const std::byte> bytes, std::string_view text) noexcept {
    if (bytes.empty() || bytes.size() >= text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != static_cast<std::byte>(text[i])) {
            return false;
        }
    }
    return true;
}

std::uint32_t syncsafe(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 7) | (loadU8(bytes, offset + i) & 0x7FU);
    }
    return value;
}

bool isSyncsafe(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        if ((loadU8(bytes, offset + i) & 0x80) != 0) {
            return false;
        }
    }
    return true;
}

bool isFrameIdChar(std::uint8_t c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Walks the frames of the ID3v2 tag at `offset`. Returns what is wrong with
// them, or an empty string. Version 2.4 frame sizes are syncsafe; iTunes
// wrote plain ones for years, so `syncsafeSizes` false reads them that way.
Result<std::string> checkFrames(IContentReader& content, std::uint64_t offset, const Id3v2Header& header,
                                bool syncsafeSizes) {
    std::uint64_t position = offset + kId3v2HeaderSize;
    const std::uint64_t end = position + header.size;
    const bool extended = (header.flags & 0x40) != 0;
    if (header.version < 4 && (header.flags & 0x80) != 0) {
        return std::string{};  // unsynchronised as a whole: the frames' stored layout differs from their sizes
    }
    if (header.version == 2 && extended) {
        return std::string{};  // "compressed", with no scheme ever defined
    }
    if (extended) {
        if (end - position < 4) {
            return std::string("the extended header crosses the end of the tag");
        }
        Result<std::span<const std::byte>> size = content.read(position, 4);
        if (!size.ok()) {
            return size.error();
        }
        // 2.3: the size excludes its own 4 bytes (6 or 10); 2.4: syncsafe, including them.
        const std::uint64_t length = header.version == 3 ? 4ULL + loadBe32(*size, 0) : syncsafe(*size, 0);
        if ((header.version == 3 && length != 10 && length != 14) || (header.version == 4 && length < 6) ||
            length > end - position) {
            return std::string("invalid extended header size");
        }
        position += length;
    }
    const std::size_t frameHeader = header.version == 2 ? 6 : 10;
    const std::size_t idLength = header.version == 2 ? 3 : 4;
    while (position < end) {
        Result<std::span<const std::byte>> first = content.read(position, 1);
        if (!first.ok()) {
            return first.error();
        }
        if (loadU8(*first, 0) == 0) {
            break;  // padding
        }
        if (end - position < frameHeader) {
            return std::string("a frame header crosses the end of the tag");
        }
        Result<std::span<const std::byte>> bytes = content.read(position, frameHeader);
        if (!bytes.ok()) {
            return bytes.error();
        }
        for (std::size_t i = 0; i < idLength; ++i) {
            if (!isFrameIdChar(loadU8(*bytes, i))) {
                return std::string("a frame id that is not upper-case letters and digits");
            }
        }
        std::uint64_t size = 0;
        if (header.version == 2) {
            size = (std::uint64_t{loadU8(*bytes, 3)} << 16) | (std::uint64_t{loadU8(*bytes, 4)} << 8) |
                   loadU8(*bytes, 5);
        } else if (header.version == 4 && syncsafeSizes) {
            if (!isSyncsafe(*bytes, 4)) {
                return std::string("a frame size that is not syncsafe");
            }
            size = syncsafe(*bytes, 4);
        } else {
            size = loadBe32(*bytes, 4);
        }
        if (size > end - position - frameHeader) {
            return std::string("a frame runs beyond the end of the tag");
        }
        position += frameHeader + size;
    }
    if (header.footer) {
        Result<std::span<const std::byte>> footer = content.read(end, kId3v2HeaderSize);
        if (!footer.ok()) {
            return footer.error();
        }
        if (!startsWith(*footer, "3DI") || loadU8(*footer, 3) != header.version || loadU8(*footer, 5) != header.flags ||
            !isSyncsafe(*footer, 6) || syncsafe(*footer, 6) != header.size) {
            return std::string("the footer does not repeat the header");
        }
    }
    return std::string{};
}

// Notes a problem when the frames of the tag at `offset` do not fit in it.
Status checkTag(IContentReader& content, std::uint64_t offset, const Id3v2Header& header, Walk& walk) {
    Result<std::string> problem = checkFrames(content, offset, header, true);
    if (!problem.ok()) {
        return problem.error();
    }
    if (!problem->empty() && header.version == 4) {
        Result<std::string> plain = checkFrames(content, offset, header, false);
        if (!plain.ok()) {
            return plain.error();
        }
        if (plain->empty()) {
            return success();
        }
    }
    if (!problem->empty()) {
        walk.noteProblem(offset, "ID3v2." + std::to_string(header.version) + " tag: " + *problem);
    }
    return success();
}

// The bytes at `offset`, at most `length` of them (fewer at the end of the data).
Result<std::span<const std::byte>> peek(IContentReader& content, std::uint64_t offset, std::size_t length) {
    const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(length, content.size() - offset));
    return content.read(offset, available);
}

}  // namespace

std::optional<Id3v2Header> parseId3v2Header(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kId3v2HeaderSize || !startsWith(bytes, "ID3")) {
        return std::nullopt;
    }
    Id3v2Header header;
    header.version = loadU8(bytes, 3);
    header.flags = loadU8(bytes, 5);
    if (header.version < 2 || header.version > 4 || loadU8(bytes, 4) == 0xFF || !isSyncsafe(bytes, 6)) {
        return std::nullopt;
    }
    constexpr std::array<std::uint8_t, 3> kDefinedFlags = {0xC0, 0xE0, 0xF0};
    if ((header.flags & ~kDefinedFlags[header.version - 2U]) != 0) {
        return std::nullopt;
    }
    header.size = syncsafe(bytes, 6);
    header.footer = header.version == 4 && (header.flags & 0x10) != 0;
    return header;
}

Result<TagRun> skipLeadingTags(IContentReader& content, std::uint64_t offset, Depth depth, Walk& walk) {
    TagRun run;
    run.end = offset;
    while (run.count < kMaxTags) {
        Result<std::optional<std::span<const std::byte>>> bytes =
            readIfAvailable(content, run.end, kId3v2HeaderSize);
        if (!bytes.ok()) {
            return bytes.error();
        }
        if (!bytes->has_value()) {
            // Fewer bytes than a header: the start of one cut off by the end of the data?
            Result<std::span<const std::byte>> rest = peek(content, run.end, kId3v2HeaderSize);
            if (!rest.ok()) {
                return rest.error();
            }
            run.truncated = isCutMarker(*rest, "ID3") || startsWith(*rest, "ID3");
            if (run.truncated) {
                return run;
            }
            break;
        }
        const std::optional<Id3v2Header> header = parseId3v2Header(**bytes);
        if (!header.has_value()) {
            break;
        }
        if (header->totalSize() > content.size() - run.end) {
            run.truncated = true;
            return run;
        }
        if (depth == Depth::Full) {
            if (Status checked = checkTag(content, run.end, *header, walk); !checked.ok()) {
                return checked.error();
            }
        }
        run.end += header->totalSize();
        ++run.count;
    }
    if (run.count == 0) {
        return run;
    }
    // Zero padding after the tags. Nearly always there is none: look at one
    // byte before reading on in chunks.
    Result<std::optional<std::span<const std::byte>>> first = readIfAvailable(content, run.end, 1);
    if (!first.ok()) {
        return first.error();
    }
    if (first->has_value() && (**first)[0] != std::byte{0}) {
        return run;
    }
    SequentialReader zeros(content, run.end, run.end + kMaxZeroPadding);
    for (;;) {
        Result<std::optional<std::uint8_t>> byte = zeros.next();
        if (!byte.ok()) {
            return byte.error();
        }
        if (!byte->has_value()) {
            // The data ended inside the padding: the frames were cut off.
            run.truncated = zeros.position() >= content.size();
            run.end = zeros.position();
            return run;
        }
        if (**byte != 0) {
            run.end = zeros.position() - 1;
            return run;
        }
    }
}

Result<TagRun> skipTrailingTags(IContentReader& content, std::uint64_t offset, Depth depth, Walk& walk) {
    TagRun run;
    run.end = offset;
    while (run.count < kMaxTags && run.end < content.size()) {
        const std::uint64_t remaining = content.size() - run.end;
        Result<std::span<const std::byte>> head = peek(content, run.end, kApeHeaderSize);
        if (!head.ok()) {
            return head.error();
        }
        const std::span<const std::byte> bytes = *head;
        for (const std::string_view marker : {kId3v1, kApe, kLyricsBegin, std::string_view("ID3")}) {
            if (isCutMarker(bytes, marker)) {
                run.truncated = true;
                return run;
            }
        }
        if (startsWith(bytes, kId3v1Extended)) {
            if (remaining < kId3v1ExtendedSize + kId3v1Size) {
                run.truncated = true;
                return run;
            }
            Result<std::span<const std::byte>> tag = content.read(run.end + kId3v1ExtendedSize, kId3v1.size());
            if (!tag.ok()) {
                return tag.error();
            }
            if (!startsWith(*tag, kId3v1)) {
                break;
            }
            run.end += kId3v1ExtendedSize + kId3v1Size;
            ++run.count;
            break;  // ID3v1 is the last thing in a file
        }
        if (startsWith(bytes, kId3v1)) {
            if (remaining < kId3v1Size) {
                run.truncated = true;
                return run;
            }
            run.end += kId3v1Size;
            ++run.count;
            break;
        }
        if (startsWith(bytes, kApe)) {
            if (remaining < kApeHeaderSize) {
                run.truncated = true;
                return run;
            }
            const std::uint32_t version = loadLe32(bytes, 8);
            const std::uint32_t size = loadLe32(bytes, 12);
            const std::uint32_t flags = loadLe32(bytes, 20);
            if ((version != 1000 && version != 2000) || size < kApeHeaderSize || size > kMaxApeSize) {
                break;
            }
            // A header announces the items and a footer; a footer on its own closes an empty tag.
            const std::uint64_t total = (flags & kApeIsHeader) != 0 ? kApeHeaderSize + std::uint64_t{size}
                                                                    : std::uint64_t{kApeHeaderSize};
            if ((flags & kApeIsHeader) == 0 && size != kApeHeaderSize) {
                break;
            }
            if (total > remaining) {
                run.truncated = true;
                return run;
            }
            if (depth == Depth::Full && total > kApeHeaderSize) {
                Result<std::span<const std::byte>> footer = content.read(run.end + total - kApeHeaderSize, 8);
                if (!footer.ok()) {
                    return footer.error();
                }
                if (!startsWith(*footer, kApe)) {
                    walk.noteProblem(run.end, "APE tag without its footer");
                }
            }
            run.end += total;
            ++run.count;
            continue;
        }
        if (startsWith(bytes, kLyricsBegin)) {
            // The size (v2) and the end marker come last, so search for the end.
            const std::uint64_t searchEnd = run.end + kLyricsBegin.size() + kMaxLyrics2 + kLyrics2End.size();
            Result<std::optional<std::uint64_t>> end = findPattern(
                content, std::as_bytes(std::span(kLyrics2End)), run.end + kLyricsBegin.size(), searchEnd);
            if (!end.ok()) {
                return end.error();
            }
            std::optional<std::uint64_t> total;
            if (end->has_value() && **end >= run.end + kLyricsBegin.size() + kLyricsSizeDigits) {
                Result<std::span<const std::byte>> digits = content.read(**end - kLyricsSizeDigits, kLyricsSizeDigits);
                if (!digits.ok()) {
                    return digits.error();
                }
                std::uint64_t size = 0;
                bool numeric = true;
                for (const std::byte digit : *digits) {
                    const auto c = static_cast<std::uint8_t>(digit);
                    numeric = numeric && c >= '0' && c <= '9';
                    size = size * 10 + (c - static_cast<std::uint8_t>('0'));
                }
                if (numeric && size == **end - kLyricsSizeDigits - run.end) {
                    total = **end + kLyrics2End.size() - run.end;
                }
            }
            if (!total.has_value()) {
                const std::uint64_t v1End = run.end + kLyricsBegin.size() + kMaxLyrics1 + kLyrics1End.size();
                Result<std::optional<std::uint64_t>> v1 = findPattern(
                    content, std::as_bytes(std::span(kLyrics1End)), run.end + kLyricsBegin.size(), v1End);
                if (!v1.ok()) {
                    return v1.error();
                }
                if (v1->has_value()) {
                    total = **v1 + kLyrics1End.size() - run.end;
                } else if (content.size() < searchEnd) {
                    run.truncated = true;  // the data ends before any end marker could
                    return run;
                }
            }
            if (!total.has_value()) {
                break;
            }
            run.end += *total;
            ++run.count;
            continue;
        }
        const std::optional<Id3v2Header> id3 = parseId3v2Header(bytes);
        if (id3.has_value()) {
            if (id3->totalSize() > remaining) {
                run.truncated = true;
                return run;
            }
            if (depth == Depth::Full) {
                if (Status checked = checkTag(content, run.end, *id3, walk); !checked.ok()) {
                    return checked.error();
                }
            }
            run.end += id3->totalSize();
            ++run.count;
            continue;
        }
        break;
    }
    return run;
}

}  // namespace recovery::formats::detail
