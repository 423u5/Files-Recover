// Image metadata: the headers of JPEG, PNG, GIF, BMP and WebP files, their
// Exif blocks, and the blocks or chunks of animations (frames, loop count,
// frame delays). Only headers are read, never the coded pixels.

#include "extraction.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"

#include <array>
#include <bit>
#include <cstdlib>
#include <string>

namespace recovery::metadata::detail {

namespace {

constexpr std::uint32_t kMaxJpegSegments = 4096;
// 0xFF fill bytes allowed before one marker.
constexpr std::uint32_t kMaxFillBytes = 64 * 1024;
constexpr std::uint32_t kMaxChunks = 65536;

bool isStartOfFrame(std::uint8_t marker) noexcept {
    return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

std::uint32_t loadLe24(std::span<const std::byte> bytes, std::size_t at) noexcept {
    return std::uint32_t{loadU8(bytes, at)} | (std::uint32_t{loadU8(bytes, at + 1)} << 8) |
           (std::uint32_t{loadU8(bytes, at + 2)} << 16);
}

std::string mib(std::uint64_t bytes) {
    return std::to_string(bytes / (1024 * 1024)) + " MiB";
}

}  // namespace

// ---------------------------------------------------------------------------
// JPEG
// ---------------------------------------------------------------------------

Status extractJpeg(Extraction& x) {
    carving::IContentReader& content = x.content;
    Result<std::span<const std::byte>> start = readUpTo(content, 0, 2);
    if (!start.ok()) {
        return start.error();
    }
    if (start->size() < 2 || loadU8(*start, 0) != 0xFF || loadU8(*start, 1) != 0xD8) {
        x.issue(0, "no JPEG start-of-image marker");
        return success();
    }
    ImageMetadata image;
    bool frame = false;
    bool jfif = false;
    std::optional<std::uint8_t> adobeTransform;
    std::array<std::uint8_t, 4> ids{};
    std::uint8_t components = 0;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> exif;
    std::uint64_t pos = 2;
    std::uint32_t fill = 0;
    for (std::uint32_t segments = 0;;) {
        Result<std::span<const std::byte>> read = readUpTo(content, pos, 4);
        if (!read.ok()) {
            return read.error();
        }
        const std::span<const std::byte> marker = *read;
        if (marker.size() < 2) {
            if (!frame) {
                x.issue(pos, "the data ends before the image's frame header");
            }
            break;
        }
        if (loadU8(marker, 0) != 0xFF) {
            x.issue(pos, "no JPEG marker where a segment should start");
            break;
        }
        const std::uint8_t code = loadU8(marker, 1);
        if (code == 0xFF) {
            if (++fill > kMaxFillBytes) {
                x.issue(pos, "too many fill bytes before a JPEG marker");
                break;
            }
            ++pos;
            continue;
        }
        fill = 0;
        if (code == 0xD8 || code == 0x01 || (code >= 0xD0 && code <= 0xD7)) {
            pos += 2;  // markers without a segment
            continue;
        }
        if (code == 0xD9 || code == 0xDA) {
            if (!frame) {
                x.issue(pos, code == 0xD9 ? "the image ends before its frame header"
                                          : "the scan starts before a frame header");
            }
            break;  // EOI, or SOS: the headers are over
        }
        if (marker.size() < 4) {
            x.issue(pos, "the data ends inside a JPEG segment header");
            break;
        }
        const std::uint16_t length = loadBe16(marker, 2);
        if (length < 2) {
            x.issue(pos, "a JPEG segment length below 2");
            break;
        }
        if (++segments > kMaxJpegSegments) {
            x.issue(pos, "more than " + std::to_string(kMaxJpegSegments) + " JPEG segments");
            break;
        }
        const std::uint64_t payload = pos + 4;
        const std::uint64_t payloadLength = length - 2U;
        if (isStartOfFrame(code) && !frame) {
            Result<std::span<const std::byte>> header =
                readUpTo(content, payload, static_cast<std::size_t>(std::min<std::uint64_t>(payloadLength, 6 + 12)));
            if (!header.ok()) {
                return header.error();
            }
            if (header->size() < 6) {
                x.issue(pos, "the JPEG frame header is cut short");
                break;
            }
            const std::uint8_t precision = loadU8(*header, 0);
            image.height = loadBe16(*header, 1);
            image.width = loadBe16(*header, 3);
            components = loadU8(*header, 5);
            for (std::size_t i = 0; i < 4 && 6 + 3 * i < header->size(); ++i) {
                ids[i] = loadU8(*header, 6 + 3 * i);
            }
            image.progressive = code == 0xC2 || code == 0xC6 || code == 0xCA || code == 0xCE;
            image.bitsPerChannel = precision;
            image.channels = components;
            image.bitsPerPixel = static_cast<std::uint16_t>(precision * components);
            frame = true;
            if (image.height == 0) {
                x.issue(pos, "the JPEG frame header leaves the height to a DNL marker (not read)");
            }
            if (x.nested) {
                break;  // its size is all a nested picture needs
            }
        } else if (code == 0xE0 || code == 0xE1 || code == 0xEE) {
            Result<std::span<const std::byte>> head =
                readUpTo(content, payload, static_cast<std::size_t>(std::min<std::uint64_t>(payloadLength, 12)));
            if (!head.ok()) {
                return head.error();
            }
            if (code == 0xE0 && startsWith(*head, std::string_view("JFIF\0", 5))) {
                jfif = true;
            } else if (code == 0xE1 && !exif.has_value() && startsWith(*head, std::string_view("Exif\0\0", 6))) {
                exif = std::make_pair(payload, payloadLength);
            } else if (code == 0xEE && head->size() >= 12 && startsWith(*head, "Adobe")) {
                adobeTransform = loadU8(*head, 11);
            }
        }
        pos = payload + payloadLength;
    }
    if (!frame) {
        return success();
    }
    switch (components) {
        case 1:
            image.color = ColorModel::Grayscale;
            break;
        case 3:
            if (adobeTransform.has_value()) {
                image.color = *adobeTransform == 0 ? ColorModel::Rgb : ColorModel::YCbCr;
            } else if (!jfif && ids[0] == 'R' && ids[1] == 'G' && ids[2] == 'B') {
                image.color = ColorModel::Rgb;
            } else {
                image.color = ColorModel::YCbCr;
            }
            break;
        case 4:
            image.color = adobeTransform == std::uint8_t{2} ? ColorModel::Ycck : ColorModel::Cmyk;
            break;
        default:
            image.color = ColorModel::Unknown;
            break;
    }
    if (exif.has_value() && !x.nested) {
        Result<ExifData> data = readExif(x, exif->first, exif->second);
        if (!data.ok()) {
            return data.error();
        }
        if (Status applied = applyExif(x, *data, image); !applied.ok()) {
            return applied;
        }
    }
    x.out.image = std::move(image);
    return success();
}

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------

Status extractPng(Extraction& x) {
    carving::IContentReader& content = x.content;
    Result<std::span<const std::byte>> read = readUpTo(content, 0, 33);
    if (!read.ok()) {
        return read.error();
    }
    const std::span<const std::byte> head = *read;
    if (head.size() < 8 || loadU8(head, 0) != 0x89 || !startsWith(head.subspan(1), "PNG\r\n\x1A\n")) {
        x.issue(0, "no PNG signature");
        return success();
    }
    if (head.size() < 33 || loadBe32(head, 8) != 13 || !startsWith(head.subspan(12), "IHDR")) {
        x.issue(8, "the first PNG chunk is not a 13-byte IHDR");
        return success();
    }
    const std::uint32_t width = loadBe32(head, 16);
    const std::uint32_t height = loadBe32(head, 20);
    const std::uint8_t depth = loadU8(head, 24);
    const std::uint8_t colorType = loadU8(head, 25);
    const std::uint8_t interlace = loadU8(head, 28);
    std::uint8_t channels = 0;
    bool depthValid = false;
    const bool wide = depth == 8 || depth == 16;
    switch (colorType) {
        case 0:
            channels = 1;
            depthValid = depth == 1 || depth == 2 || depth == 4 || wide;
            break;
        case 2:
            channels = 3;
            depthValid = wide;
            break;
        case 3:
            channels = 1;
            depthValid = depth == 1 || depth == 2 || depth == 4 || depth == 8;
            break;
        case 4:
            channels = 2;
            depthValid = wide;
            break;
        case 6:
            channels = 4;
            depthValid = wide;
            break;
        default:
            break;
    }
    constexpr std::uint32_t kMaxDimension = 0x7FFFFFFF;
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension || !depthValid ||
        interlace > 1) {
        x.issue(16, "the PNG header's fields are not valid");
        return success();
    }
    ImageMetadata image;
    image.width = width;
    image.height = height;
    image.color = colorType == 3 ? ColorModel::Indexed : (colorType == 2 || colorType == 6 ? ColorModel::Rgb
                                                                                            : ColorModel::Grayscale);
    image.channels = channels;
    image.bitsPerChannel = depth;
    image.bitsPerPixel = static_cast<std::uint16_t>(depth * channels);
    image.alpha = colorType == 4 || colorType == 6;
    image.progressive = interlace == 1;
    if (x.nested) {
        x.out.image = std::move(image);
        return success();
    }
    std::optional<std::pair<std::uint64_t, std::uint64_t>> exif;
    bool animated = false;
    std::uint64_t delays = 0;  // microseconds
    std::uint32_t frameControls = 0;
    std::uint64_t pos = 33;
    for (std::uint32_t chunks = 0;; ++chunks) {
        if (pos >= content.size()) {
            x.issue(pos, "the data ends before the PNG's IEND chunk");
            break;
        }
        if (pos > x.options.maxScanBytes || chunks >= kMaxChunks) {
            x.issue(pos, "PNG chunks beyond the first " + mib(x.options.maxScanBytes) + " or " +
                             std::to_string(kMaxChunks) + " chunks not read");
            break;
        }
        Result<std::span<const std::byte>> chunk = readUpTo(content, pos, 8 + 26);
        if (!chunk.ok()) {
            return chunk.error();
        }
        if (chunk->size() < 8) {
            x.issue(pos, "the data ends inside a PNG chunk header");
            break;
        }
        const std::uint32_t length = loadBe32(*chunk, 0);
        if (length > 0x7FFFFFFFU) {
            x.issue(pos, "a PNG chunk length above 2^31 - 1");
            break;
        }
        const std::span<const std::byte> type = chunk->subspan(4, 4);
        const std::span<const std::byte> data = chunk->subspan(8);
        if (startsWith(type, "IEND")) {
            break;
        }
        if (startsWith(type, "tRNS")) {
            image.alpha = true;
        } else if (startsWith(type, "acTL") && length >= 8 && data.size() >= 8) {
            animated = true;
            image.frames = std::max<std::uint32_t>(loadBe32(data, 0), 1);
            image.loopCount = loadBe32(data, 4);
        } else if (startsWith(type, "fcTL") && length >= 26 && data.size() >= 26) {
            const std::uint64_t numerator = loadBe16(data, 20);
            const std::uint64_t denominator = loadBe16(data, 22) == 0 ? 100 : loadBe16(data, 22);
            delays += numerator * 1'000'000 / denominator;
            ++frameControls;
        } else if (startsWith(type, "eXIf") && !exif.has_value()) {
            exif = std::make_pair(pos + 8, std::uint64_t{length});
        }
        pos += std::uint64_t{12} + length;
    }
    if (animated && frameControls > 0) {
        x.out.duration = MediaDuration{static_cast<MediaDuration::rep>(delays)};
    }
    if (exif.has_value()) {
        Result<ExifData> data = readExif(x, exif->first, exif->second);
        if (!data.ok()) {
            return data.error();
        }
        if (Status applied = applyExif(x, *data, image); !applied.ok()) {
            return applied;
        }
    }
    x.out.image = std::move(image);
    return success();
}

// ---------------------------------------------------------------------------
// GIF
// ---------------------------------------------------------------------------

namespace {

// Skips the data sub-blocks at `pos`, up to and including the terminator.
// None when the data ends first.
Result<std::optional<std::uint64_t>> skipSubBlocks(carving::IContentReader& content, std::uint64_t pos) {
    for (;;) {
        Result<std::span<const std::byte>> size = readUpTo(content, pos, 1);
        if (!size.ok()) {
            return size.error();
        }
        if (size->empty()) {
            return std::optional<std::uint64_t>{};
        }
        const std::uint8_t length = loadU8(*size, 0);
        pos += 1 + std::uint64_t{length};
        if (length == 0) {
            return std::optional<std::uint64_t>{pos};
        }
    }
}

}  // namespace

Status extractGif(Extraction& x) {
    carving::IContentReader& content = x.content;
    Result<std::span<const std::byte>> read = readUpTo(content, 0, 13);
    if (!read.ok()) {
        return read.error();
    }
    const std::span<const std::byte> head = *read;
    if (head.size() < 13 || !(startsWith(head, "GIF87a") || startsWith(head, "GIF89a"))) {
        x.issue(0, "no GIF header");
        return success();
    }
    const std::uint8_t packed = loadU8(head, 10);
    const bool globalTable = (packed & 0x80U) != 0;
    std::uint8_t bits = static_cast<std::uint8_t>((packed & 7U) + 1);
    ImageMetadata image;
    image.width = loadLe16(head, 6);
    image.height = loadLe16(head, 8);
    image.color = ColorModel::Indexed;
    image.channels = 1;
    std::uint64_t pos = 13 + (globalTable ? std::uint64_t{3} << bits : 0);
    std::uint32_t frames = 0;
    std::uint64_t delay = 0;  // hundredths of a second
    bool tableKnown = globalTable;
    while (!x.nested) {
        if (pos > x.options.maxScanBytes) {
            x.issue(pos, "GIF blocks beyond the first " + mib(x.options.maxScanBytes) +
                             " not read: the frames are those counted before");
            x.out.durationEstimated = true;
            break;
        }
        Result<std::span<const std::byte>> block = readUpTo(content, pos, 14);
        if (!block.ok()) {
            return block.error();
        }
        if (block->empty()) {
            x.issue(pos, "the data ends before the GIF trailer");
            break;
        }
        const std::uint8_t introducer = loadU8(*block, 0);
        if (introducer == 0x3B) {
            break;
        }
        if (introducer == 0x2C) {
            if (block->size() < 10) {
                x.issue(pos, "the data ends inside a GIF image descriptor");
                break;
            }
            const std::uint8_t local = loadU8(*block, 9);
            ++frames;
            if (frames == 1) {
                image.progressive = (local & 0x40U) != 0;
            }
            if (!tableKnown && (local & 0x80U) != 0) {
                bits = static_cast<std::uint8_t>((local & 7U) + 1);
                tableKnown = true;
            }
            pos += 10 + ((local & 0x80U) != 0 ? std::uint64_t{3} << ((local & 7U) + 1) : 0) + 1;
        } else if (introducer == 0x21) {
            if (block->size() < 2) {
                x.issue(pos, "the data ends inside a GIF extension");
                break;
            }
            const std::uint8_t label = loadU8(*block, 1);
            if (label == 0xF9 && block->size() >= 7 && loadU8(*block, 2) == 4) {
                image.alpha = image.alpha || (loadU8(*block, 3) & 1U) != 0;
                delay += loadLe16(*block, 4);
            } else if (label == 0xFF && block->size() >= 14 && loadU8(*block, 2) == 11 &&
                       (startsWith(block->subspan(3), "NETSCAPE2.0") || startsWith(block->subspan(3), "ANIMEXTS1.0"))) {
                Result<std::span<const std::byte>> loop = readUpTo(content, pos + 14, 4);
                if (!loop.ok()) {
                    return loop.error();
                }
                if (loop->size() >= 4 && loadU8(*loop, 0) == 3 && loadU8(*loop, 1) == 1) {
                    image.loopCount = loadLe16(*loop, 2);
                }
            }
            pos += 2;
        } else {
            x.issue(pos, "an unknown GIF block");
            break;
        }
        Result<std::optional<std::uint64_t>> next = skipSubBlocks(content, pos);
        if (!next.ok()) {
            return next.error();
        }
        if (!next->has_value()) {
            x.issue(pos, "the data ends inside GIF data sub-blocks");
            break;
        }
        pos = **next;
    }
    image.bitsPerChannel = bits;
    image.bitsPerPixel = bits;
    if (!x.nested) {
        if (frames == 0) {
            x.issue(std::nullopt, "the GIF holds no image");
        }
        image.frames = std::max<std::uint32_t>(frames, 1);
        if (frames > 1) {
            x.out.duration = MediaDuration{static_cast<MediaDuration::rep>(delay * 10'000)};
        }
    }
    x.out.image = std::move(image);
    return success();
}

// ---------------------------------------------------------------------------
// BMP
// ---------------------------------------------------------------------------

Status extractBmp(Extraction& x) {
    Result<std::span<const std::byte>> read = readUpTo(x.content, 0, 14 + 124 + 16);
    if (!read.ok()) {
        return read.error();
    }
    const std::span<const std::byte> head = *read;
    if (head.size() < 18 || !startsWith(head, "BM")) {
        x.issue(0, "no BMP header");
        return success();
    }
    const std::uint32_t headerSize = loadLe32(head, 14);
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::uint16_t bitCount = 0;
    std::uint32_t compression = 0;
    if (headerSize == 12) {
        if (head.size() < 26) {
            x.issue(14, "the BMP header is cut short");
            return success();
        }
        width = loadLe16(head, 18);
        height = loadLe16(head, 20);
        bitCount = loadLe16(head, 24);
    } else if (headerSize == 16 || headerSize == 40 || headerSize == 52 || headerSize == 56 || headerSize == 64 ||
               headerSize == 108 || headerSize == 124) {
        if (head.size() < 14 + std::min<std::size_t>(headerSize, 40)) {
            x.issue(14, "the BMP header is cut short");
            return success();
        }
        width = static_cast<std::int32_t>(loadLe32(head, 18));
        height = static_cast<std::int32_t>(loadLe32(head, 22));
        bitCount = loadLe16(head, 28);
        compression = headerSize >= 20 ? loadLe32(head, 30) : 0;
    } else {
        x.issue(14, "a BMP header of an unknown size");
        return success();
    }
    if (width <= 0 || height == 0 || (bitCount != 1 && bitCount != 2 && bitCount != 4 && bitCount != 8 &&
                                       bitCount != 16 && bitCount != 24 && bitCount != 32 && bitCount != 64 &&
                                       !(bitCount == 0 && (compression == 4 || compression == 5)))) {
        x.issue(18, "the BMP header's size or bits per pixel are not valid");
        return success();
    }
    // OS/2 headers (16 and 64 bytes) give compressions 3 and 4 other
    // meanings (Huffman 1D, RLE24), and have no bit fields.
    const bool os2 = headerSize == 16 || headerSize == 64;
    ImageMetadata image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(std::abs(height));
    image.bitsPerPixel = bitCount;
    if (!os2 && (compression == 4 || compression == 5)) {
        // The pixels are a JPEG or PNG image.
        image.color = ColorModel::Unknown;
    } else if (bitCount <= 8) {
        image.color = ColorModel::Indexed;
        image.channels = 1;
        image.bitsPerChannel = static_cast<std::uint8_t>(bitCount);
    } else {
        image.color = ColorModel::Rgb;
        image.channels = 3;
        image.bitsPerChannel = bitCount == 16 ? 5 : (bitCount == 64 ? 16 : 8);
        // Bit fields: the masks right after the 40 bytes of the header (in
        // the header from V2 on, after it for a 40-byte one).
        constexpr std::size_t kMasks = 14 + 40;
        const bool bitFields = !os2 && (compression == 3 || compression == 6);
        const bool alphaMask = compression == 6 || headerSize >= 56;
        if (bitFields && head.size() >= kMasks + (alphaMask ? 16 : 12)) {
            const int red = std::popcount(loadLe32(head, kMasks));
            const int green = std::popcount(loadLe32(head, kMasks + 4));
            const int blue = std::popcount(loadLe32(head, kMasks + 8));
            image.bitsPerChannel = red == green && green == blue ? static_cast<std::uint8_t>(red) : 0;
            if (alphaMask && loadLe32(head, kMasks + 12) != 0) {
                image.alpha = true;
                image.channels = 4;
            }
        }
    }
    x.out.image = std::move(image);
    return success();
}

// ---------------------------------------------------------------------------
// WebP
// ---------------------------------------------------------------------------

namespace {

// What a VP8 or VP8L chunk's header says.
struct Bitstream {
    bool valid = false;
    bool lossless = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool alpha = false;
};

Bitstream bitstream(std::span<const std::byte> type, std::span<const std::byte> data) {
    Bitstream out;
    if (startsWith(type, "VP8 ") && data.size() >= 10) {
        const bool keyFrame = (loadU8(data, 0) & 1U) == 0;
        out.valid = keyFrame && loadU8(data, 3) == 0x9D && loadU8(data, 4) == 0x01 && loadU8(data, 5) == 0x2A;
        out.width = loadLe16(data, 6) & 0x3FFFU;
        out.height = loadLe16(data, 8) & 0x3FFFU;
    } else if (startsWith(type, "VP8L") && data.size() >= 5) {
        const std::uint32_t bits = loadLe32(data, 1);
        out.valid = loadU8(data, 0) == 0x2F && (bits >> 29) == 0;
        out.lossless = true;
        out.width = (bits & 0x3FFFU) + 1;
        out.height = ((bits >> 14) & 0x3FFFU) + 1;
        out.alpha = ((bits >> 28) & 1U) != 0;
    }
    return out;
}

void setColor(ImageMetadata& image, const Bitstream& stream) {
    image.color = stream.lossless ? ColorModel::Rgb : ColorModel::YCbCr;
    image.alpha = image.alpha || stream.alpha;
}

}  // namespace

Status extractWebp(Extraction& x) {
    carving::IContentReader& content = x.content;
    Result<std::span<const std::byte>> read = readUpTo(content, 0, 12);
    if (!read.ok()) {
        return read.error();
    }
    if (read->size() < 12 || !startsWith(*read, "RIFF") || !startsWith(read->subspan(8), "WEBP")) {
        x.issue(0, "no WebP header");
        return success();
    }
    const std::uint64_t end = std::min<std::uint64_t>(std::uint64_t{8} + loadLe32(*read, 4), content.size());
    ImageMetadata image;
    bool first = true;
    bool extended = false;
    bool colorKnown = false;
    std::uint32_t frames = 0;
    std::uint64_t duration = 0;  // milliseconds
    std::optional<std::pair<std::uint64_t, std::uint64_t>> exif;
    std::uint64_t pos = 12;
    for (std::uint32_t chunks = 0; pos + 8 <= end; ++chunks) {
        if (pos > x.options.maxScanBytes || chunks >= kMaxChunks) {
            x.issue(pos, "WebP chunks beyond the first " + mib(x.options.maxScanBytes) + " or " +
                             std::to_string(kMaxChunks) + " chunks not read");
            x.out.durationEstimated = true;
            break;
        }
        Result<std::span<const std::byte>> chunk = readUpTo(content, pos, 8 + 26);
        if (!chunk.ok()) {
            return chunk.error();
        }
        const std::span<const std::byte> type = chunk->subspan(0, 4);
        const std::uint32_t size = loadLe32(*chunk, 4);
        const std::span<const std::byte> data =
            chunk->subspan(8, static_cast<std::size_t>(std::min<std::uint64_t>(size, chunk->size() - 8)));
        if (first) {
            first = false;
            if (startsWith(type, "VP8X")) {
                if (data.size() < 10) {
                    x.issue(pos, "the WebP VP8X chunk is cut short");
                    return success();
                }
                extended = true;
                const std::uint8_t flags = loadU8(data, 0);
                image.alpha = (flags & 0x10U) != 0;
                image.width = loadLe24(data, 4) + 1;
                image.height = loadLe24(data, 7) + 1;
            } else {
                const Bitstream stream = bitstream(type, data);
                if (!stream.valid) {
                    x.issue(pos, "the first WebP chunk is not a valid VP8, VP8L or VP8X header");
                    return success();
                }
                image.width = stream.width;
                image.height = stream.height;
                setColor(image, stream);
                colorKnown = true;
            }
            if (x.nested) {
                break;
            }
        } else if (startsWith(type, "ALPH")) {
            image.alpha = true;
        } else if (startsWith(type, "ANIM") && data.size() >= 6) {
            image.loopCount = loadLe16(data, 4);
        } else if (startsWith(type, "ANMF") && data.size() >= 16) {
            ++frames;
            duration += loadLe24(data, 12);
            if (!colorKnown && data.size() >= 16 + 8) {
                // The frame's own first chunk follows its 16-byte header.
                Result<std::span<const std::byte>> inner = readUpTo(content, pos + 8 + 16, 8 + 10);
                if (!inner.ok()) {
                    return inner.error();
                }
                if (inner->size() >= 8) {
                    const Bitstream stream = bitstream(inner->subspan(0, 4), inner->subspan(8));
                    if (stream.valid) {
                        setColor(image, stream);
                        colorKnown = true;
                    }
                }
            }
        } else if ((startsWith(type, "VP8 ") || startsWith(type, "VP8L")) && !colorKnown) {
            const Bitstream stream = bitstream(type, data);
            if (stream.valid) {
                setColor(image, stream);
                colorKnown = true;
            }
        } else if (startsWith(type, "EXIF") && !exif.has_value()) {
            exif = std::make_pair(pos + 8, std::uint64_t{size});
        }
        pos += std::uint64_t{8} + size + (size & 1U);
    }
    if (pos < end && pos + 8 > end && !x.nested) {
        x.issue(pos, "the data ends inside a WebP chunk header");
    }
    if (extended && !colorKnown && !x.nested) {
        x.issue(12, "the WebP file holds no VP8 or VP8L image");
    }
    image.channels = image.alpha ? 4 : 3;
    image.bitsPerChannel = 8;
    image.bitsPerPixel = static_cast<std::uint16_t>(8 * image.channels);
    if (frames > 0) {
        image.frames = frames;
        x.out.duration = MediaDuration{static_cast<MediaDuration::rep>(duration * 1000)};
    }
    if (exif.has_value() && !x.nested) {
        Result<ExifData> data = readExif(x, exif->first, exif->second);
        if (!data.ok()) {
            return data.error();
        }
        if (Status applied = applyExif(x, *data, image); !applied.ok()) {
            return applied;
        }
    }
    x.out.image = std::move(image);
    return success();
}

}  // namespace recovery::metadata::detail
