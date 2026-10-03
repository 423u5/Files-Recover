// GIF media validation: the LZW data of every image decoded code by code.
// Only the length of each dictionary string is kept, which is all that
// decides whether a code is valid and how many pixels it makes: a code must
// be in the dictionary (or the one about to be added), the code width grows
// as giflib's does, and the pixels must come out exactly width x height. A
// missing end-of-information code after the last pixel is accepted, as by
// giflib and the browsers.

#include "content_bytes.hpp"
#include "media_decoders.hpp"

#include "recovery/checked_math.hpp"

#include <array>
#include <string>
#include <vector>

namespace recovery::validation::detail {

namespace {

constexpr std::uint8_t kImage = 0x2C;
constexpr std::uint8_t kExtension = 0x21;
constexpr std::uint8_t kTrailer = 0x3B;
constexpr std::uint32_t kMinCodeSize = 1;
constexpr std::uint32_t kMaxCodeSize = 11;
constexpr std::uint32_t kMaxCodes = 4096;
constexpr unsigned kMaxCodeWidth = 12;

// The data bytes of a chain of sub-blocks ([length][data]..., then 0).
class SubBlocks {
public:
    explicit SubBlocks(ContentBytes& bytes) noexcept : bytes_(bytes) {}

    // The next data byte; false at the terminator or at the end of the content.
    bool next(std::uint8_t& value) {
        while (left_ == 0) {
            if (ended_) {
                return false;
            }
            std::uint8_t length = 0;
            if (!bytes_.next(length)) {
                cut_ = true;
                ended_ = true;
                return false;
            }
            if (length == 0) {
                ended_ = true;
                return false;
            }
            left_ = length;
        }
        if (!bytes_.next(value)) {
            cut_ = true;
            ended_ = true;
            return false;
        }
        --left_;
        return true;
    }
    // Skips to the end of the chain; false when the content ends first.
    bool skipRest() {
        while (!ended_) {
            if (left_ > 0 && !bytes_.skip(left_)) {
                cut_ = true;
                return false;
            }
            left_ = 0;
            std::uint8_t length = 0;
            if (!bytes_.next(length)) {
                cut_ = true;
                return false;
            }
            if (length == 0) {
                ended_ = true;
            } else {
                left_ = length;
            }
        }
        return true;
    }
    // The content ended inside the chain.
    [[nodiscard]] bool cut() const noexcept { return cut_; }

private:
    ContentBytes& bytes_;
    std::uint32_t left_ = 0;
    bool ended_ = false;
    bool cut_ = false;
};

struct LzwOutcome {
    enum class Kind : std::uint8_t { Done, Cut, Invalid };
    Kind kind = Kind::Done;
    std::string detail;
    std::uint64_t pixels = 0;
    bool endCode = false;
};

LzwOutcome decodeLzw(SubBlocks& data, std::uint32_t codeSize, std::uint64_t target) {
    const std::uint32_t clear = 1U << codeSize;
    const std::uint32_t end = clear + 1;
    // On the heap: 16 KiB is too much for the stack.
    std::vector<std::uint32_t> lengths(kMaxCodes, 0);
    for (std::uint32_t code = 0; code < clear; ++code) {
        lengths[code] = 1;
    }
    std::uint32_t next = clear + 2;
    unsigned width = codeSize + 1;
    std::int32_t previous = -1;
    std::uint64_t accumulator = 0;
    unsigned bits = 0;
    std::uint64_t codes = 0;
    LzwOutcome outcome;
    const auto invalid = [&](std::string detail) {
        outcome.kind = LzwOutcome::Kind::Invalid;
        outcome.detail = "code " + std::to_string(codes) + ": " + std::move(detail);
        return outcome;
    };
    for (;;) {
        while (bits < width) {
            std::uint8_t byte = 0;
            if (!data.next(byte)) {
                if (data.cut()) {
                    outcome.kind = LzwOutcome::Kind::Cut;
                    return outcome;
                }
                // The sub-blocks end without an end-of-information code.
                if (outcome.pixels != target) {
                    return invalid("the data ends after " + std::to_string(outcome.pixels) + " of " +
                                   std::to_string(target) + " pixels");
                }
                return outcome;
            }
            accumulator |= std::uint64_t{byte} << bits;
            bits += 8;
        }
        const auto code = static_cast<std::uint32_t>(accumulator & ((1U << width) - 1));
        accumulator >>= width;
        bits -= width;
        ++codes;
        if (code == clear) {
            next = clear + 2;
            width = codeSize + 1;
            previous = -1;
            continue;
        }
        if (code == end) {
            outcome.endCode = true;
            if (outcome.pixels != target) {
                return invalid("the end code comes after " + std::to_string(outcome.pixels) + " of " +
                               std::to_string(target) + " pixels");
            }
            return outcome;
        }
        std::uint32_t produced = 0;
        if (previous < 0) {
            if (code > clear) {
                return invalid("the code " + std::to_string(code) + " with no string before it");
            }
            produced = 1;
        } else if (code < next) {
            produced = lengths[code];
            if (next < kMaxCodes) {
                lengths[next++] = lengths[static_cast<std::size_t>(previous)] + 1;
            }
        } else if (code == next && next < kMaxCodes) {
            produced = lengths[static_cast<std::size_t>(previous)] + 1;
            lengths[next++] = produced;
        } else {
            return invalid("the code " + std::to_string(code) + " is not in the dictionary (" + std::to_string(next) +
                           " entries)");
        }
        outcome.pixels += produced;
        if (outcome.pixels > target) {
            return invalid("the data holds more than the image's " + std::to_string(target) + " pixels");
        }
        if (next == (1U << width) && width < kMaxCodeWidth) {
            ++width;
        }
        previous = static_cast<std::int32_t>(code);
    }
}

class GifMediaValidator final : public IMediaValidator {
public:
    [[nodiscard]] std::string_view formatId() const noexcept override { return "gif"; }

    [[nodiscard]] Result<LevelResult> validate(carving::IContentReader& content,
                                               const MediaLimits& limits) const override {
        const MediaVerdict verdict("gif decoder");
        ContentBytes bytes(content, 0, content.size());
        const auto truncated = [&](std::string detail) -> Result<LevelResult> {
            if (bytes.error().has_value()) {
                return *bytes.error();
            }
            return verdict.truncated(bytes.position(), std::move(detail));
        };
        std::array<std::uint8_t, 13> header{};
        for (std::uint8_t& value : header) {
            if (!bytes.next(value)) {
                return truncated("the data ends inside the header");
            }
        }
        if (header[0] != 'G' || header[1] != 'I' || header[2] != 'F' || header[3] != '8' ||
            (header[4] != '7' && header[4] != '9') || header[5] != 'a') {
            return verdict.failed(0, "not a GIF header");
        }
        if ((header[10] & 0x80U) != 0 && !bytes.skip(std::uint64_t{3} << ((header[10] & 0x07U) + 1))) {
            return truncated("the data ends inside the global color table");
        }

        std::uint64_t images = 0;
        std::uint64_t pixels = 0;
        std::uint64_t withoutEndCode = 0;
        for (;;) {
            const std::uint64_t block = bytes.position();
            std::uint8_t introducer = 0;
            if (!bytes.next(introducer)) {
                return truncated("the data ends before the trailer");
            }
            if (introducer == kTrailer) {
                break;
            }
            if (introducer == kExtension) {
                std::uint8_t label = 0;
                SubBlocks data(bytes);
                if (!bytes.next(label) || !data.skipRest()) {
                    return truncated("the data ends inside an extension");
                }
                continue;
            }
            if (introducer != kImage) {
                return verdict.failed(block, "an invalid block introducer");
            }
            std::array<std::uint8_t, 9> descriptor{};
            for (std::uint8_t& value : descriptor) {
                if (!bytes.next(value)) {
                    return truncated("the data ends inside an image descriptor");
                }
            }
            const std::uint32_t width = descriptor[4] | (std::uint32_t{descriptor[5]} << 8);
            const std::uint32_t height = descriptor[6] | (std::uint32_t{descriptor[7]} << 8);
            const std::uint8_t packed = descriptor[8];
            if ((packed & 0x80U) != 0 && !bytes.skip(std::uint64_t{3} << ((packed & 0x07U) + 1))) {
                return truncated("the data ends inside a local color table");
            }
            std::uint8_t codeSizeByte = 0;
            if (!bytes.next(codeSizeByte)) {
                return truncated("the data ends before an image's LZW minimum code size");
            }
            const std::uint32_t codeSize = codeSizeByte;
            if (codeSize < kMinCodeSize || codeSize > kMaxCodeSize) {
                return verdict.failed(block, "image " + std::to_string(images + 1) + ": LZW minimum code size " +
                                                 std::to_string(codeSize));
            }
            const std::uint64_t target = std::uint64_t{width} * height;
            if (target > limits.maxDecodedBytes - pixels) {
                return verdict.unsupported("the images hold more pixels than the decoding limit (" +
                                           describeBytes(limits.maxDecodedBytes) + ")");
            }
            SubBlocks data(bytes);
            const LzwOutcome lzw = decodeLzw(data, codeSize, target);
            ++images;
            const std::string image = "image " + std::to_string(images) + " (" + std::to_string(width) + "x" +
                                      std::to_string(height) + ")";
            switch (lzw.kind) {
            case LzwOutcome::Kind::Cut:
                return truncated(image + ": the data ends inside the LZW data after " + std::to_string(lzw.pixels) +
                                 " of " + std::to_string(target) + " pixels");
            case LzwOutcome::Kind::Invalid:
                return verdict.failed(block, image + ": " + lzw.detail);
            case LzwOutcome::Kind::Done:
                break;
            }
            if (!lzw.endCode) {
                ++withoutEndCode;
            }
            // What follows the end code in the sub-blocks is not image data; decoders skip it.
            if (!data.skipRest()) {
                return truncated(image + ": the data ends inside the image data's sub-blocks");
            }
            pixels += target;
        }
        if (images == 0) {
            return verdict.failed(0, "no image");
        }
        std::string detail = std::to_string(images) + (images == 1 ? " image" : " images") + ", " +
                             std::to_string(pixels) + " pixels decoded from their LZW data";
        if (withoutEndCode > 0) {
            detail += "; " + std::to_string(withoutEndCode) + " without an end-of-information code";
        }
        return verdict.passed(std::move(detail));
    }
};

}  // namespace

std::shared_ptr<const IMediaValidator> makeGifMediaValidator() {
    return std::make_shared<GifMediaValidator>();
}

}  // namespace recovery::validation::detail
