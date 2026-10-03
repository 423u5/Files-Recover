#include "formats/webp_format.hpp"

#include "recovery/byte_order.hpp"
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
using detail::Walk;
using detail::WalkStatus;

constexpr std::uint8_t kAnimationFlag = 0x02;
constexpr std::size_t kRiffHeader = 12;
constexpr std::size_t kChunkHeader = 8;
constexpr std::size_t kVp8xPayload = 10;
constexpr std::size_t kVp8FrameHeader = 10;
constexpr std::size_t kVp8lHeader = 5;
constexpr std::size_t kFrameHeader = 16;
constexpr std::uint8_t kVp8lSignature = 0x2F;
constexpr std::uint64_t kMaxCanvasArea = 0xFFFFFFFFULL;

using FourCc = std::array<std::byte, 4>;

FourCc fourCcAt(std::span<const std::byte> bytes, std::size_t offset) {
    FourCc code{};
    for (std::size_t i = 0; i < code.size(); ++i) {
        code[i] = bytes[offset + i];
    }
    return code;
}

bool is(const FourCc& code, std::string_view name) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        if (code[i] != static_cast<std::byte>(name[i])) {
            return false;
        }
    }
    return true;
}

bool isPrintable(const FourCc& code) noexcept {
    for (const std::byte byte : code) {
        const auto c = static_cast<std::uint8_t>(byte);
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

bool isImageData(const FourCc& code) noexcept {
    return is(code, "VP8 ") || is(code, "VP8L");
}

std::string name(const FourCc& code) {
    return detail::fourCcText(code);
}

std::uint32_t loadLe24(std::span<const std::byte> data, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(loadU8(data, offset)) |
           (static_cast<std::uint32_t>(loadU8(data, offset + 1)) << 8) |
           (static_cast<std::uint32_t>(loadU8(data, offset + 2)) << 16);
}

bool isRiffWebp(std::span<const std::byte> header) noexcept {
    constexpr std::string_view kRiff = "RIFF";
    constexpr std::string_view kWebp = "WEBP";
    if (header.size() < kRiffHeader) {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (header[i] != static_cast<std::byte>(kRiff[i]) || header[8 + i] != static_cast<std::byte>(kWebp[i])) {
            return false;
        }
    }
    return true;
}

struct Chunk {
    FourCc type{};
    std::uint32_t size = 0;
    // Offset of the chunk header, and of its payload.
    std::uint64_t offset = 0;
    std::uint64_t payload = 0;
};

struct Bitstream {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool lossless = false;
};

struct Canvas {
    std::uint8_t flags = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool animated() const noexcept { return (flags & kAnimationFlag) != 0; }
};

// Walks a WebP's chunks to the end of its RIFF data. See structure_walk.hpp for what a walk reports.
class WebpWalker {
public:
    explicit WebpWalker(IContentReader& content) : content_(content) {}

    Result<Walk> run();

private:
    Status visitChunk(const Chunk& chunk, std::size_t index);
    Status visitFrame(const Chunk& chunk);
    Status visitAlpha(const Chunk& chunk);
    // The dimensions in a VP8 or VP8L bitstream's header; nothing (and a problem) when it is damaged.
    Result<std::optional<Bitstream>> readBitstream(const Chunk& chunk);

    [[nodiscard]] bool animated() const noexcept { return canvas_.has_value() && canvas_->animated(); }
    std::string summary() const;

    IContentReader& content_;
    Walk walk_;
    std::optional<Canvas> canvas_;
    std::optional<Bitstream> image_;
    std::uint64_t images_ = 0;
    std::uint64_t frames_ = 0;
    bool animationHeader_ = false;
    // An ALPH chunk waiting for the VP8 chunk it belongs to.
    std::optional<std::uint64_t> pendingAlpha_;
};

Result<std::optional<Bitstream>> WebpWalker::readBitstream(const Chunk& chunk) {
    const bool lossless = is(chunk.type, "VP8L");
    const std::size_t headerSize = lossless ? kVp8lHeader : kVp8FrameHeader;
    if (chunk.size < headerSize) {
        walk_.noteProblem(chunk.offset, name(chunk.type) + " data is too short for its header");
        return std::optional<Bitstream>{};
    }
    Result<std::span<const std::byte>> header = content_.read(chunk.payload, headerSize);
    if (!header.ok()) {
        return header.error();
    }
    Bitstream bitstream;
    bitstream.lossless = lossless;
    if (lossless) {
        const std::uint32_t bits = loadLe32(*header, 1);
        bitstream.width = (bits & 0x3FFF) + 1;
        bitstream.height = ((bits >> 14) & 0x3FFF) + 1;
        if (loadU8(*header, 0) != kVp8lSignature || (bits >> 29) != 0) {
            walk_.noteProblem(chunk.offset, "VP8L data has no valid signature and version");
            return std::optional<Bitstream>{};
        }
        return std::optional<Bitstream>(bitstream);
    }
    const std::uint32_t tag = loadLe24(*header, 0);
    const bool keyFrame = (tag & 1) == 0;
    const std::uint32_t version = (tag >> 1) & 7;
    const bool shown = ((tag >> 4) & 1) != 0;
    const std::uint32_t firstPartition = tag >> 5;
    bitstream.width = loadLe16(*header, 6) & 0x3FFF;
    bitstream.height = loadLe16(*header, 8) & 0x3FFF;
    if (!keyFrame || version > 3 || !shown) {
        walk_.noteProblem(chunk.offset, "VP8 data is not a shown key frame of version 0 to 3");
        return std::optional<Bitstream>{};
    }
    if (loadU8(*header, 3) != 0x9D || loadU8(*header, 4) != 0x01 || loadU8(*header, 5) != 0x2A) {
        walk_.noteProblem(chunk.offset, "VP8 data has no start code");
        return std::optional<Bitstream>{};
    }
    if (bitstream.width == 0 || bitstream.height == 0) {
        walk_.noteProblem(chunk.offset, "VP8 frame of width or height 0");
        return std::optional<Bitstream>{};
    }
    if (firstPartition > chunk.size - kVp8FrameHeader) {
        walk_.noteProblem(chunk.offset, "VP8 first partition runs beyond its chunk");
    }
    return std::optional<Bitstream>(bitstream);
}

Status WebpWalker::visitAlpha(const Chunk& chunk) {
    if (!canvas_.has_value() || animated()) {
        walk_.noteProblem(chunk.offset, "ALPH outside an extended still image");
    }
    pendingAlpha_ = chunk.offset;
    if (chunk.size == 0) {
        walk_.noteProblem(chunk.offset, "ALPH is empty");
        return success();
    }
    Result<std::span<const std::byte>> header = content_.read(chunk.payload, 1);
    if (!header.ok()) {
        return header.error();
    }
    const std::uint8_t info = loadU8(*header, 0);
    const auto compression = static_cast<std::uint8_t>(info & 0x03);
    const auto preprocessing = static_cast<std::uint8_t>((info >> 4) & 0x03);
    if (compression > 1 || preprocessing > 1 || (info >> 6) != 0) {
        walk_.noteProblem(chunk.offset, "ALPH header names an unknown method");
    } else if (compression == 0 && canvas_.has_value() &&
               chunk.size - 1ULL < std::uint64_t{canvas_->width} * canvas_->height) {
        walk_.noteProblem(chunk.offset, "uncompressed ALPH data is smaller than the canvas");
    }
    return success();
}

Status WebpWalker::visitFrame(const Chunk& chunk) {
    if (chunk.size < kFrameHeader) {
        walk_.noteProblem(chunk.offset, "ANMF is too short");
        return success();
    }
    Result<std::span<const std::byte>> header = content_.read(chunk.payload, kFrameHeader);
    if (!header.ok()) {
        return header.error();
    }
    const std::uint64_t x = 2ULL * loadLe24(*header, 0);
    const std::uint64_t y = 2ULL * loadLe24(*header, 3);
    const std::uint32_t width = loadLe24(*header, 6) + 1;
    const std::uint32_t height = loadLe24(*header, 9) + 1;
    if (canvas_.has_value() && (x + width > canvas_->width || y + height > canvas_->height)) {
        walk_.noteProblem(chunk.offset, "an animation frame reaches beyond the canvas");
    }
    // The frame's own chunks: optional ALPH, then VP8 or VP8L, and unknown chunks.
    const std::uint64_t end = chunk.payload + chunk.size;
    std::uint64_t position = chunk.payload + kFrameHeader;
    std::uint64_t images = 0;
    bool alpha = false;
    while (position < end) {
        if (end - position < kChunkHeader) {
            walk_.noteProblem(chunk.offset, "an animation frame ends inside a chunk header");
            return success();
        }
        Result<std::span<const std::byte>> bytes = content_.read(position, kChunkHeader);
        if (!bytes.ok()) {
            return bytes.error();
        }
        Chunk inner{fourCcAt(*bytes, 0), loadLe32(*bytes, 4), position, position + kChunkHeader};
        if (!isPrintable(inner.type) || inner.size > end - inner.payload) {
            walk_.noteProblem(chunk.offset, "an animation frame's chunks do not fit in it");
            return success();
        }
        if (isImageData(inner.type)) {
            ++images;
            if (alpha && is(inner.type, "VP8L")) {
                walk_.noteProblem(inner.offset, "ALPH before a lossless image");
            }
            Result<std::optional<Bitstream>> image = readBitstream(inner);
            if (!image.ok()) {
                return image.error();
            }
            if (image->has_value() && ((**image).width != width || (**image).height != height)) {
                walk_.noteProblem(inner.offset, "a frame's image does not have the frame's size");
            }
        } else if (is(inner.type, "ALPH")) {
            alpha = true;
        }
        position = std::min(end, inner.payload + inner.size + (inner.size & 1));
    }
    if (images != 1) {
        walk_.noteProblem(chunk.offset, "an animation frame with " + std::to_string(images) + " images");
    }
    return success();
}

Status WebpWalker::visitChunk(const Chunk& chunk, std::size_t index) {
    const FourCc& type = chunk.type;
    if (index == 0) {
        if (is(type, "VP8X")) {
            if (chunk.size < kVp8xPayload) {
                walk_.noteProblem(chunk.offset, "VP8X is too short");
            } else {
                Result<std::span<const std::byte>> payload = content_.read(chunk.payload, kVp8xPayload);
                if (!payload.ok()) {
                    return payload.error();
                }
                Canvas canvas;
                canvas.flags = loadU8(*payload, 0);
                canvas.width = loadLe24(*payload, 4) + 1;
                canvas.height = loadLe24(*payload, 7) + 1;
                if (std::uint64_t{canvas.width} * canvas.height > kMaxCanvasArea) {
                    walk_.noteProblem(chunk.offset, "the canvas has more than 2^32-1 pixels");
                }
                canvas_ = canvas;
            }
            return success();
        }
        if (!isImageData(type)) {
            walk_.noteProblem(chunk.offset, "the first chunk is " + name(type) + ", not VP8, VP8L or VP8X");
        }
    } else if (is(type, "VP8X")) {
        walk_.noteProblem(chunk.offset, "VP8X is not the first chunk");
    }
    if (pendingAlpha_.has_value() && !is(type, "VP8 ")) {
        walk_.noteProblem(*pendingAlpha_, "ALPH is not followed by VP8 data");
        pendingAlpha_.reset();
    }

    if (isImageData(type)) {
        if (animated()) {
            walk_.noteProblem(chunk.offset, "image data outside the frames of an animation");
        }
        if (++images_ > 1) {
            walk_.noteProblem(chunk.offset, "a second image");
        }
        pendingAlpha_.reset();
        Result<std::optional<Bitstream>> image = readBitstream(chunk);
        if (!image.ok()) {
            return image.error();
        }
        if (image->has_value()) {
            if (canvas_.has_value() && !animated() &&
                ((**image).width != canvas_->width || (**image).height != canvas_->height)) {
                walk_.noteProblem(chunk.offset, "the image does not have the canvas's size");
            }
            if (!image_.has_value()) {
                image_ = **image;
            }
        }
    } else if (is(type, "ALPH")) {
        return visitAlpha(chunk);
    } else if (is(type, "ANIM")) {
        if (!animated()) {
            walk_.noteProblem(chunk.offset, "ANIM in a still image");
        }
        if (chunk.size < 6) {
            walk_.noteProblem(chunk.offset, "ANIM is too short");
        }
        animationHeader_ = true;
    } else if (is(type, "ANMF")) {
        if (!animated()) {
            walk_.noteProblem(chunk.offset, "ANMF in a still image");
        } else if (!animationHeader_) {
            walk_.noteProblem(chunk.offset, "ANMF before ANIM");
        }
        ++frames_;
        return visitFrame(chunk);
    }
    // ICCP, EXIF, XMP and unknown chunks carry nothing the structure depends on.
    return success();
}

std::string WebpWalker::summary() const {
    if (animated()) {
        return "VP8X " + std::to_string(canvas_->width) + "x" + std::to_string(canvas_->height) + ", animation of " +
               std::to_string(frames_) + (frames_ == 1 ? " frame" : " frames");
    }
    std::string text = canvas_.has_value() ? "VP8X, " : "";
    if (image_.has_value()) {
        text += std::string(image_->lossless ? "lossless " : "lossy ") + std::to_string(image_->width) + "x" +
                std::to_string(image_->height);
    } else {
        text += "no image";
    }
    return text;
}

Result<Walk> WebpWalker::run() {
    const std::uint64_t size = content_.size();
    Result<std::span<const std::byte>> start = content_.read(0, static_cast<std::size_t>(std::min<std::uint64_t>(
                                                                    size, kRiffHeader)));
    if (!start.ok()) {
        return start.error();
    }
    if (size < kRiffHeader) {
        const std::span<const std::byte> riff = *start;
        constexpr std::string_view kRiff = "RIFF";
        for (std::size_t i = 0; i < std::min<std::size_t>(riff.size(), 4); ++i) {
            if (riff[i] != static_cast<std::byte>(kRiff[i])) {
                return walk_.finish(WalkStatus::Broken, 0, "no RIFF WEBP header");
            }
        }
        return walk_.finish(WalkStatus::Truncated, 0, "the data ends inside the RIFF header");
    }
    if (!isRiffWebp(*start)) {
        return walk_.finish(WalkStatus::Broken, 0, "no RIFF WEBP header");
    }
    const std::uint32_t riffSize = loadLe32(*start, 4);
    if (riffSize < 4 + kChunkHeader) {
        return walk_.finish(WalkStatus::Broken, 0, "the RIFF size " + std::to_string(riffSize) +
                                                       " leaves no room for a chunk");
    }
    const std::uint64_t fileEnd = 8ULL + riffSize;
    std::uint64_t position = kRiffHeader;
    for (std::size_t index = 0; position < fileEnd; ++index) {
        if (fileEnd - position < kChunkHeader) {
            return walk_.finish(WalkStatus::Broken, position, "a chunk header crosses the end of the RIFF data");
        }
        Result<std::optional<std::span<const std::byte>>> header =
            detail::readIfAvailable(content_, position, kChunkHeader);
        if (!header.ok()) {
            return header.error();
        }
        if (!header->has_value()) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends before the next chunk");
        }
        const Chunk current{fourCcAt(**header, 0), loadLe32(**header, 4), position, position + kChunkHeader};
        if (!isPrintable(current.type)) {
            return walk_.finish(WalkStatus::Broken, position, "invalid chunk id");
        }
        if (current.size > fileEnd - current.payload) {
            return walk_.finish(WalkStatus::Broken, position,
                                "chunk " + name(current.type) + " runs beyond the RIFF data");
        }
        std::uint64_t end = current.payload + current.size + (current.size & 1);
        if (end > fileEnd) {
            walk_.noteProblem(position, "chunk " + name(current.type) + " lacks its padding byte");
            end = fileEnd;
        }
        if (end > size) {
            return walk_.finish(WalkStatus::Truncated, position,
                                "the data ends inside chunk " + name(current.type));
        }
        if (Status checked = visitChunk(current, index); !checked.ok()) {
            return checked.error();
        }
        position = end;
    }
    if (pendingAlpha_.has_value()) {
        walk_.noteProblem(*pendingAlpha_, "ALPH is not followed by VP8 data");
    }
    if (animated()) {
        if (frames_ == 0) {
            walk_.noteProblem(kRiffHeader, "an animation without frames");
        }
    } else if (images_ == 0) {
        walk_.noteProblem(kRiffHeader, "no image data");
    }
    return walk_.finish(WalkStatus::Complete, fileEnd, summary());
}

Result<Walk> walkWebp(IContentReader& content) {
    return WebpWalker(content).run();
}

}  // namespace

WebpFormat::WebpFormat() {
    descriptor_.id = "webp";
    descriptor_.name = "WebP image";
    descriptor_.extension = "webp";
    // "RIFF", any size, "WEBP".
    descriptor_.signatures = {carving::byteSignature(
        "RIFF WEBP header", {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'}, 0,
        {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF})};
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::SizeField;
}

HeaderCheck WebpFormat::checkHeader(std::span<const std::byte> header) const {
    if (!isRiffWebp(header) || header.size() < kRiffHeader + kChunkHeader) {
        return HeaderCheck::reject("no RIFF WEBP header and first chunk");
    }
    const std::uint32_t riffSize = loadLe32(header, 4);
    if (riffSize < 4 + kChunkHeader) {
        return HeaderCheck::reject("the RIFF size leaves no room for a chunk");
    }
    const FourCc type = fourCcAt(header, kRiffHeader);
    const std::uint32_t size = loadLe32(header, kRiffHeader + 4);
    if (!is(type, "VP8X") && !isImageData(type)) {
        return HeaderCheck::reject("the first chunk is not VP8, VP8L or VP8X");
    }
    if (size > riffSize - 4 - kChunkHeader) {
        return HeaderCheck::reject("the first chunk is larger than the RIFF data");
    }
    const std::size_t payload = kRiffHeader + kChunkHeader;
    if (is(type, "VP8X")) {
        if (size < kVp8xPayload) {
            return HeaderCheck::reject("VP8X is too short");
        }
    } else if (is(type, "VP8L")) {
        if (header.size() >= payload + kVp8lHeader &&
            (loadU8(header, payload) != kVp8lSignature || (loadLe32(header, payload + 1) >> 29) != 0)) {
            return HeaderCheck::reject("VP8L data has no valid signature and version");
        }
    } else if (header.size() >= payload + kVp8FrameHeader) {
        if ((loadU8(header, payload) & 1) != 0 || loadU8(header, payload + 3) != 0x9D ||
            loadU8(header, payload + 4) != 0x01 || loadU8(header, payload + 5) != 0x2A) {
            return HeaderCheck::reject("VP8 data is not a key frame with a start code");
        }
    }
    return HeaderCheck::accept();
}

Result<EndDetection> WebpFormat::findEnd(IContentReader& content) const {
    Result<Walk> walk = walkWebp(content);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::endOf(*walk, content.size());
}

Result<ValidationResult> WebpFormat::validate(IContentReader& content) const {
    Result<Walk> walk = walkWebp(content);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::verdictOf(*walk, content.size());
}

}  // namespace recovery::formats
