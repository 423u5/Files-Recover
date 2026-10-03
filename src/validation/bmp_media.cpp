// BMP media validation: RLE8 and RLE4 pixel data decoded run by run, with the
// position of every run: an encoded or absolute run must lie inside its row,
// a delta must stay inside the bitmap, no row may follow the last one, and an
// end-of-bitmap code must come before the end of the image data. Pixels a
// delta skips are left unwritten, as the format allows. Uncompressed bitmaps
// have nothing coded to decode.

#include "content_bytes.hpp"
#include "media_decoders.hpp"

#include "recovery/byte_order.hpp"

#include <string>

namespace recovery::validation::detail {

namespace {

constexpr std::uint64_t kFileHeader = 14;
constexpr std::uint32_t kCoreHeader = 12;
constexpr std::uint32_t kOs2ShortHeader = 16;
constexpr std::uint32_t kInfoHeader = 40;
constexpr std::uint32_t kOs2Header = 64;

constexpr std::uint32_t kRgb = 0;
constexpr std::uint32_t kRle8 = 1;
constexpr std::uint32_t kRle4 = 2;
constexpr std::uint32_t kBitfields = 3;
constexpr std::uint32_t kJpeg = 4;
constexpr std::uint32_t kPng = 5;
constexpr std::uint32_t kAlphaBitfields = 6;
constexpr std::uint32_t kCmyk = 11;
constexpr std::uint32_t kCmykRle8 = 12;
constexpr std::uint32_t kCmykRle4 = 13;

class BmpMediaValidator final : public IMediaValidator {
public:
    [[nodiscard]] std::string_view formatId() const noexcept override { return "bmp"; }

    [[nodiscard]] Result<LevelResult> validate(carving::IContentReader& content, const MediaLimits&) const override {
        const MediaVerdict verdict("bmp decoder");
        const std::uint64_t size = content.size();
        if (size < kFileHeader + 4) {
            return verdict.truncated(size, "the data ends inside the file header");
        }
        Result<std::span<const std::byte>> start = content.read(0, kFileHeader + 4);
        if (!start.ok()) {
            return start.error();
        }
        if (loadU8(*start, 0) != 'B' || loadU8(*start, 1) != 'M') {
            return verdict.failed(0, "no BMP file header");
        }
        const std::uint64_t pixelOffset = loadLe32(*start, 10);
        const std::uint32_t headerSize = loadLe32(*start, kFileHeader);
        if (headerSize == kCoreHeader || headerSize == kOs2ShortHeader) {
            return verdict.notApplicable("uncompressed pixels: nothing is coded");
        }
        if (headerSize < kInfoHeader) {
            return verdict.failed(kFileHeader, "an unknown DIB header size");
        }
        if (size < kFileHeader + kInfoHeader) {
            return verdict.truncated(size, "the data ends inside the DIB header");
        }
        Result<std::span<const std::byte>> header = content.read(0, kFileHeader + kInfoHeader);
        if (!header.ok()) {
            return header.error();
        }
        const auto width = static_cast<std::int32_t>(loadLe32(*header, 18));
        const auto height = static_cast<std::int32_t>(loadLe32(*header, 22));
        const std::uint32_t compression = loadLe32(*header, 30);
        const std::uint32_t imageSize = loadLe32(*header, 34);
        if (headerSize == kOs2Header && compression == kBitfields) {
            return verdict.unsupported("OS/2 Huffman 1D compression is not decoded");
        }
        switch (compression) {
        case kRgb:
        case kBitfields:
        case kAlphaBitfields:
        case kCmyk:
            return verdict.notApplicable("uncompressed pixels: nothing is coded");
        case kRle8:
        case kRle4:
        case kCmykRle8:
        case kCmykRle4:
            break;
        case kJpeg:
        case kPng:
            return verdict.unsupported("compression " + std::to_string(compression) +
                                       " (a JPEG or PNG inside the bitmap, or OS/2 RLE24) is not decoded");
        default:
            return verdict.unsupported("compression " + std::to_string(compression) + " is not decoded");
        }
        if (width <= 0 || height <= 0) {
            return verdict.failed(18, "an RLE bitmap of " + std::to_string(width) + "x" + std::to_string(height) +
                                          " (RLE bitmaps are bottom-up)");
        }
        if (imageSize == 0) {
            return verdict.failed(34, "an RLE bitmap without an image size");
        }
        const bool rle4 = compression == kRle4 || compression == kCmykRle4;
        const auto columns = static_cast<std::uint64_t>(width);
        const auto rows = static_cast<std::uint64_t>(height);
        const std::uint64_t end = pixelOffset + imageSize;

        ContentBytes bytes(content, pixelOffset, end);
        std::uint64_t x = 0;
        std::uint64_t y = 0;
        std::uint64_t pixels = 0;
        std::uint64_t codes = 0;
        const auto ended = [&](std::string_view inside) -> Result<LevelResult> {
            if (bytes.error().has_value()) {
                return *bytes.error();
            }
            if (end > size) {
                return verdict.truncated(size, "the data ends inside " + std::string(inside));
            }
            return verdict.failed(pixelOffset, "the image data ends without an end-of-bitmap code");
        };
        const auto outside = [&](std::uint64_t at, std::string detail) {
            return verdict.failed(at, "code " + std::to_string(codes) + ": " + std::move(detail));
        };
        for (;;) {
            const std::uint64_t at = bytes.position();
            std::uint8_t count = 0;
            std::uint8_t code = 0;
            if (!bytes.next(count) || !bytes.next(code)) {
                return ended("the RLE data");
            }
            ++codes;
            if (count != 0) {
                if (y >= rows || x + count > columns) {
                    return outside(at, "a run of " + std::to_string(count) + " pixels at column " + std::to_string(x) +
                                           " of row " + std::to_string(y) + " leaves the " + std::to_string(columns) +
                                           "x" + std::to_string(rows) + " bitmap");
                }
                x += count;
                pixels += count;
                continue;
            }
            if (code == 0) {  // end of line
                x = 0;
                if (++y > rows) {
                    return outside(at, "an end of line after the last row");
                }
            } else if (code == 1) {  // end of bitmap
                break;
            } else if (code == 2) {  // delta
                std::uint8_t dx = 0;
                std::uint8_t dy = 0;
                if (!bytes.next(dx) || !bytes.next(dy)) {
                    return ended("a delta");
                }
                x += dx;
                y += dy;
                if (x > columns || y > rows) {
                    return outside(at, "a delta to column " + std::to_string(x) + " of row " + std::to_string(y) +
                                           " leaves the bitmap");
                }
            } else {  // an absolute run of `code` pixels, padded to 16 bits
                if (y >= rows || x + code > columns) {
                    return outside(at, "an absolute run of " + std::to_string(code) + " pixels at column " +
                                           std::to_string(x) + " of row " + std::to_string(y) + " leaves the bitmap");
                }
                std::uint64_t length = rle4 ? (code + 1U) / 2 : code;
                length += length & 1;
                if (!bytes.skip(length)) {
                    return ended("an absolute run");
                }
                x += code;
                pixels += code;
            }
        }
        return verdict.passed(std::string(rle4 ? "RLE4" : "RLE8") + ", " + std::to_string(columns) + "x" +
                              std::to_string(rows) + ": " + std::to_string(codes) + " codes, " +
                              std::to_string(pixels) + " pixels written inside the bitmap, end-of-bitmap code");
    }
};

}  // namespace

std::shared_ptr<const IMediaValidator> makeBmpMediaValidator() {
    return std::make_shared<BmpMediaValidator>();
}

}  // namespace recovery::validation::detail
