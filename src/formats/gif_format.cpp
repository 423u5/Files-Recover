#include "formats/gif_format.hpp"

#include "recovery/byte_order.hpp"
#include "structure_walk.hpp"

#include <optional>
#include <string>

namespace recovery::formats {

namespace {

using carving::EndDetection;
using carving::HeaderCheck;
using carving::IContentReader;
using carving::ValidationResult;
using detail::hexByte;
using detail::Walk;
using detail::WalkStatus;

constexpr std::uint8_t kImage = 0x2C;
constexpr std::uint8_t kExtension = 0x21;
constexpr std::uint8_t kTrailer = 0x3B;
constexpr std::uint8_t kGraphicControl = 0xF9;
constexpr std::uint8_t kPlainText = 0x01;
constexpr std::uint8_t kApplication = 0xFF;
constexpr std::size_t kScreenEnd = 13;  // header and logical screen descriptor
// LZW minimum code sizes decoders accept (the specification uses 2 to 8;
// browsers decode 1 to 11, the largest that leaves room for 12-bit codes).
constexpr std::uint8_t kMinCodeSize = 1;
constexpr std::uint8_t kMaxCodeSize = 11;

bool hasSignature(std::span<const std::byte> bytes) noexcept {
    constexpr std::string_view kGif = "GIF8";
    if (bytes.size() < 6) {
        return false;
    }
    for (std::size_t i = 0; i < kGif.size(); ++i) {
        if (bytes[i] != static_cast<std::byte>(kGif[i])) {
            return false;
        }
    }
    const auto version = static_cast<std::uint8_t>(bytes[4]);
    return (version == '7' || version == '9') && bytes[5] == std::byte{'a'};
}

// Bytes of the color table a packed field announces (flag in bit 7, size in bits 0-2).
std::uint64_t colorTableSize(std::uint8_t packed) noexcept {
    return (packed & 0x80) != 0 ? 3ULL << ((packed & 0x07) + 1) : 0;
}

// Walks a GIF from its header to the trailer. See structure_walk.hpp for what a walk reports.
class GifWalker {
public:
    explicit GifWalker(IContentReader& content) : content_(content) {}

    Result<Walk> run();

private:
    struct SubBlocks {
        std::uint64_t end = 0;
        // Size of the first sub-block (0: the chain is empty).
        std::uint8_t firstSize = 0;
    };

    Result<std::optional<std::uint8_t>> byteAt(std::uint64_t offset);
    // Follows the chain of data sub-blocks at `offset` to its terminator;
    // nullopt when the walk ended. `block` is where the enclosing block starts.
    Result<std::optional<SubBlocks>> subBlocks(std::uint64_t offset, std::uint64_t block);

    IContentReader& content_;
    Walk walk_;
    std::uint64_t images_ = 0;
    std::uint64_t extensions_ = 0;
};

Result<std::optional<std::uint8_t>> GifWalker::byteAt(std::uint64_t offset) {
    Result<std::optional<std::span<const std::byte>>> bytes = detail::readIfAvailable(content_, offset, 1);
    if (!bytes.ok()) {
        return bytes.error();
    }
    if (!bytes->has_value()) {
        return std::optional<std::uint8_t>{};
    }
    return std::optional<std::uint8_t>(static_cast<std::uint8_t>((**bytes)[0]));
}

Result<std::optional<GifWalker::SubBlocks>> GifWalker::subBlocks(std::uint64_t offset, std::uint64_t block) {
    SubBlocks blocks;
    bool first = true;
    for (;;) {
        Result<std::optional<std::uint8_t>> size = byteAt(offset);
        if (!size.ok()) {
            return size.error();
        }
        if (!size->has_value()) {
            walk_.finish(WalkStatus::Truncated, block, "the data ends inside a chain of data sub-blocks");
            return std::optional<SubBlocks>{};
        }
        if (first) {
            blocks.firstSize = **size;
            first = false;
        }
        offset += 1 + static_cast<std::uint64_t>(**size);
        if (**size == 0) {
            blocks.end = offset;
            return std::optional<SubBlocks>(blocks);
        }
    }
}

Result<Walk> GifWalker::run() {
    Result<std::optional<std::span<const std::byte>>> screen = detail::readIfAvailable(content_, 0, kScreenEnd);
    if (!screen.ok()) {
        return screen.error();
    }
    if (!screen->has_value()) {
        if (content_.size() >= 6) {
            Result<std::span<const std::byte>> start = content_.read(0, 6);
            if (!start.ok()) {
                return start.error();
            }
            if (!hasSignature(*start)) {
                return walk_.finish(WalkStatus::Broken, 0, "no GIF signature");
            }
        }
        return walk_.finish(WalkStatus::Truncated, 0, "the data ends inside the logical screen descriptor");
    }
    if (!hasSignature(**screen)) {
        return walk_.finish(WalkStatus::Broken, 0, "no GIF signature");
    }
    const std::uint16_t width = loadLe16(**screen, 6);
    const std::uint16_t height = loadLe16(**screen, 8);
    std::uint64_t position = kScreenEnd + colorTableSize(loadU8(**screen, 10));
    if (position > content_.size()) {
        return walk_.finish(WalkStatus::Truncated, kScreenEnd, "the data ends inside the global color table");
    }
    for (;;) {
        Result<std::optional<std::uint8_t>> introducer = byteAt(position);
        if (!introducer.ok()) {
            return introducer.error();
        }
        if (!introducer->has_value()) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends before the trailer");
        }
        const std::uint64_t block = position;
        switch (**introducer) {
        case kImage: {
            Result<std::optional<std::span<const std::byte>>> descriptor =
                detail::readIfAvailable(content_, block, 10);
            if (!descriptor.ok()) {
                return descriptor.error();
            }
            if (!descriptor->has_value()) {
                return walk_.finish(WalkStatus::Truncated, block, "the data ends inside an image descriptor");
            }
            const std::uint64_t codeSizeOffset = block + 10 + colorTableSize(loadU8(**descriptor, 9));
            Result<std::optional<std::uint8_t>> codeSize = byteAt(codeSizeOffset);
            if (!codeSize.ok()) {
                return codeSize.error();
            }
            if (!codeSize->has_value()) {
                return walk_.finish(WalkStatus::Truncated, block, "the data ends inside an image's header");
            }
            if (**codeSize < kMinCodeSize || **codeSize > kMaxCodeSize) {
                walk_.noteProblem(block, "LZW minimum code size " + std::to_string(**codeSize));
            }
            Result<std::optional<SubBlocks>> data = subBlocks(codeSizeOffset + 1, block);
            if (!data.ok()) {
                return data.error();
            }
            if (!data->has_value()) {
                return walk_;
            }
            if ((**data).firstSize == 0) {
                walk_.noteProblem(block, "an image without data");
            }
            ++images_;
            position = (**data).end;
            break;
        }
        case kExtension: {
            Result<std::optional<std::uint8_t>> label = byteAt(block + 1);
            if (!label.ok()) {
                return label.error();
            }
            if (!label->has_value()) {
                return walk_.finish(WalkStatus::Truncated, block, "the data ends inside an extension");
            }
            Result<std::optional<SubBlocks>> data = subBlocks(block + 2, block);
            if (!data.ok()) {
                return data.error();
            }
            if (!data->has_value()) {
                return walk_;
            }
            const std::uint8_t firstSize = (**data).firstSize;
            if ((**label == kGraphicControl && firstSize != 4) || (**label == kApplication && firstSize != 11) ||
                (**label == kPlainText && firstSize != 12)) {
                walk_.noteProblem(block, "extension " + hexByte(**label) + " has a first block of " +
                                             std::to_string(firstSize) + " bytes");
            }
            ++extensions_;
            position = (**data).end;
            break;
        }
        case kTrailer:
            if (images_ == 0) {
                walk_.noteProblem(block, "the trailer comes before any image");
            }
            return walk_.finish(WalkStatus::Complete, block + 1,
                                std::to_string(width) + "x" + std::to_string(height) + ", " +
                                    std::to_string(images_) + (images_ == 1 ? " image, " : " images, ") +
                                    std::to_string(extensions_) + (extensions_ == 1 ? " extension" : " extensions") +
                                    ", trailer");
        default:
            return walk_.finish(WalkStatus::Broken, block, "invalid block introducer " + hexByte(**introducer));
        }
    }
}

Result<Walk> walkGif(IContentReader& content) {
    return GifWalker(content).run();
}

}  // namespace

GifFormat::GifFormat() {
    descriptor_.id = "gif";
    descriptor_.name = "GIF image";
    descriptor_.extension = "gif";
    descriptor_.signatures = {carving::textSignature("GIF89a header", "GIF89a"),
                              carving::textSignature("GIF87a header", "GIF87a")};
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::StructureWalk;
}

HeaderCheck GifFormat::checkHeader(std::span<const std::byte> header) const {
    if (!hasSignature(header)) {
        return HeaderCheck::reject("no GIF signature");
    }
    if (header.size() < kScreenEnd) {
        return HeaderCheck::accept();
    }
    const std::uint64_t first = kScreenEnd + colorTableSize(loadU8(header, 10));
    if (first < header.size()) {
        const std::uint8_t introducer = loadU8(header, static_cast<std::size_t>(first));
        if (introducer != kImage && introducer != kExtension && introducer != kTrailer) {
            return HeaderCheck::reject("invalid first block introducer " + hexByte(introducer));
        }
    }
    return HeaderCheck::accept();
}

Result<EndDetection> GifFormat::findEnd(IContentReader& content) const {
    Result<Walk> walk = walkGif(content);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::endOf(*walk, content.size());
}

Result<ValidationResult> GifFormat::validate(IContentReader& content) const {
    Result<Walk> walk = walkGif(content);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::verdictOf(*walk, content.size());
}

}  // namespace recovery::formats
