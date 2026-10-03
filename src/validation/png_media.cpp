// PNG media validation: every zlib stream of the file inflated (the image of
// the IDAT chunks, and the frames of an APNG animation), against the exact
// size of its scanlines (Adam7 passes included) and their filter types.

#include "content_bytes.hpp"
#include "inflate.hpp"
#include "media_decoders.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace recovery::validation::detail {

namespace {

constexpr std::array<std::uint8_t, 8> kSignature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::uint32_t kMaxChunkLength = 0x7FFFFFFF;

// Adam7: first column, first row, column step, row step of each pass.
constexpr std::array<std::array<std::uint32_t, 4>, 7> kAdam7 = {{
    {0, 0, 8, 8},
    {4, 0, 8, 8},
    {0, 4, 4, 8},
    {2, 0, 4, 4},
    {0, 2, 2, 4},
    {1, 0, 2, 2},
    {0, 1, 1, 2},
}};

bool isType(std::span<const std::byte> header, const char (&type)[5]) {
    for (std::size_t i = 0; i < 4; ++i) {
        if (static_cast<char>(header[4 + i]) != type[i]) {
            return false;
        }
    }
    return true;
}

struct ImageHeader {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t depth = 0;
    std::uint8_t color = 0;
    bool interlaced = false;
    std::uint32_t bitsPerPixel = 0;
};

// One pass of an image: its rows, and the bytes of each row after the filter type.
struct Pass {
    std::uint64_t rows = 0;
    std::uint64_t rowBytes = 0;
};

std::vector<Pass> passesOf(std::uint32_t width, std::uint32_t height, const ImageHeader& header) {
    std::vector<Pass> passes;
    const auto add = [&](std::uint64_t columns, std::uint64_t rows) {
        if (columns > 0 && rows > 0) {
            passes.push_back(Pass{rows, (columns * header.bitsPerPixel + 7) / 8});
        }
    };
    if (!header.interlaced) {
        add(width, height);
        return passes;
    }
    for (const auto& [x0, y0, dx, dy] : kAdam7) {
        add(width > x0 ? (std::uint64_t{width} - x0 + dx - 1) / dx : 0,
            height > y0 ? (std::uint64_t{height} - y0 + dy - 1) / dy : 0);
    }
    return passes;
}

// Bytes of scanlines (filter type bytes included), if they fit in 64 bits.
std::optional<std::uint64_t> scanlineBytes(const std::vector<Pass>& passes) {
    std::uint64_t total = 0;
    for (const Pass& pass : passes) {
        const std::optional<std::uint64_t> row = checkedAdd<std::uint64_t>(pass.rowBytes, 1);
        const std::optional<std::uint64_t> bytes = row.has_value() ? checkedMul(*row, pass.rows) : std::nullopt;
        const std::optional<std::uint64_t> sum = bytes.has_value() ? checkedAdd(total, *bytes) : std::nullopt;
        if (!sum.has_value()) {
            return std::nullopt;
        }
        total = *sum;
    }
    return total;
}

// Follows the inflated data through the scanlines: a filter type of 0 to 4
// starts each row, and the rows end with the data.
class ScanlineSink final : public InflateSink {
public:
    ScanlineSink(std::vector<Pass> passes, std::uint64_t expected) : passes_(std::move(passes)), expected_(expected) {}

    bool write(std::span<const std::uint8_t> data) override {
        std::size_t i = 0;
        while (i < data.size()) {
            if (received_ >= expected_) {
                problem_ =
                    "the image data holds more than the " + std::to_string(expected_) + " bytes of its scanlines";
                return false;
            }
            const Pass& pass = passes_[pass_];
            if (column_ == 0 && data[i] > 4) {
                problem_ = "row " + std::to_string(row_) + (passes_.size() > 1 ? " of pass " + std::to_string(pass_ + 1)
                                                                                 : std::string()) +
                           " has the filter type " + std::to_string(data[i]) + " (0 to 4 exist)";
                return false;
            }
            const std::uint64_t rowLeft = 1 + pass.rowBytes - column_;
            const std::uint64_t take = std::min<std::uint64_t>(rowLeft, data.size() - i);
            i += static_cast<std::size_t>(take);
            received_ += take;
            column_ += take;
            if (column_ == 1 + pass.rowBytes) {
                column_ = 0;
                if (++row_ == pass.rows) {
                    row_ = 0;
                    ++pass_;
                }
            }
        }
        return true;
    }

    [[nodiscard]] std::uint64_t received() const noexcept { return received_; }
    [[nodiscard]] const std::string& problem() const noexcept { return problem_; }

private:
    std::vector<Pass> passes_;
    std::uint64_t expected_;
    std::size_t pass_ = 0;
    std::uint64_t row_ = 0;
    std::uint64_t column_ = 0;
    std::uint64_t received_ = 0;
    std::string problem_;
};

// An image to inflate: the IDAT image, or a frame of an animation.
struct Image {
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<ContentBytes::Range> data;
};

class PngMediaValidator final : public IMediaValidator {
public:
    [[nodiscard]] std::string_view formatId() const noexcept override { return "png"; }

    [[nodiscard]] Result<LevelResult> validate(carving::IContentReader& content,
                                               const MediaLimits& limits) const override {
        const MediaVerdict verdict("png decoder");
        const std::uint64_t size = content.size();
        if (size < kSignature.size()) {
            return verdict.truncated(size, "the data ends inside the signature");
        }
        Result<std::span<const std::byte>> signature = content.read(0, kSignature.size());
        if (!signature.ok()) {
            return signature.error();
        }
        for (std::size_t i = 0; i < kSignature.size(); ++i) {
            if (static_cast<std::uint8_t>((*signature)[i]) != kSignature[i]) {
                return verdict.failed(0, "not a PNG signature");
            }
        }

        // The chunks: IHDR, the image data, and the frames of an animation.
        std::optional<ImageHeader> header;
        Image idat{"the image", 0, 0, {}};
        std::vector<Image> frames;
        std::optional<std::uint32_t> declaredFrames;
        bool frameBeforeImageData = false;
        bool imageDataSeen = false;
        std::uint32_t sequence = 0;
        std::size_t ranges = 0;
        bool ended = false;
        std::uint64_t offset = kSignature.size();
        while (!ended && size - offset >= 8) {
            Result<std::span<const std::byte>> chunk = content.read(offset, 8);
            if (!chunk.ok()) {
                return chunk.error();
            }
            const std::uint32_t length = loadBe32(*chunk, 0);
            if (length > kMaxChunkLength) {
                return verdict.failed(offset, "a chunk length of " + std::to_string(length));
            }
            const std::uint64_t payload = offset + 8;
            const std::uint64_t end = payload + length + 4;
            const bool whole = end <= size;
            if (isType(*chunk, "IEND")) {
                ended = true;
            } else if (isType(*chunk, "IHDR")) {
                if (length != 13 || !whole) {
                    return length != 13 ? verdict.failed(offset, "IHDR is " + std::to_string(length) + " bytes long")
                                        : verdict.truncated(size, "the data ends inside IHDR");
                }
                Result<std::span<const std::byte>> fields = content.read(payload, 13);
                if (!fields.ok()) {
                    return fields.error();
                }
                ImageHeader parsed;
                parsed.width = loadBe32(*fields, 0);
                parsed.height = loadBe32(*fields, 4);
                parsed.depth = loadU8(*fields, 8);
                parsed.color = loadU8(*fields, 9);
                parsed.interlaced = loadU8(*fields, 12) == 1;
                const std::uint32_t channels = parsed.color == 0   ? 1
                                               : parsed.color == 2 ? 3
                                               : parsed.color == 3 ? 1
                                               : parsed.color == 4 ? 2
                                               : parsed.color == 6 ? 4
                                                                   : 0;
                if (parsed.width == 0 || parsed.height == 0 || parsed.width > kMaxChunkLength ||
                    parsed.height > kMaxChunkLength || channels == 0 || parsed.depth == 0 || parsed.depth > 16 ||
                    loadU8(*fields, 10) != 0 || loadU8(*fields, 11) != 0 || loadU8(*fields, 12) > 1) {
                    return verdict.failed(offset, "IHDR's fields are not valid");
                }
                parsed.bitsPerPixel = channels * parsed.depth;
                header = parsed;
            } else if (isType(*chunk, "IDAT")) {
                imageDataSeen = true;
                idat.data.push_back({payload, std::min(payload + length, size)});
                ++ranges;
            } else if (isType(*chunk, "acTL") && !imageDataSeen && whole && length == 8) {
                Result<std::span<const std::byte>> fields = content.read(payload, 8);
                if (!fields.ok()) {
                    return fields.error();
                }
                declaredFrames = loadBe32(*fields, 0);
            } else if (declaredFrames.has_value() && (isType(*chunk, "fcTL") || isType(*chunk, "fdAT"))) {
                // An animation (APNG): frame controls and frame data, numbered in one sequence.
                const bool control = isType(*chunk, "fcTL");
                if (!whole && control) {
                    break;
                }
                if (length < (control ? 26U : 4U)) {
                    return verdict.failed(offset, std::string(control ? "fcTL" : "fdAT") + " is too short");
                }
                Result<std::span<const std::byte>> fields = content.read(payload, control ? 26 : 4);
                if (!fields.ok()) {
                    return fields.error();
                }
                const std::uint32_t number = loadBe32(*fields, 0);
                if (number != sequence) {
                    return verdict.failed(offset, "animation chunk " + std::to_string(number) + " where " +
                                                      std::to_string(sequence) + " comes next");
                }
                ++sequence;
                if (control) {
                    Image frame;
                    frame.name = "frame " + std::to_string(frames.size() + 1);
                    frame.width = loadBe32(*fields, 4);
                    frame.height = loadBe32(*fields, 8);
                    const std::uint32_t x = loadBe32(*fields, 12);
                    const std::uint32_t y = loadBe32(*fields, 16);
                    const std::uint8_t dispose = loadU8(*fields, 24);
                    const std::uint8_t blend = loadU8(*fields, 25);
                    if (!header.has_value() || frame.width == 0 || frame.height == 0 ||
                        !rangeWithin<std::uint64_t>(x, frame.width, header->width) ||
                        !rangeWithin<std::uint64_t>(y, frame.height, header->height) || dispose > 2 || blend > 1) {
                        return verdict.failed(offset, frame.name + "'s control (size, position, disposal or "
                                                                   "blending) is not valid");
                    }
                    if (!imageDataSeen) {
                        // The IDAT image is the first frame.
                        if (frameBeforeImageData || x != 0 || y != 0 || frame.width != header->width ||
                            frame.height != header->height) {
                            return verdict.failed(offset, "the first frame does not cover the image");
                        }
                        frameBeforeImageData = true;
                    }
                    frames.push_back(std::move(frame));
                } else {
                    if (frames.empty() || (frames.size() == 1 && frameBeforeImageData)) {
                        return verdict.failed(offset, "frame data before its frame control");
                    }
                    frames.back().data.push_back({payload + 4, std::min(payload + length, size)});
                    ++ranges;
                }
            }
            if (!whole) {
                break;
            }
            offset = end;
        }
        // The content ends before IEND: images cut short are truncated, not broken.
        const bool cut = !ended;
        if (!header.has_value()) {
            return cut ? verdict.truncated(size, "the data ends before IHDR") : verdict.failed(8, "no IHDR chunk");
        }
        if (idat.data.empty()) {
            return cut ? verdict.truncated(size, "the data ends before the image data")
                       : verdict.failed(8, "no image data");
        }
        if (ranges > limits.maxMemory / sizeof(ContentBytes::Range)) {
            return verdict.unsupported("the image data is split into " + std::to_string(ranges) +
                                       " chunks, more than the memory limit allows");
        }
        idat.width = header->width;
        idat.height = header->height;
        std::vector<Image> images;
        images.push_back(std::move(idat));
        if (declaredFrames.has_value()) {
            if (!cut && *declaredFrames != frames.size()) {
                return verdict.failed(8, "acTL announces " + std::to_string(*declaredFrames) + " frames, there are " +
                                             std::to_string(frames.size()));
            }
            for (std::size_t i = frameBeforeImageData ? 1 : 0; i < frames.size(); ++i) {
                if (frames[i].data.empty() && !(cut && i + 1 == frames.size())) {
                    return verdict.failed(8, frames[i].name + " has no data");
                }
                images.push_back(std::move(frames[i]));
            }
        }

        // The work, before any of it is done.
        std::uint64_t work = 0;
        for (const Image& image : images) {
            const std::optional<std::uint64_t> bytes = scanlineBytes(passesOf(image.width, image.height, *header));
            const std::optional<std::uint64_t> sum = bytes.has_value() ? checkedAdd(work, *bytes) : std::nullopt;
            if (!sum.has_value() || *sum > limits.maxDecodedBytes) {
                return verdict.unsupported("the images hold more scanline data than the decoding limit (" +
                                           describeBytes(limits.maxDecodedBytes) + ")");
            }
            work = *sum;
        }

        std::uint64_t inflated = 0;
        for (Image& image : images) {
            if (image.data.empty()) {
                return verdict.truncated(size, "the data ends before " + image.name + "'s data");
            }
            std::vector<Pass> passes = passesOf(image.width, image.height, *header);
            const std::uint64_t expected = *scanlineBytes(passes);
            const std::uint64_t first = image.data.front().begin;
            ContentBytes bytes(content, std::move(image.data));
            ScanlineSink sink(std::move(passes), expected);
            const InflateOutcome outcome = inflateZlib(bytes, sink);
            const std::string where = image.name + "'s zlib stream";
            switch (outcome.kind) {
            case InflateOutcome::Kind::ReadError:
                return *bytes.error();
            case InflateOutcome::Kind::Truncated:
                if (cut) {
                    return verdict.truncated(outcome.offset, where + ": " + outcome.detail);
                }
                return verdict.failed(outcome.offset,
                                      where + " ends before its last block (" + std::to_string(sink.received()) +
                                          " of " + std::to_string(expected) + " bytes of scanlines)");
            case InflateOutcome::Kind::Invalid:
                return verdict.failed(outcome.offset, where + ": " + outcome.detail);
            case InflateOutcome::Kind::Stopped:
                return verdict.failed(outcome.offset, where + ": " + sink.problem());
            case InflateOutcome::Kind::Done:
                break;
            }
            if (sink.received() != expected) {
                return verdict.failed(first, where + " holds " + std::to_string(sink.received()) + " of the " +
                                                 std::to_string(expected) + " bytes of its scanlines");
            }
            if (outcome.trailing > 0) {
                return verdict.failed(first, where + " is followed by " + std::to_string(outcome.trailing) +
                                                 " more bytes of image data");
            }
            inflated += expected;
        }
        std::string detail = std::to_string(header->width) + "x" + std::to_string(header->height) + ", " +
                             std::to_string(header->bitsPerPixel) + " bits per pixel" +
                             (header->interlaced ? ", Adam7" : "") + ": " + std::to_string(images.size()) +
                             (images.size() == 1 ? " zlib stream" : " zlib streams") + " inflated to " +
                             std::to_string(inflated) + " bytes of scanlines, Adler-32 correct";
        if (cut) {
            return verdict.truncated(size, "the data ends before IEND; " + detail);
        }
        return verdict.passed(std::move(detail));
    }
};

}  // namespace

std::shared_ptr<const IMediaValidator> makePngMediaValidator() {
    return std::make_shared<PngMediaValidator>();
}

}  // namespace recovery::validation::detail
