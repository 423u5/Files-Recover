#include "formats/bmp_format.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"
#include "structure_walk.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace recovery::formats {

namespace {

using carving::EndDetection;
using carving::HeaderCheck;
using carving::IContentReader;
using carving::ValidationResult;
using detail::Depth;
using detail::Walk;
using detail::WalkStatus;

constexpr std::size_t kFileHeader = 14;
constexpr std::uint32_t kCoreHeader = 12;
constexpr std::uint32_t kOs2ShortHeader = 16;
constexpr std::uint32_t kInfoHeader = 40;
constexpr std::uint32_t kOs2Header = 64;
constexpr std::uint32_t kV5Header = 124;
// bV5CSType values that put a color profile in the file.
constexpr std::uint32_t kProfileEmbedded = 0x4D424544;  // 'MBED'
constexpr std::uint32_t kProfileLinked = 0x4C494E4B;    // 'LINK'
constexpr std::uint32_t kMaxPaletteColors = 1U << 24;

// biCompression values.
constexpr std::uint32_t kRgb = 0;
constexpr std::uint32_t kRle8 = 1;
constexpr std::uint32_t kRle4 = 2;
constexpr std::uint32_t kBitfields = 3;  // OS/2 2.x: Huffman 1D
constexpr std::uint32_t kJpeg = 4;       // OS/2 2.x: RLE24
constexpr std::uint32_t kPng = 5;
constexpr std::uint32_t kAlphaBitfields = 6;
constexpr std::uint32_t kCmyk = 11;
constexpr std::uint32_t kCmykRle8 = 12;
constexpr std::uint32_t kCmykRle4 = 13;

bool isKnownHeaderSize(std::uint32_t size) noexcept {
    switch (size) {
    case kCoreHeader:
    case kOs2ShortHeader:
    case kInfoHeader:
    case 52:
    case 56:
    case kOs2Header:
    case 108:
    case kV5Header:
        return true;
    default:
        return false;
    }
}

std::string headerName(std::uint32_t size) {
    switch (size) {
    case kCoreHeader:
        return "OS/2 core header";
    case kOs2ShortHeader:
    case kOs2Header:
        return "OS/2 2.x header";
    case kInfoHeader:
        return "BITMAPINFOHEADER";
    case 52:
        return "BITMAPV2INFOHEADER";
    case 56:
        return "BITMAPV3INFOHEADER";
    case 108:
        return "BITMAPV4HEADER";
    default:
        return "BITMAPV5HEADER";
    }
}

struct Layout {
    std::uint32_t headerSize = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool topDown = false;
    std::uint16_t planes = 1;
    std::uint16_t bitCount = 0;
    std::uint32_t compression = kRgb;
    std::uint64_t fileSize = 0;
    std::uint64_t pixelOffset = 0;
    // Where the file header, DIB header, bit masks and palette end.
    std::uint64_t headersEnd = 0;
    std::uint64_t pixelBytes = 0;
    // What the headers need: the pixel data, and an embedded or linked color profile.
    std::uint64_t required = 0;
    // Run-length encoded pixel data (4 or 8 bits per pixel).
    bool rle = false;
    bool rle4 = false;
};

struct Parsed {
    Layout layout;
    // Why the headers make no sense (no layout can be computed), or empty.
    std::string fatal;
    // Inconsistencies a real file might still have.
    std::vector<std::string> problems;
    // Values no writer produces (reason, or empty); the header check refuses them.
    std::string implausible;
};

bool bitCountAllowed(std::uint32_t headerSize, std::uint32_t compression, std::uint16_t bits) noexcept {
    if (headerSize == kCoreHeader) {
        return bits == 1 || bits == 4 || bits == 8 || bits == 24;
    }
    const bool os2 = headerSize == kOs2ShortHeader || headerSize == kOs2Header;
    switch (compression) {
    case kRgb:
        return bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 16 || bits == 24 || bits == 32;
    case kRle8:
    case kCmykRle8:
        return bits == 8;
    case kRle4:
    case kCmykRle4:
        return bits == 4;
    case kBitfields:
        return os2 ? bits == 1 : bits == 16 || bits == 32;
    case kJpeg:
        return os2 ? bits == 24 : bits == 0 || bits == 24 || bits == 32;
    case kPng:
        return bits == 0 || bits == 24 || bits == 32;
    case kAlphaBitfields:
        return bits == 16 || bits == 32;
    case kCmyk:
        return bits == 32;
    default:
        return false;
    }
}

bool isUncompressed(const Layout& layout, bool os2) noexcept {
    return layout.compression == kRgb || layout.compression == kCmyk ||
           (!os2 && (layout.compression == kBitfields || layout.compression == kAlphaBitfields));
}

// `headers` holds the file header and the whole DIB header.
Parsed parse(std::span<const std::byte> headers) {
    Parsed parsed;
    Layout& layout = parsed.layout;
    layout.headerSize = loadLe32(headers, 14);
    layout.fileSize = loadLe32(headers, 2);
    layout.pixelOffset = loadLe32(headers, 10);
    const bool os2 = layout.headerSize == kOs2ShortHeader || layout.headerSize == kOs2Header;
    std::uint32_t colorsUsed = 0;
    std::uint32_t imageSize = 0;
    if (layout.headerSize == kCoreHeader) {
        layout.width = loadLe16(headers, 18);
        layout.height = loadLe16(headers, 20);
        layout.planes = loadLe16(headers, 22);
        layout.bitCount = loadLe16(headers, 24);
    } else {
        const auto width = static_cast<std::int32_t>(loadLe32(headers, 18));
        const auto height = static_cast<std::int32_t>(loadLe32(headers, 22));
        if (width <= 0) {
            parsed.fatal = "width " + std::to_string(width);
            return parsed;
        }
        layout.width = static_cast<std::uint32_t>(width);
        layout.topDown = height < 0;
        // A negative height is a top-down bitmap; its magnitude still fits in 32 bits.
        layout.height = layout.topDown ? static_cast<std::uint32_t>(-static_cast<std::int64_t>(height))
                                       : static_cast<std::uint32_t>(height);
        layout.planes = loadLe16(headers, 26);
        layout.bitCount = loadLe16(headers, 28);
        if (layout.headerSize >= kInfoHeader) {
            layout.compression = loadLe32(headers, 30);
            imageSize = loadLe32(headers, 34);
            colorsUsed = loadLe32(headers, 46);
        }
    }
    if (layout.width == 0 || layout.height == 0) {
        parsed.fatal = "a bitmap of width or height 0";
        return parsed;
    }
    if (!bitCountAllowed(layout.headerSize, layout.compression, layout.bitCount)) {
        parsed.fatal = "compression " + std::to_string(layout.compression) + " with " +
                       std::to_string(layout.bitCount) + " bits per pixel";
        return parsed;
    }
    if (layout.pixelOffset < kFileHeader + layout.headerSize) {
        parsed.fatal = "the pixel data offset points into the headers";
        return parsed;
    }
    if (layout.planes != 1) {
        parsed.implausible = std::to_string(layout.planes) + " planes";
        parsed.problems.push_back(parsed.implausible);
    }
    const bool uncompressed = isUncompressed(layout, os2);
    if (layout.topDown && !uncompressed) {
        parsed.implausible = "a compressed top-down bitmap";
        parsed.problems.push_back(parsed.implausible);
    }
    const bool indexed = layout.bitCount >= 1 && layout.bitCount <= 8;
    if (colorsUsed > kMaxPaletteColors || (indexed && colorsUsed > (1U << layout.bitCount))) {
        parsed.implausible = std::to_string(colorsUsed) + " palette colors";
        parsed.problems.push_back(parsed.implausible);
        colorsUsed = 0;
    }

    // Headers, bit masks and palette.
    std::uint64_t masks = 0;
    if (layout.headerSize == kInfoHeader && layout.compression == kBitfields) {
        masks = 12;
    } else if (layout.headerSize == kInfoHeader && layout.compression == kAlphaBitfields) {
        masks = 16;
    }
    std::uint64_t colors = colorsUsed;
    if (indexed && (colorsUsed == 0 || layout.headerSize == kCoreHeader)) {
        colors = 1ULL << layout.bitCount;
    }
    const std::uint64_t entrySize = layout.headerSize == kCoreHeader ? 3 : 4;
    layout.headersEnd = kFileHeader + layout.headerSize + masks + colors * entrySize;
    if (layout.pixelOffset < layout.headersEnd) {
        parsed.problems.push_back("the pixel data offset points into the palette or bit masks");
    }

    // Pixel data.
    if (uncompressed) {
        const std::uint64_t stride = (std::uint64_t{layout.width} * layout.bitCount + 31) / 32 * 4;
        const std::optional<std::uint64_t> bytes = checkedMul<std::uint64_t>(stride, layout.height);
        if (!bytes.has_value()) {
            parsed.fatal = "the pixel data size overflows";
            return parsed;
        }
        layout.pixelBytes = *bytes;
    } else {
        if (imageSize == 0) {
            parsed.fatal = "a compressed bitmap without an image size";
            return parsed;
        }
        layout.pixelBytes = imageSize;
        layout.rle = layout.compression == kRle8 || layout.compression == kRle4 ||
                     layout.compression == kCmykRle8 || layout.compression == kCmykRle4;
        layout.rle4 = layout.compression == kRle4 || layout.compression == kCmykRle4;
    }
    const std::optional<std::uint64_t> required = checkedAdd(layout.pixelOffset, layout.pixelBytes);
    if (!required.has_value()) {
        parsed.fatal = "the pixel data size overflows";
        return parsed;
    }
    layout.required = *required;

    // A color profile stored in the file (its offset counts from the DIB header).
    if (layout.headerSize >= kV5Header) {
        const std::uint32_t colorSpace = loadLe32(headers, kFileHeader + 56);
        if (colorSpace == kProfileEmbedded || colorSpace == kProfileLinked) {
            const std::uint64_t profileOffset = loadLe32(headers, kFileHeader + 112);
            const std::uint64_t profileSize = loadLe32(headers, kFileHeader + 116);
            if (profileOffset < layout.headerSize || profileSize == 0) {
                parsed.problems.push_back("the color profile overlaps the header or is empty");
            } else {
                layout.required = std::max(layout.required, kFileHeader + profileOffset + profileSize);
            }
        }
    }
    return parsed;
}

// Walks a BMP's headers and, for a full walk, its RLE data. See structure_walk.hpp for what a walk reports.
class BmpWalker {
public:
    BmpWalker(IContentReader& content, Depth depth) : content_(content), depth_(depth) {}

    Result<Walk> run();

private:
    Status checkRle(const Layout& layout);

    IContentReader& content_;
    Depth depth_;
    Walk walk_;
};

Status BmpWalker::checkRle(const Layout& layout) {
    detail::SequentialReader reader(content_, layout.pixelOffset, layout.pixelOffset + layout.pixelBytes);
    const auto next = [&](std::uint8_t& value) -> Result<bool> {
        Result<std::optional<std::uint8_t>> byte = reader.next();
        if (!byte.ok()) {
            return byte.error();
        }
        if (!byte->has_value()) {
            return false;
        }
        value = **byte;
        return true;
    };
    const std::uint64_t at = layout.pixelOffset;
    const auto missingEnd = [&]() {
        walk_.noteProblem(at, "the RLE data has no end-of-bitmap code");
        return success();
    };
    const auto beyondLastRow = [&]() {
        walk_.noteProblem(at, "the RLE data reaches beyond the last row");
        return success();
    };
    std::uint64_t row = 0;
    for (;;) {
        std::uint8_t count = 0;
        std::uint8_t code = 0;
        Result<bool> first = next(count);
        if (!first.ok()) {
            return first.error();
        }
        Result<bool> second = *first ? next(code) : Result<bool>(false);
        if (!second.ok()) {
            return second.error();
        }
        if (!*second) {
            return missingEnd();
        }
        if (count != 0) {
            // An encoded run of `count` pixels.
            if (row >= layout.height) {
                return beyondLastRow();
            }
            continue;
        }
        switch (code) {
        case 0:  // end of line
            if (++row > layout.height) {
                return beyondLastRow();
            }
            break;
        case 1:  // end of bitmap
            return success();
        case 2: {  // delta
            std::uint8_t dx = 0;
            std::uint8_t dy = 0;
            Result<bool> x = next(dx);
            if (!x.ok()) {
                return x.error();
            }
            Result<bool> y = *x ? next(dy) : Result<bool>(false);
            if (!y.ok()) {
                return y.error();
            }
            if (!*y) {
                return missingEnd();
            }
            row += dy;
            if (row > layout.height) {
                return beyondLastRow();
            }
            break;
        }
        default: {  // an absolute run of `code` pixels, padded to 16 bits
            if (row >= layout.height) {
                return beyondLastRow();
            }
            std::uint64_t bytes = layout.rle4 ? (code + 1U) / 2 : code;
            bytes += bytes & 1;
            if (!reader.skip(bytes)) {
                return missingEnd();
            }
            break;
        }
        }
    }
}

Result<Walk> BmpWalker::run() {
    const std::uint64_t size = content_.size();
    Result<std::optional<std::span<const std::byte>>> start = detail::readIfAvailable(content_, 0, kFileHeader + 4);
    if (!start.ok()) {
        return start.error();
    }
    if (!start->has_value()) {
        if (size >= 2) {
            Result<std::span<const std::byte>> magic = content_.read(0, 2);
            if (!magic.ok()) {
                return magic.error();
            }
            if (loadU8(*magic, 0) != 'B' || loadU8(*magic, 1) != 'M') {
                return walk_.finish(WalkStatus::Broken, 0, "no BMP file header");
            }
        }
        return walk_.finish(WalkStatus::Truncated, 0, "the data ends inside the file header");
    }
    if (loadU8(**start, 0) != 'B' || loadU8(**start, 1) != 'M') {
        return walk_.finish(WalkStatus::Broken, 0, "no BMP file header");
    }
    const std::uint32_t headerSize = loadLe32(**start, kFileHeader);
    if (!isKnownHeaderSize(headerSize)) {
        return walk_.finish(WalkStatus::Broken, 0, "unknown DIB header size " + std::to_string(headerSize));
    }
    Result<std::optional<std::span<const std::byte>>> headers =
        detail::readIfAvailable(content_, 0, kFileHeader + headerSize);
    if (!headers.ok()) {
        return headers.error();
    }
    if (!headers->has_value()) {
        return walk_.finish(WalkStatus::Truncated, 0, "the data ends inside the DIB header");
    }
    const Parsed parsed = parse(**headers);
    if (!parsed.fatal.empty()) {
        return walk_.finish(WalkStatus::Broken, 0, parsed.fatal);
    }
    const Layout& layout = parsed.layout;
    for (const std::string& problem : parsed.problems) {
        walk_.noteProblem(kFileHeader, problem);
    }

    std::uint64_t end = layout.required;
    std::string origin;
    if (layout.fileSize >= layout.required) {
        end = layout.fileSize;
        origin = "the file size field";
    } else if (layout.fileSize == 0) {
        origin = "the headers (the file size field is 0)";
    } else {
        walk_.noteProblem(0, "the file size field (" + std::to_string(layout.fileSize) +
                                 ") is smaller than the headers need (" + std::to_string(layout.required) + ")");
        origin = "the headers";
    }
    if (end > size) {
        return walk_.finish(WalkStatus::Truncated, size,
                            "the data ends before the " + std::to_string(end) + " bytes " + origin + " gives");
    }
    if (depth_ == Depth::Full && layout.rle) {
        if (Status checked = checkRle(layout); !checked.ok()) {
            return checked.error();
        }
    }
    std::string summary = headerName(layout.headerSize) + ", " + std::to_string(layout.width) + "x" +
                          std::to_string(layout.height) + (layout.topDown ? " top-down" : "") + ", " +
                          std::to_string(layout.bitCount) + " bits per pixel, compression " +
                          std::to_string(layout.compression) + ", size from " + origin;
    return walk_.finish(WalkStatus::Complete, end, std::move(summary));
}

Result<Walk> walkBmp(IContentReader& content, Depth depth) {
    return BmpWalker(content, depth).run();
}

}  // namespace

BmpFormat::BmpFormat() {
    descriptor_.id = "bmp";
    descriptor_.name = "BMP image";
    descriptor_.extension = "bmp";
    descriptor_.signatures = {carving::textSignature("BMP file header", "BM")};
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::SizeField;
}

HeaderCheck BmpFormat::checkHeader(std::span<const std::byte> header) const {
    if (header.size() < kFileHeader + 4 || loadU8(header, 0) != 'B' || loadU8(header, 1) != 'M') {
        return HeaderCheck::reject("no BMP file header");
    }
    const std::uint32_t headerSize = loadLe32(header, kFileHeader);
    if (!isKnownHeaderSize(headerSize)) {
        return HeaderCheck::reject("unknown DIB header size " + std::to_string(headerSize));
    }
    if (header.size() < kFileHeader + headerSize) {
        // Too little data for the whole DIB header: the carve will be truncated anyway.
        return HeaderCheck::accept();
    }
    const Parsed parsed = parse(header.first(kFileHeader + headerSize));
    if (!parsed.fatal.empty()) {
        return HeaderCheck::reject(parsed.fatal);
    }
    if (!parsed.implausible.empty()) {
        return HeaderCheck::reject(parsed.implausible);
    }
    return HeaderCheck::accept();
}

Result<EndDetection> BmpFormat::findEnd(IContentReader& content) const {
    Result<Walk> walk = walkBmp(content, Depth::Layout);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::endOf(*walk, content.size());
}

Result<ValidationResult> BmpFormat::validate(IContentReader& content) const {
    Result<Walk> walk = walkBmp(content, Depth::Full);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::verdictOf(*walk, content.size());
}

}  // namespace recovery::formats
