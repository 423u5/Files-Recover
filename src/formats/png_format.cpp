#include "formats/png_format.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/crc32.hpp"
#include "structure_walk.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace recovery::formats {

namespace {

using carving::EndDetection;
using carving::HeaderCheck;
using carving::IContentReader;
using carving::ValidationResult;
using detail::Depth;
using detail::Walk;
using detail::WalkStatus;

constexpr std::array<std::uint8_t, 8> kSignature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::uint32_t kMaxChunkLength = 0x7FFFFFFF;
constexpr std::size_t kIhdrLength = 13;
// Chunk data is read in pieces of this size for its CRC.
constexpr std::size_t kCrcChunk = 64 * kKiB;

enum class ColorType : std::uint8_t { Gray = 0, Rgb = 2, Palette = 3, GrayAlpha = 4, Rgba = 6 };

using ChunkType = std::array<std::byte, 4>;

bool is(const ChunkType& type, std::string_view name) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        if (type[i] != static_cast<std::byte>(name[i])) {
            return false;
        }
    }
    return true;
}

std::string typeText(const ChunkType& type) {
    return detail::fourCcText(type);
}

bool hasSignature(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kSignature.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kSignature.size(); ++i) {
        if (bytes[i] != static_cast<std::byte>(kSignature[i])) {
            return false;
        }
    }
    return true;
}

struct Ihdr {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bitDepth = 0;
    std::uint8_t colorType = 0;
    std::uint8_t interlace = 0;
};

bool bitDepthAllowed(std::uint8_t colorType, std::uint8_t depth) noexcept {
    switch (static_cast<ColorType>(colorType)) {
    case ColorType::Gray:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case ColorType::Palette:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case ColorType::Rgb:
    case ColorType::GrayAlpha:
    case ColorType::Rgba:
        return depth == 8 || depth == 16;
    }
    return false;
}

// Reads IHDR's 13 data bytes; the reason they are invalid, or nothing.
std::optional<std::string> parseIhdr(std::span<const std::byte> data, Ihdr& ihdr) {
    ihdr.width = loadBe32(data, 0);
    ihdr.height = loadBe32(data, 4);
    ihdr.bitDepth = loadU8(data, 8);
    ihdr.colorType = loadU8(data, 9);
    ihdr.interlace = loadU8(data, 12);
    if (ihdr.width == 0 || ihdr.height == 0 || ihdr.width > kMaxChunkLength || ihdr.height > kMaxChunkLength) {
        return "IHDR dimensions must lie between 1 and 2^31-1";
    }
    if (!bitDepthAllowed(ihdr.colorType, ihdr.bitDepth)) {
        return "IHDR color type " + std::to_string(ihdr.colorType) + " with bit depth " +
               std::to_string(ihdr.bitDepth);
    }
    if (loadU8(data, 10) != 0 || loadU8(data, 11) != 0) {
        return "IHDR compression or filter method is not 0";
    }
    if (ihdr.interlace > 1) {
        return "IHDR interlace method " + std::to_string(ihdr.interlace);
    }
    return std::nullopt;
}

// Walks a PNG from its signature to IEND. See structure_walk.hpp for what a walk reports.
class PngWalker {
public:
    PngWalker(IContentReader& content, Depth depth) : content_(content), depth_(depth) {}

    Result<Walk> run();

private:
    // CRC-32 of the chunk at `offset` (type and data).
    Result<std::uint32_t> chunkCrc(std::uint64_t offset, std::uint32_t length);
    Status chunkContents(std::uint64_t offset, const ChunkType& type, std::uint32_t length);
    Status checkZlibHeader(std::uint64_t offset, std::uint32_t length);

    IContentReader& content_;
    Depth depth_;
    Walk walk_;
    std::optional<Ihdr> ihdr_;
    std::uint64_t chunks_ = 0;
    bool palette_ = false;
    bool imageData_ = false;
    // A chunk other than IDAT followed the image data.
    bool imageDataEnded_ = false;
    // Bytes of the zlib stream seen so far (its 2-byte header can span IDATs).
    std::array<std::uint8_t, 2> zlibHeader_{};
    std::size_t zlibHeaderBytes_ = 0;
};

Result<std::uint32_t> PngWalker::chunkCrc(std::uint64_t offset, std::uint32_t length) {
    std::uint32_t crc = 0;
    const std::uint64_t end = offset + 8 + length;
    for (std::uint64_t position = offset + 4; position < end;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(kCrcChunk, end - position));
        Result<std::span<const std::byte>> piece = content_.read(position, count);
        if (!piece.ok()) {
            return piece.error();
        }
        crc = crc32Update(crc, *piece);
        position += count;
    }
    return crc;
}

Status PngWalker::checkZlibHeader(std::uint64_t offset, std::uint32_t length) {
    const std::size_t wanted = std::min<std::size_t>(zlibHeader_.size() - zlibHeaderBytes_, length);
    if (wanted == 0) {
        return success();
    }
    Result<std::span<const std::byte>> bytes = content_.read(offset + 8, wanted);
    if (!bytes.ok()) {
        return bytes.error();
    }
    for (const std::byte byte : *bytes) {
        zlibHeader_[zlibHeaderBytes_++] = static_cast<std::uint8_t>(byte);
    }
    if (zlibHeaderBytes_ == zlibHeader_.size()) {
        const std::uint8_t cmf = zlibHeader_[0];
        const std::uint8_t flg = zlibHeader_[1];
        // Deflate with a window of at most 32 KiB, a valid check value and no preset dictionary.
        const bool valid = (cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && ((cmf << 8) | flg) % 31 == 0 && (flg & 0x20) == 0;
        if (!valid) {
            walk_.noteProblem(offset, "the image data does not start a zlib stream");
        }
    }
    return success();
}

Status PngWalker::chunkContents(std::uint64_t offset, const ChunkType& type, std::uint32_t length) {
    const bool first = chunks_ == 0;
    if (first && !is(type, "IHDR")) {
        walk_.noteProblem(offset, "the first chunk is " + typeText(type) + ", not IHDR");
    }
    if (!is(type, "IDAT") && imageData_) {
        imageDataEnded_ = true;
    }
    if (is(type, "IHDR")) {
        if (!first) {
            walk_.noteProblem(offset, "IHDR is not the first chunk");
        }
        if (length != kIhdrLength) {
            walk_.noteProblem(offset, "IHDR length is " + std::to_string(length) + ", not 13");
            return success();
        }
        Result<std::span<const std::byte>> data = content_.read(offset + 8, kIhdrLength);
        if (!data.ok()) {
            return data.error();
        }
        Ihdr ihdr;
        if (std::optional<std::string> invalid = parseIhdr(*data, ihdr); invalid.has_value()) {
            walk_.noteProblem(offset, *invalid);
        }
        if (first) {
            ihdr_ = ihdr;
        }
    } else if (is(type, "PLTE")) {
        if (palette_ || imageData_) {
            walk_.noteProblem(offset, palette_ ? "a second PLTE chunk" : "PLTE after the image data");
        }
        palette_ = true;
        if (length == 0 || length > 768 || length % 3 != 0) {
            walk_.noteProblem(offset, "PLTE length " + std::to_string(length) + " is not 3 to 768 bytes of RGB entries");
        } else if (ihdr_.has_value()) {
            const auto colorType = static_cast<ColorType>(ihdr_->colorType);
            if (colorType == ColorType::Gray || colorType == ColorType::GrayAlpha) {
                walk_.noteProblem(offset, "PLTE in a grayscale image");
            } else if (colorType == ColorType::Palette && ihdr_->bitDepth <= 8 &&
                       length / 3 > (1U << ihdr_->bitDepth)) {
                walk_.noteProblem(offset, "PLTE has more entries than the bit depth can index");
            }
        }
    } else if (is(type, "IDAT")) {
        if (imageDataEnded_) {
            walk_.noteProblem(offset, "IDAT chunks are not consecutive");
        }
        if (!imageData_ && ihdr_.has_value() && static_cast<ColorType>(ihdr_->colorType) == ColorType::Palette &&
            !palette_) {
            walk_.noteProblem(offset, "a palette image without PLTE before its image data");
        }
        imageData_ = true;
        if (Status checked = checkZlibHeader(offset, length); !checked.ok()) {
            return checked;
        }
    } else if (is(type, "IEND")) {
        if (length != 0) {
            walk_.noteProblem(offset, "IEND has data");
        }
        if (!imageData_) {
            walk_.noteProblem(offset, "no IDAT chunk");
        } else if (zlibHeaderBytes_ < zlibHeader_.size()) {
            walk_.noteProblem(offset, "the image data is too short for a zlib stream");
        }
    } else if ((static_cast<std::uint8_t>(type[0]) & 0x20) == 0) {
        // Decoders must refuse a critical chunk they do not know.
        walk_.noteProblem(offset, "unknown critical chunk " + typeText(type));
    }
    return success();
}

Result<Walk> PngWalker::run() {
    Result<std::optional<std::span<const std::byte>>> signature = detail::readIfAvailable(content_, 0, 8);
    if (!signature.ok()) {
        return signature.error();
    }
    if (!signature->has_value()) {
        return walk_.finish(WalkStatus::Truncated, 0, "the data ends inside the PNG signature");
    }
    if (!hasSignature(**signature)) {
        return walk_.finish(WalkStatus::Broken, 0, "no PNG signature");
    }
    std::uint64_t position = 8;
    for (;;) {
        Result<std::optional<std::span<const std::byte>>> header = detail::readIfAvailable(content_, position, 8);
        if (!header.ok()) {
            return header.error();
        }
        if (!header->has_value()) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends before IEND");
        }
        const std::uint32_t length = loadBe32(**header, 0);
        ChunkType type{};
        std::copy_n((**header).begin() + 4, 4, type.begin());
        if (length > kMaxChunkLength) {
            return walk_.finish(WalkStatus::Broken, position, "chunk length beyond 2^31-1");
        }
        if (!detail::isLetterCode(type)) {
            return walk_.finish(WalkStatus::Broken, position, "invalid chunk type");
        }
        const std::uint64_t chunkEnd = position + 12 + length;
        if (chunkEnd > content_.size()) {
            return walk_.finish(WalkStatus::Truncated, position,
                                "the data ends inside chunk " + typeText(type));
        }
        // The CRC first: when it fails, the chunk's fields are damaged too.
        if (depth_ == Depth::Full) {
            Result<std::uint32_t> crc = chunkCrc(position, length);
            if (!crc.ok()) {
                return crc.error();
            }
            Result<std::span<const std::byte>> stored = content_.read(position + 8 + length, 4);
            if (!stored.ok()) {
                return stored.error();
            }
            if (*crc != loadBe32(*stored, 0)) {
                walk_.noteProblem(position, "CRC mismatch in chunk " + typeText(type));
            }
        }
        if (Status contents = chunkContents(position, type, length); !contents.ok()) {
            return contents.error();
        }
        ++chunks_;
        if (is(type, "IEND")) {
            std::string summary = "IEND after " + std::to_string(chunks_) + " chunks";
            if (ihdr_.has_value()) {
                summary = std::to_string(ihdr_->width) + "x" + std::to_string(ihdr_->height) + ", color type " +
                          std::to_string(ihdr_->colorType) + ", bit depth " + std::to_string(ihdr_->bitDepth) +
                          (ihdr_->interlace != 0 ? ", interlaced, " : ", ") + summary;
            }
            return walk_.finish(WalkStatus::Complete, chunkEnd, summary);
        }
        position = chunkEnd;
    }
}

Result<Walk> walkPng(IContentReader& content, Depth depth) {
    return PngWalker(content, depth).run();
}

}  // namespace

PngFormat::PngFormat() {
    descriptor_.id = "png";
    descriptor_.name = "PNG image";
    descriptor_.extension = "png";
    descriptor_.signatures = {
        carving::byteSignature("PNG signature", {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})};
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::StructureWalk;
}

HeaderCheck PngFormat::checkHeader(std::span<const std::byte> header) const {
    if (header.size() < kHeaderSize || !hasSignature(header)) {
        return HeaderCheck::reject("no PNG signature and IHDR");
    }
    ChunkType type{};
    std::copy_n(header.begin() + 12, 4, type.begin());
    if (loadBe32(header, 8) != kIhdrLength || !is(type, "IHDR")) {
        return HeaderCheck::reject("the first chunk is not a 13-byte IHDR");
    }
    Ihdr ihdr;
    if (std::optional<std::string> invalid = parseIhdr(header.subspan(16, kIhdrLength), ihdr); invalid.has_value()) {
        return HeaderCheck::reject(*invalid);
    }
    if (crc32(header.subspan(12, 4 + kIhdrLength)) != loadBe32(header, 29)) {
        return HeaderCheck::reject("IHDR CRC mismatch");
    }
    return HeaderCheck::accept();
}

Result<EndDetection> PngFormat::findEnd(IContentReader& content) const {
    Result<Walk> walk = walkPng(content, Depth::Layout);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::endOf(*walk, content.size());
}

Result<ValidationResult> PngFormat::validate(IContentReader& content) const {
    Result<Walk> walk = walkPng(content, Depth::Full);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::verdictOf(*walk, content.size());
}

}  // namespace recovery::formats
