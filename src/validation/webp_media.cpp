// WebP media validation: every image bitstream of the file, simple or
// extended, animation frames included: VP8L images and ALPH alpha planes
// decoded in full (vp8l_decoder.hpp), VP8 frames checked as far as their
// headers and partitions go (vp8_header.hpp). Each bitstream's size must be
// the canvas's (a still image) or its frame's (ANMF).

#include "content_bytes.hpp"
#include "media_decoders.hpp"
#include "vp8_header.hpp"
#include "vp8l_decoder.hpp"

#include "recovery/byte_order.hpp"

#include <optional>
#include <string>
#include <vector>

namespace recovery::validation::detail {

namespace {

bool isChunk(std::span<const std::byte> header, const char (&type)[5]) {
    for (std::size_t i = 0; i < 4; ++i) {
        if (static_cast<char>(header[i]) != type[i]) {
            return false;
        }
    }
    return true;
}

struct Range {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

class WebpWalk {
public:
    WebpWalk(carving::IContentReader& content, const MediaLimits& limits)
        : content_(content), limits_(limits), verdict_("webp decoder") {}

    Result<LevelResult> run();

private:
    using Outcome = std::optional<Result<LevelResult>>;

    // An image bitstream (VP8 or VP8L, with its ALPH when lossy) of the size
    // `width` x `height` (0: any).
    Outcome image(bool lossless, Range data, std::optional<Range> alpha, std::uint32_t width, std::uint32_t height,
                  const std::string& name);
    Outcome alphaPlane(Range data, std::uint32_t width, std::uint32_t height, const std::string& name);
    Outcome vp8lOutcome(const Vp8lOutcome& outcome, Range data, const std::string& name);

    carving::IContentReader& content_;
    const MediaLimits& limits_;
    MediaVerdict verdict_;
    std::uint64_t work_ = 0;
    std::uint64_t lossless_ = 0;
    std::uint64_t lossy_ = 0;
    std::uint64_t alpha_ = 0;
    std::string losslessSummary_;
};

WebpWalk::Outcome WebpWalk::vp8lOutcome(const Vp8lOutcome& outcome, Range data, const std::string& name) {
    switch (outcome.kind) {
    case Vp8lOutcome::Kind::Done:
        work_ = outcome.work;
        return std::nullopt;
    case Vp8lOutcome::Kind::ReadError:
        break;
    case Vp8lOutcome::Kind::Ended:
        if (data.end > content_.size()) {
            return Result<LevelResult>(verdict_.truncated(outcome.offset, name + ": " + outcome.detail));
        }
        return Result<LevelResult>(
            verdict_.failed(outcome.offset, name + ": the chunk ends inside the bitstream (" + outcome.detail + ")"));
    case Vp8lOutcome::Kind::Invalid:
        return Result<LevelResult>(verdict_.failed(outcome.offset, name + ": " + outcome.detail));
    case Vp8lOutcome::Kind::Unsupported:
        return Result<LevelResult>(verdict_.unsupported(name + ": " + outcome.detail));
    }
    return Result<LevelResult>(makeError(ErrorCode::IoError, "the content could not be read"));
}

WebpWalk::Outcome WebpWalk::alphaPlane(Range data, std::uint32_t width, std::uint32_t height,
                                       const std::string& name) {
    const std::uint64_t size = content_.size();
    if (data.begin >= std::min(data.end, size)) {
        return Result<LevelResult>(data.end > size
                                       ? verdict_.truncated(size, "the data ends inside " + name + "'s ALPH")
                                       : verdict_.failed(data.begin, name + "'s ALPH chunk is empty"));
    }
    Result<std::span<const std::byte>> first = content_.read(data.begin, 1);
    if (!first.ok()) {
        return Result<LevelResult>(first.error());
    }
    const std::uint8_t header = loadU8(*first, 0);
    const std::uint32_t compression = header & 0x03U;
    const std::uint32_t preprocessing = (header >> 4) & 0x03U;
    if ((header >> 6) != 0 || compression > 1 || preprocessing > 1) {
        return Result<LevelResult>(verdict_.failed(data.begin, name + "'s ALPH header is not valid"));
    }
    ++alpha_;
    const Range plane{data.begin + 1, data.end};
    if (compression == 0) {
        if (plane.end - plane.begin < std::uint64_t{width} * height) {
            return Result<LevelResult>(verdict_.failed(
                data.begin, name + "'s uncompressed alpha holds fewer than its " +
                                std::to_string(std::uint64_t{width} * height) + " pixels"));
        }
        return std::nullopt;
    }
    ContentBytes bytes(content_, plane.begin, plane.end);
    const Vp8lOutcome outcome = decodeVp8lStream(bytes, width, height, limits_, work_);
    if (outcome.kind == Vp8lOutcome::Kind::ReadError) {
        return Result<LevelResult>(*bytes.error());
    }
    return vp8lOutcome(outcome, plane, name + "'s lossless alpha");
}

WebpWalk::Outcome WebpWalk::image(bool lossless, Range data, std::optional<Range> alpha, std::uint32_t width,
                                  std::uint32_t height, const std::string& name) {
    std::uint32_t decodedWidth = 0;
    std::uint32_t decodedHeight = 0;
    if (lossless) {
        ContentBytes bytes(content_, data.begin, data.end);
        const Vp8lOutcome outcome = decodeVp8l(bytes, limits_, work_);
        if (outcome.kind == Vp8lOutcome::Kind::ReadError) {
            return Result<LevelResult>(*bytes.error());
        }
        if (Outcome failure = vp8lOutcome(outcome, data, name); failure.has_value()) {
            return failure;
        }
        decodedWidth = outcome.width;
        decodedHeight = outcome.height;
        ++lossless_;
        losslessSummary_ = std::to_string(outcome.width) + "x" + std::to_string(outcome.height) + ", " +
                           std::to_string(outcome.transforms) +
                           (outcome.transforms == 1 ? " transform" : " transforms") +
                           ", " + (outcome.cacheBits != 0 ? "a " + std::to_string(outcome.cacheBits) + "-bit"
                                                          : std::string("no")) +
                           " color cache, " + std::to_string(outcome.groups) +
                           (outcome.groups == 1 ? " prefix code group" : " prefix code groups");
    } else {
        Result<Vp8Check> check = checkVp8(content_, data.begin, data.end);
        if (!check.ok()) {
            return Result<LevelResult>(check.error());
        }
        switch (check->kind) {
        case Vp8Check::Kind::Ended:
            return Result<LevelResult>(verdict_.truncated(check->offset, name + ": " + check->detail));
        case Vp8Check::Kind::Invalid:
            return Result<LevelResult>(verdict_.failed(check->offset, name + ": " + check->detail));
        case Vp8Check::Kind::Done:
            break;
        }
        decodedWidth = check->width;
        decodedHeight = check->height;
        ++lossy_;
    }
    if (width != 0 && (decodedWidth != width || decodedHeight != height)) {
        return Result<LevelResult>(verdict_.failed(data.begin, name + " is " + std::to_string(decodedWidth) + "x" +
                                                                   std::to_string(decodedHeight) + ", not " +
                                                                   std::to_string(width) + "x" +
                                                                   std::to_string(height)));
    }
    if (!lossless && alpha.has_value()) {
        return alphaPlane(*alpha, decodedWidth, decodedHeight, name);
    }
    return std::nullopt;
}

Result<LevelResult> WebpWalk::run() {
    const std::uint64_t size = content_.size();
    if (size < 12) {
        return verdict_.truncated(size, "the data ends inside the RIFF header");
    }
    Result<std::span<const std::byte>> riff = content_.read(0, 12);
    if (!riff.ok()) {
        return riff.error();
    }
    if (!isChunk(*riff, "RIFF") || !isChunk(riff->subspan(8), "WEBP")) {
        return verdict_.failed(0, "not a RIFF WEBP header");
    }
    const std::uint64_t fileEnd = 8 + std::uint64_t{loadLe32(*riff, 4)};

    // The chunks of one level: the file's, or an animation frame's.
    struct Chunk {
        std::string type;
        Range payload;
        std::uint64_t offset = 0;
    };
    const auto chunksOf = [&](Range range, std::vector<Chunk>& chunks) -> Status {
        std::uint64_t offset = range.begin;
        while (offset < range.end && range.end - offset >= 8 && size - std::min(size, offset) >= 8) {
            Result<std::span<const std::byte>> header = content_.read(offset, 8);
            if (!header.ok()) {
                return header.error();
            }
            Chunk chunk;
            for (std::size_t i = 0; i < 4; ++i) {
                chunk.type.push_back(static_cast<char>((*header)[i]));
            }
            const std::uint64_t length = loadLe32(*header, 4);
            chunk.offset = offset;
            chunk.payload = Range{offset + 8, offset + 8 + length};
            chunks.push_back(chunk);
            offset = chunk.payload.end + (length & 1U);
        }
        return success();
    };
    std::vector<Chunk> chunks;
    if (Status walked = chunksOf(Range{12, fileEnd}, chunks); !walked.ok()) {
        return walked.error();
    }
    if (chunks.empty()) {
        return fileEnd > size ? verdict_.truncated(size, "the data ends before the first chunk")
                              : verdict_.failed(12, "no image chunk");
    }

    std::string kind;
    const Chunk& first = chunks.front();
    if (first.type == "VP8 " || first.type == "VP8L") {
        kind = "simple";
        if (Outcome outcome = image(first.type == "VP8L", first.payload, std::nullopt, 0, 0, "the image");
            outcome.has_value()) {
            return std::move(*outcome);
        }
    } else if (first.type == "VP8X") {
        if (first.payload.end - first.payload.begin < 10 || first.payload.end > size) {
            return first.payload.end > size ? verdict_.truncated(size, "the data ends inside VP8X")
                                            : verdict_.failed(first.offset, "VP8X is too short");
        }
        Result<std::span<const std::byte>> header = content_.read(first.payload.begin, 10);
        if (!header.ok()) {
            return header.error();
        }
        const bool animated = (loadU8(*header, 0) & 0x02U) != 0;
        const std::uint32_t canvasWidth = 1 + (loadLe32(*header, 4) & 0xFFFFFFU);
        const std::uint32_t canvasHeight = 1 + (loadLe32(*header, 6) >> 8);
        kind = animated ? "extended, animated" : "extended";
        std::optional<Range> alpha;
        std::uint64_t frames = 0;
        bool still = false;
        for (std::size_t i = 1; i < chunks.size(); ++i) {
            const Chunk& chunk = chunks[i];
            if (chunk.type == "ALPH" && !animated) {
                alpha = chunk.payload;
            } else if ((chunk.type == "VP8 " || chunk.type == "VP8L") && !animated) {
                still = true;
                if (Outcome outcome = image(chunk.type == "VP8L", chunk.payload, alpha, canvasWidth, canvasHeight,
                                            "the image");
                    outcome.has_value()) {
                    return std::move(*outcome);
                }
            } else if (chunk.type == "ANMF" && animated) {
                ++frames;
                const std::string name = "frame " + std::to_string(frames);
                if (chunk.payload.end - chunk.payload.begin < 16 || chunk.payload.begin + 16 > size) {
                    return chunk.payload.begin + 16 > size ? verdict_.truncated(size, "the data ends inside " + name)
                                                           : verdict_.failed(chunk.offset, name + " is too short");
                }
                Result<std::span<const std::byte>> frame = content_.read(chunk.payload.begin, 16);
                if (!frame.ok()) {
                    return frame.error();
                }
                const std::uint64_t x = 2ULL * (loadLe32(*frame, 0) & 0xFFFFFFU);
                const std::uint64_t y = 2ULL * (loadLe32(*frame, 3) & 0xFFFFFFU);
                const std::uint32_t width = 1 + (loadLe32(*frame, 6) & 0xFFFFFFU);
                const std::uint32_t height = 1 + (loadLe32(*frame, 9) & 0xFFFFFFU);
                if (x + width > canvasWidth || y + height > canvasHeight) {
                    return verdict_.failed(chunk.offset, name + " lies outside the canvas");
                }
                std::vector<Chunk> parts;
                if (Status walked = chunksOf(Range{chunk.payload.begin + 16, chunk.payload.end}, parts); !walked.ok()) {
                    return walked.error();
                }
                std::optional<Range> frameAlpha;
                bool found = false;
                for (const Chunk& part : parts) {
                    if (part.type == "ALPH") {
                        frameAlpha = part.payload;
                    } else if (part.type == "VP8 " || part.type == "VP8L") {
                        found = true;
                        if (Outcome outcome = image(part.type == "VP8L", part.payload, frameAlpha, width, height, name);
                            outcome.has_value()) {
                            return std::move(*outcome);
                        }
                        break;
                    }
                }
                if (!found) {
                    return chunk.payload.end > size ? verdict_.truncated(size, "the data ends inside " + name)
                                                    : verdict_.failed(chunk.offset, name + " has no image");
                }
            }
        }
        if (!still && frames == 0) {
            return fileEnd > size ? verdict_.truncated(size, "the data ends before the image")
                                  : verdict_.failed(12, "no image in the extended file");
        }
        if (animated) {
            kind += " (" + std::to_string(frames) + (frames == 1 ? " frame)" : " frames)");
        }
    } else {
        return verdict_.failed(12, "the first chunk is not VP8, VP8L or VP8X");
    }

    std::string detail = kind + ": ";
    if (lossless_ > 0) {
        detail += std::to_string(lossless_) + " VP8L " + (lossless_ == 1 ? "image" : "images") + " decoded";
        if (lossless_ == 1) {
            detail += " (" + losslessSummary_ + ")";
        }
    }
    if (lossy_ > 0) {
        detail += std::string(lossless_ > 0 ? ", " : "") + std::to_string(lossy_) + " VP8 " +
                  (lossy_ == 1 ? "frame" : "frames") + " checked up to the token probabilities";
    }
    if (alpha_ > 0) {
        detail += ", " + std::to_string(alpha_) + " alpha " + (alpha_ == 1 ? "plane" : "planes");
    }
    if (fileEnd > size) {
        return verdict_.truncated(size, "the data ends before the RIFF size; " + detail);
    }
    return verdict_.passed(std::move(detail), lossy_ > 0 ? Coverage::Partial : Coverage::Full);
}

class WebpMediaValidator final : public IMediaValidator {
public:
    [[nodiscard]] std::string_view formatId() const noexcept override { return "webp"; }

    [[nodiscard]] Result<LevelResult> validate(carving::IContentReader& content,
                                               const MediaLimits& limits) const override {
        WebpWalk walk(content, limits);
        return walk.run();
    }
};

}  // namespace

std::shared_ptr<const IMediaValidator> makeWebpMediaValidator() {
    return std::make_shared<WebpMediaValidator>();
}

}  // namespace recovery::validation::detail
