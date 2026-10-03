#include "formats/aac_format.hpp"

#include "audio_tags.hpp"
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
using detail::Depth;
using detail::Walk;
using detail::WalkStatus;

constexpr std::size_t kHeader = 7;
constexpr std::size_t kCrcSize = 2;
// Sync, MPEG version, layer, protection, profile, sampling frequency and
// channel configuration (not the private bit): what every frame of a stream shares.
constexpr std::uint32_t kStreamMask = 0xFFFFFDC0;
constexpr std::uint32_t kSamplingFrequencies = 13;
constexpr std::array<std::uint32_t, kSamplingFrequencies> kSampleRates = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
constexpr std::array<std::string_view, 4> kProfiles = {"Main", "LC", "SSR", "LTP"};
// The decoder input buffer holds 6144 bits per channel (ISO/IEC 14496-3
// 4.5.3.1), so no raw data block is larger than 768 bytes per channel.
constexpr std::uint32_t kMaxBlockBytesPerChannel = 768;
constexpr std::uint32_t kMaxFrameLength = 8191;  // 13 bits
// Channels of each channel configuration (0: described by a program config element).
constexpr std::array<std::uint32_t, 8> kChannels = {0, 1, 2, 3, 4, 5, 6, 8};

struct FrameHeader {
    std::uint32_t bits = 0;  // the first four bytes
    bool mpeg2 = false;
    bool crc = false;
    std::uint32_t profile = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;  // 0: described by a program config element
    std::uint32_t length = 0;
    std::uint32_t rawBlocks = 0;

    [[nodiscard]] std::uint32_t key() const noexcept { return bits & kStreamMask; }
};

std::optional<FrameHeader> parseHeader(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kHeader) {
        return std::nullopt;
    }
    FrameHeader header;
    header.bits = loadBe32(bytes, 0);
    // Sync 0xFFF and layer 00.
    if ((header.bits >> 20) != 0xFFF || ((header.bits >> 17) & 3) != 0) {
        return std::nullopt;
    }
    const std::uint32_t frequency = (header.bits >> 10) & 0xF;
    if (frequency >= kSamplingFrequencies) {
        return std::nullopt;
    }
    header.mpeg2 = ((header.bits >> 19) & 1) != 0;
    header.crc = ((header.bits >> 16) & 1) == 0;
    header.profile = (header.bits >> 14) & 3;
    header.sampleRate = kSampleRates[frequency];
    header.channels = (header.bits >> 6) & 7;
    header.length = ((header.bits & 3) << 11) | (std::uint32_t{loadU8(bytes, 4)} << 3) | (loadU8(bytes, 5) >> 5);
    header.rawBlocks = (loadU8(bytes, 6) & 3U) + 1;
    // The header, its CRC (one per raw data block after the first), and data
    // no larger than the decoder's input buffer.
    const std::uint32_t overhead =
        static_cast<std::uint32_t>(kHeader) + (header.crc ? static_cast<std::uint32_t>(kCrcSize) * header.rawBlocks : 0);
    const std::uint32_t channels = kChannels[header.channels];
    const std::uint32_t largest =
        channels == 0 ? kMaxFrameLength
                      : std::min(kMaxFrameLength, overhead + kMaxBlockBytesPerChannel * channels * header.rawBlocks);
    if (header.length <= overhead || header.length > largest) {
        return std::nullopt;
    }
    return header;
}

// Walks an ADTS file from its first byte. See structure_walk.hpp for what a walk reports.
class AdtsWalker {
public:
    AdtsWalker(IContentReader& content, Depth depth) : content_(content), depth_(depth) {}

    Result<Walk> run();

private:
    [[nodiscard]] std::string summary() const;

    IContentReader& content_;
    Depth depth_;
    Walk walk_;
    std::optional<FrameHeader> first_;
    std::uint64_t frames_ = 0;
};

std::string AdtsWalker::summary() const {
    std::string text = std::string(first_->mpeg2 ? "MPEG-2" : "MPEG-4") + " AAC " +
                       std::string(kProfiles[first_->profile]) + ", " + std::to_string(first_->sampleRate) + " Hz, ";
    text += first_->channels == 0 ? std::string("channels in a PCE") : std::to_string(first_->channels) + " channels";
    return text + ", " + std::to_string(frames_) + " frames";
}

Result<Walk> AdtsWalker::run() {
    const std::uint64_t size = content_.size();
    Result<detail::TagRun> leading = detail::skipLeadingTags(content_, 0, depth_, walk_);
    if (!leading.ok()) {
        return leading.error();
    }
    if (leading->truncated) {
        return walk_.finish(WalkStatus::Truncated, leading->end, "the data ends inside the leading ID3v2 tags");
    }
    const std::uint64_t streamStart = leading->end;
    std::uint64_t position = streamStart;
    // The data ends inside a frame. Before one whole frame, and with no
    // ID3v2 tag before it, nothing confirms the lone header: not a file.
    const auto cutOff = [&](std::uint64_t at, std::string detail) -> Walk& {
        if (frames_ == 0 && streamStart == 0) {
            return walk_.finish(WalkStatus::Broken, 0, "a lone frame header, cut off by the end of the data");
        }
        return walk_.finish(WalkStatus::Truncated, at, std::move(detail));
    };
    bool dataEnded = false;
    for (;;) {
        const std::uint64_t remaining = size - position;
        if (remaining < kHeader) {
            if (remaining == 0) {
                dataEnded = true;
                break;
            }
            Result<std::span<const std::byte>> rest = content_.read(position, static_cast<std::size_t>(remaining));
            if (!rest.ok()) {
                return rest.error();
            }
            if (loadU8(*rest, 0) == 0xFF) {
                return cutOff(position, "the data ends inside a frame header");
            }
            break;
        }
        Result<std::span<const std::byte>> bytes = content_.read(position, kHeader);
        if (!bytes.ok()) {
            return bytes.error();
        }
        const std::optional<FrameHeader> header = parseHeader(*bytes);
        if (!header.has_value() || (first_.has_value() && header->key() != first_->key())) {
            break;
        }
        if (header->length > remaining) {
            return cutOff(position, "the data ends inside frame " + std::to_string(frames_ + 1));
        }
        if (!first_.has_value()) {
            first_ = header;
        }
        ++frames_;
        position += header->length;
    }

    if (frames_ == 0) {
        if (dataEnded) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends before the first frame");
        }
        return walk_.finish(WalkStatus::Broken, 0,
                            streamStart > 0 ? "no ADTS frame after the ID3v2 tag" : "no ADTS frame");
    }
    if (frames_ < AacFormat::kMinimumFrames) {
        if (dataEnded) {
            return walk_.finish(WalkStatus::Truncated, position,
                                "the data ends after " + std::to_string(frames_) + " frames");
        }
        return walk_.finish(WalkStatus::Broken, 0, "only " + std::to_string(frames_) + " consecutive frames");
    }
    Result<detail::TagRun> trailing = detail::skipTrailingTags(content_, position, depth_, walk_);
    if (!trailing.ok()) {
        return trailing.error();
    }
    if (trailing->truncated) {
        return walk_.finish(WalkStatus::Truncated, trailing->end, "the data ends inside a tag after the frames");
    }
    std::string text = summary();
    if (leading->count + trailing->count > 0) {
        text += ", " + std::to_string(leading->count + trailing->count) + " tags";
    }
    return walk_.finish(WalkStatus::Complete, trailing->end, std::move(text));
}

Result<Walk> walkAdts(IContentReader& content, Depth depth) {
    return AdtsWalker(content, depth).run();
}

}  // namespace

AacFormat::AacFormat() {
    descriptor_.id = "aac";
    descriptor_.name = "AAC audio (ADTS)";
    descriptor_.extension = "aac";
    descriptor_.signatures = {
        carving::textSignature("ID3v2 tag", "ID3"),
        carving::byteSignature("ADTS frame", {0xFF, 0xF0}, 0, {0xFF, 0xF6}),
    };
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::StructureWalk;
    descriptor_.selfSynchronizing = true;
}

HeaderCheck AacFormat::checkHeader(std::span<const std::byte> header) const {
    if (header.size() >= 3 && header[0] == std::byte{'I'} && header[1] == std::byte{'D'} &&
        header[2] == std::byte{'3'}) {
        return detail::parseId3v2Header(header).has_value() ? HeaderCheck::accept()
                                                             : HeaderCheck::reject("not an ID3v2 header");
    }
    const std::optional<FrameHeader> first = parseHeader(header);
    if (!first.has_value()) {
        return HeaderCheck::reject("not an ADTS frame header");
    }
    // The next frames, as far as the header holds theirs (a frame of up to
    // 8191 bytes may leave none of them in it).
    std::uint64_t position = first->length;
    std::uint64_t frames = 1;
    while (frames < kMinimumFrames && position + kHeader <= header.size()) {
        const std::optional<FrameHeader> next = parseHeader(header.subspan(static_cast<std::size_t>(position)));
        if (!next.has_value() || next->key() != first->key()) {
            return HeaderCheck::reject("only " + std::to_string(frames) + " consecutive frames");
        }
        position += next->length;
        ++frames;
    }
    return HeaderCheck::accept();
}

Result<EndDetection> AacFormat::findEnd(IContentReader& content) const {
    Result<Walk> walk = walkAdts(content, Depth::Layout);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::endOf(*walk, content.size());
}

Result<ValidationResult> AacFormat::validate(IContentReader& content) const {
    Result<Walk> walk = walkAdts(content, Depth::Full);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::verdictOf(*walk, content.size());
}

}  // namespace recovery::formats
