#include "formats/mp3_format.hpp"

#include "audio_tags.hpp"
#include "crc16.hpp"
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

constexpr std::size_t kFrameHeader = 4;
constexpr std::size_t kCrcSize = 2;
// Sync, version, layer, protection and sample rate: what every frame of a stream shares.
constexpr std::uint32_t kStreamMask = 0xFFFF0C00;
constexpr std::uint32_t kMaxBigValues = 288;
// A VBRI tag is always 32 bytes after the header.
constexpr std::size_t kVbriOffset = kFrameHeader + 32;
// Bytes of a frame read at Depth::Layout to spot an info tag.
constexpr std::size_t kInfoProbe = kVbriOffset + 4;
constexpr std::size_t kLameTag = 36;
constexpr std::size_t kTocSize = 100;

enum class Version : std::uint8_t { Mpeg25 = 0, Mpeg2 = 2, Mpeg1 = 3 };

constexpr std::array<std::array<std::uint16_t, 16>, 2> kBitrates = {{
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},  // MPEG-1 Layer III
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},     // MPEG-2 and 2.5 Layer III
}};
constexpr std::array<std::uint32_t, 3> kSampleRates = {44100, 48000, 32000};

struct FrameHeader {
    std::uint32_t bits = 0;
    Version version = Version::Mpeg1;
    bool crc = false;
    std::uint32_t bitrate = 0;  // kbit/s
    std::uint32_t sampleRate = 0;
    bool mono = false;
    std::uint32_t length = 0;
    std::uint32_t sideInfo = 0;

    [[nodiscard]] std::uint32_t key() const noexcept { return (bits & kStreamMask) | (mono ? 1U : 0U); }
    [[nodiscard]] bool mpeg1() const noexcept { return version == Version::Mpeg1; }
    [[nodiscard]] std::size_t sideInfoOffset() const noexcept { return kFrameHeader + (crc ? kCrcSize : 0); }
    [[nodiscard]] std::size_t mainDataOffset() const noexcept { return sideInfoOffset() + sideInfo; }
    [[nodiscard]] std::uint32_t mainDataSize() const noexcept {
        return length - static_cast<std::uint32_t>(mainDataOffset());
    }
    // Readers look for a Xing or Info tag right after the side information,
    // whether or not the frame has a CRC.
    [[nodiscard]] std::size_t xingOffset() const noexcept { return kFrameHeader + sideInfo; }
};

std::optional<FrameHeader> parseHeader(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kFrameHeader) {
        return std::nullopt;
    }
    FrameHeader header;
    header.bits = loadBe32(bytes, 0);
    const std::uint32_t version = (header.bits >> 19) & 3;
    const std::uint32_t layer = (header.bits >> 17) & 3;
    const std::uint32_t bitrateIndex = (header.bits >> 12) & 0xF;
    const std::uint32_t rateIndex = (header.bits >> 10) & 3;
    const std::uint32_t padding = (header.bits >> 9) & 1;
    const std::uint32_t mode = (header.bits >> 6) & 3;
    const std::uint32_t emphasis = header.bits & 3;
    // Layer III only (layer bits 01); no free-format bitrate, no reserved values.
    if ((header.bits >> 21) != 0x7FF || version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 ||
        rateIndex == 3 || emphasis == 2) {
        return std::nullopt;
    }
    header.version = static_cast<Version>(version);
    header.crc = ((header.bits >> 16) & 1) == 0;
    header.mono = mode == 3;
    const std::uint32_t shift = header.version == Version::Mpeg1 ? 0 : header.version == Version::Mpeg2 ? 1 : 2;
    header.sampleRate = kSampleRates[rateIndex] >> shift;
    header.bitrate = kBitrates[header.mpeg1() ? 0 : 1][bitrateIndex];
    const std::uint32_t slots = header.mpeg1() ? 144 : 72;
    header.length = slots * header.bitrate * 1000 / header.sampleRate + padding;
    header.sideInfo = header.mpeg1() ? (header.mono ? 17U : 32U) : (header.mono ? 9U : 17U);
    if (header.length < header.mainDataOffset()) {
        return std::nullopt;
    }
    return header;
}

std::string versionName(Version version) {
    switch (version) {
    case Version::Mpeg1:
        return "MPEG-1";
    case Version::Mpeg2:
        return "MPEG-2";
    case Version::Mpeg25:
        break;
    }
    return "MPEG-2.5";
}

class BitReader {
public:
    explicit BitReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    // The next `count` (at most 16) bits, most significant first; zeros past the end.
    std::uint32_t read(unsigned count) noexcept {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i, ++position_) {
            const std::size_t byte = position_ / 8;
            const std::uint32_t bit =
                byte < bytes_.size() ? (static_cast<std::uint32_t>(bytes_[byte]) >> (7 - position_ % 8)) & 1U : 0U;
            value = (value << 1) | bit;
        }
        return value;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

struct SideInfo {
    std::uint32_t mainDataBegin = 0;
    // Bits of main data the frame's granules use (the part2_3_length fields).
    std::uint32_t mainDataBits = 0;
    // What is impossible in it, if anything.
    std::string problem;
};

SideInfo parseSideInfo(const FrameHeader& header, std::span<const std::byte> bytes) {
    BitReader bits(bytes);
    SideInfo side;
    const unsigned channels = header.mono ? 1 : 2;
    const unsigned granules = header.mpeg1() ? 2 : 1;
    if (header.mpeg1()) {
        side.mainDataBegin = bits.read(9);
        (void)bits.read(header.mono ? 5 : 3);  // private bits
        (void)bits.read(4 * channels);         // scale factor selection information
    } else {
        side.mainDataBegin = bits.read(8);
        (void)bits.read(header.mono ? 1 : 2);
    }
    for (unsigned granule = 0; granule < granules; ++granule) {
        for (unsigned channel = 0; channel < channels; ++channel) {
            side.mainDataBits += bits.read(12);  // part2_3_length
            const std::uint32_t bigValues = bits.read(9);
            (void)bits.read(8);                        // global_gain
            (void)bits.read(header.mpeg1() ? 4 : 9);   // scalefac_compress
            if (bits.read(1) != 0) {                   // window_switching_flag
                const std::uint32_t blockType = bits.read(2);
                (void)bits.read(1 + 10 + 9);  // mixed_block_flag, table_select, subblock_gain
                if (blockType == 0 && side.problem.empty()) {
                    side.problem = "a window switch to a normal block";
                }
            } else {
                (void)bits.read(15 + 4 + 3);  // table_select, region0_count, region1_count
            }
            (void)bits.read(header.mpeg1() ? 3 : 2);  // preflag (MPEG-1), scalefac_scale, count1table_select
            if (bigValues > kMaxBigValues && side.problem.empty()) {
                side.problem = "big_values beyond 288";
            }
        }
    }
    return side;
}

bool bytesAre(std::span<const std::byte> bytes, std::size_t offset, std::string_view text) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (bytes[offset + i] != static_cast<std::byte>(text[i])) {
            return false;
        }
    }
    return true;
}

enum class InfoKind : std::uint8_t { Xing, Info, Vbri };

// Whether the frame whose first bytes are `bytes` carries an info tag.
std::optional<InfoKind> infoTagIn(const FrameHeader& header, std::span<const std::byte> bytes) noexcept {
    if (bytesAre(bytes, header.xingOffset(), "Xing")) {
        return InfoKind::Xing;
    }
    if (bytesAre(bytes, header.xingOffset(), "Info")) {
        return InfoKind::Info;
    }
    if (header.length >= kVbriOffset + 4 && bytesAre(bytes, kVbriOffset, "VBRI")) {
        return InfoKind::Vbri;
    }
    return std::nullopt;
}

struct InfoTag {
    InfoKind kind = InfoKind::Xing;
    // Xing/Info: audio frames after the tag's frame, and bytes of the stream
    // including that frame (0 when not recorded).
    std::uint32_t frames = 0;
    std::uint32_t bytes = 0;
    // A LAME extension whose own CRC matched: its music length and CRC.
    bool lame = false;
    std::uint32_t musicLength = 0;
    std::uint16_t musicCrc = 0;
    // What is wrong with the tag, if anything.
    std::string problem;
};

// Reads the info tag of the stream's first frame (all of its bytes).
InfoTag parseInfoTag(InfoKind kind, const FrameHeader& header, std::span<const std::byte> frame) {
    InfoTag tag;
    tag.kind = kind;
    if (kind == InfoKind::Vbri) {
        return tag;  // its counts are not used (see docs/formats/audio.md)
    }
    std::size_t position = header.xingOffset() + 4;
    if (frame.size() < position + 4) {
        tag.problem = "the info tag does not fit in its frame";
        return tag;
    }
    const std::uint32_t flags = loadBe32(frame, position);
    position += 4;
    const std::size_t fields = ((flags & 1) != 0 ? 4U : 0U) + ((flags & 2) != 0 ? 4U : 0U) +
                               ((flags & 4) != 0 ? kTocSize : 0U) + ((flags & 8) != 0 ? 4U : 0U);
    if (frame.size() - position < fields) {
        tag.problem = "the info tag does not fit in its frame";
        return tag;
    }
    if ((flags & 1) != 0) {
        tag.frames = loadBe32(frame, position);
        position += 4;
    }
    if ((flags & 2) != 0) {
        tag.bytes = loadBe32(frame, position);
        position += 4;
    }
    if ((flags & 4) != 0) {
        for (std::size_t i = 1; i < kTocSize; ++i) {
            if (loadU8(frame, position + i) < loadU8(frame, position + i - 1)) {
                tag.problem = "the info tag's table of contents is not in order";
                break;
            }
        }
        position += kTocSize;
    }
    if ((flags & 8) != 0) {
        position += 4;
    }
    // The LAME extension, as LAME and FFmpeg write it: its CRC covers the frame up to the CRC.
    if (frame.size() - position >= kLameTag) {
        const std::uint16_t stored = loadBe16(frame, position + 34);
        if (detail::crc16Arc(frame.first(position + 34)) == stored) {
            tag.lame = true;
            tag.musicLength = loadBe32(frame, position + 28);
            tag.musicCrc = loadBe16(frame, position + 32);
        } else if ((bytesAre(frame, position, "LAME") || bytesAre(frame, position, "Lavc") ||
                    bytesAre(frame, position, "Lavf")) &&
                   tag.problem.empty()) {
            tag.problem = "the LAME tag's CRC does not match";
        }
    }
    return tag;
}

// Walks an MP3 from its first byte. See structure_walk.hpp for what a walk reports.
class Mp3Walker {
public:
    Mp3Walker(IContentReader& content, Depth depth) : content_(content), depth_(depth) {}

    Result<Walk> run();

private:
    void checkFrame(std::uint64_t offset, const FrameHeader& header, std::span<const std::byte> frame);
    void checkInfoTag(std::uint64_t streamEnd);
    [[nodiscard]] std::uint64_t audioFrames() const noexcept { return frames_ - (info_.has_value() ? 1 : 0); }
    [[nodiscard]] std::string summary() const;

    IContentReader& content_;
    Depth depth_;
    Walk walk_;
    std::optional<FrameHeader> first_;
    std::optional<InfoTag> info_;
    std::uint64_t streamStart_ = 0;
    // Frames of the stream, an info tag's frame included.
    std::uint64_t frames_ = 0;
    std::uint64_t kilobits_ = 0;
    std::uint16_t musicCrc_ = 0;
    // The bit reservoir, in bytes of main data (headers and side information
    // left out): where the current frame's main data area starts, and where
    // the previous frame's main data ended.
    std::int64_t areaStart_ = 0;
    std::optional<std::int64_t> previousDataEnd_;
};

void Mp3Walker::checkFrame(std::uint64_t offset, const FrameHeader& header, std::span<const std::byte> frame) {
    const std::string where = "frame " + std::to_string(frames_ + 1) + ": ";
    if (header.crc) {
        std::uint16_t crc = detail::crc16Mpeg(frame.subspan(2, 2));
        crc = detail::crc16Mpeg(frame.subspan(header.sideInfoOffset(), header.sideInfo), crc);
        if (crc != loadBe16(frame, kFrameHeader)) {
            walk_.noteProblem(offset, where + "CRC mismatch");
        }
    }
    if (frames_ == 0 && info_.has_value()) {
        return;  // the info tag's frame holds no audio
    }
    musicCrc_ = detail::crc16Arc(frame, musicCrc_);
    const SideInfo side = parseSideInfo(header, frame.subspan(header.sideInfoOffset(), header.sideInfo));
    if (!side.problem.empty()) {
        walk_.noteProblem(offset, where + side.problem);
    }
    // Main data: from mainDataBegin bytes before this frame's area, never
    // overlapping the previous frame's, never beyond this frame's area. An
    // encoder starts a stream with an empty reservoir, so the first audio
    // frame pointing back means the stream's start is missing (the tail of a
    // stream whose first frames were lost, or a stream cut from a longer one).
    const std::int64_t start = areaStart_ - std::int64_t{side.mainDataBegin};
    const std::int64_t end = start + (std::int64_t{side.mainDataBits} + 7) / 8;
    const std::int64_t areaEnd = areaStart_ + std::int64_t{header.mainDataSize()};
    if (!previousDataEnd_.has_value() && side.mainDataBegin != 0) {
        walk_.noteProblem(offset, where + "main data begins before the stream: its start is missing");
    }
    if (previousDataEnd_.has_value() && start < *previousDataEnd_) {
        walk_.noteProblem(offset, where + "main data overlaps the previous frame's");
    }
    if (end > areaEnd) {
        walk_.noteProblem(offset, where + "main data runs beyond the frame");
    }
    previousDataEnd_ = end;
    areaStart_ = areaEnd;
}

void Mp3Walker::checkInfoTag(std::uint64_t streamEnd) {
    if (!info_.has_value()) {
        return;
    }
    const InfoTag& tag = *info_;
    const std::uint64_t streamBytes = streamEnd - streamStart_;
    if (!tag.problem.empty()) {
        walk_.noteProblem(streamStart_, tag.problem);
    }
    if (tag.bytes != 0 && tag.bytes != streamBytes) {
        walk_.noteProblem(streamStart_, "the info tag records " + std::to_string(tag.bytes) + " bytes, the stream has " +
                                            std::to_string(streamBytes));
    }
    if (tag.lame) {
        if (tag.musicLength != 0 && tag.musicLength != streamBytes) {
            walk_.noteProblem(streamStart_, "the LAME tag records " + std::to_string(tag.musicLength) +
                                                " bytes of music, the stream has " + std::to_string(streamBytes));
        }
        if (tag.musicCrc != musicCrc_) {
            walk_.noteProblem(streamStart_, "the LAME tag's music CRC does not match the frames");
        }
    }
}

std::string Mp3Walker::summary() const {
    std::string text = versionName(first_->version) + " Layer III, " + std::to_string(first_->sampleRate) + " Hz, " +
                       (first_->mono ? "mono, " : "stereo, ") + std::to_string(audioFrames()) + " frames";
    if (audioFrames() > 0) {
        text += ", " + std::to_string(kilobits_ / frames_) + " kbit/s";
    }
    if (info_.has_value()) {
        text += info_->kind == InfoKind::Xing ? ", Xing tag" : info_->kind == InfoKind::Info ? ", Info tag" : ", VBRI tag";
        if (info_->lame) {
            text += " with LAME extension";
        }
    }
    return text;
}

Result<Walk> Mp3Walker::run() {
    const std::uint64_t size = content_.size();
    Result<detail::TagRun> leading = detail::skipLeadingTags(content_, 0, depth_, walk_);
    if (!leading.ok()) {
        return leading.error();
    }
    if (leading->truncated) {
        return walk_.finish(WalkStatus::Truncated, leading->end, "the data ends inside the leading ID3v2 tags");
    }
    streamStart_ = leading->end;
    std::uint64_t position = streamStart_;
    // The data ends inside a frame. Before one whole frame, and with no
    // ID3v2 tag before it, nothing confirms the lone header: not a file.
    const auto cutOff = [&](std::uint64_t at, std::string detail) -> Walk& {
        if (frames_ == 0 && streamStart_ == 0) {
            return walk_.finish(WalkStatus::Broken, 0, "a lone frame header, cut off by the end of the data");
        }
        return walk_.finish(WalkStatus::Truncated, at, std::move(detail));
    };
    bool dataEnded = false;
    std::string stop = "the next bytes are not a frame of the stream";
    for (;;) {
        if (info_.has_value() && info_->frames != 0 && audioFrames() == info_->frames) {
            stop = "the info tag's frame count";
            break;
        }
        const std::uint64_t remaining = size - position;
        if (remaining < kFrameHeader) {
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
        Result<std::span<const std::byte>> bytes = content_.read(position, kFrameHeader);
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
        // The whole frame at Depth::Full and for the first frame (its info tag); enough to spot an info tag otherwise.
        const bool whole = depth_ == Depth::Full || frames_ == 0;
        Result<std::span<const std::byte>> frame =
            content_.read(position, whole ? header->length : std::min<std::size_t>(header->length, kInfoProbe));
        if (!frame.ok()) {
            return frame.error();
        }
        const std::optional<InfoKind> tag = infoTagIn(*header, *frame);
        if (frames_ == 0) {
            first_ = header;
            if (tag.has_value()) {
                info_ = parseInfoTag(*tag, *header, *frame);
            }
        } else if (tag.has_value()) {
            stop = "another file's info tag";
            break;
        }
        if (depth_ == Depth::Full) {
            checkFrame(position, *header, *frame);
        }
        kilobits_ += header->bitrate;
        ++frames_;
        position += header->length;
    }

    if (frames_ == 0) {
        if (dataEnded) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends before the first frame");
        }
        return walk_.finish(WalkStatus::Broken, 0,
                            streamStart_ > 0 ? "no MPEG Layer III frame after the ID3v2 tag" : "no MPEG Layer III frame");
    }
    if (frames_ < Mp3Format::kMinimumFrames) {
        if (dataEnded) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends after " + std::to_string(frames_) +
                                                                     " frames");
        }
        return walk_.finish(WalkStatus::Broken, 0, "only " + std::to_string(frames_) + " consecutive frames");
    }
    if (info_.has_value() && info_->frames != 0 && audioFrames() < info_->frames) {
        const std::string count = std::to_string(audioFrames()) + " of the " + std::to_string(info_->frames) +
                                  " frames the info tag records";
        if (dataEnded) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends after " + count);
        }
        return walk_.finish(WalkStatus::Broken, position, "the stream stops after " + count + " (" + stop + ")");
    }
    const std::uint64_t streamEnd = position;
    Result<detail::TagRun> trailing = detail::skipTrailingTags(content_, streamEnd, depth_, walk_);
    if (!trailing.ok()) {
        return trailing.error();
    }
    if (trailing->truncated) {
        return walk_.finish(WalkStatus::Truncated, trailing->end, "the data ends inside a tag after the frames");
    }
    if (depth_ == Depth::Full) {
        checkInfoTag(streamEnd);
    }
    std::string text = summary();
    if (leading->count + trailing->count > 0) {
        text += ", " + std::to_string(leading->count + trailing->count) + " tags";
    }
    return walk_.finish(WalkStatus::Complete, trailing->end, std::move(text));
}

Result<Walk> walkMp3(IContentReader& content, Depth depth) {
    return Mp3Walker(content, depth).run();
}

}  // namespace

Mp3Format::Mp3Format() {
    descriptor_.id = "mp3";
    descriptor_.name = "MP3 audio";
    descriptor_.extension = "mp3";
    descriptor_.signatures = {
        carving::textSignature("ID3v2 tag", "ID3"),
        carving::byteSignature("MPEG-1 Layer III frame", {0xFF, 0xFA}, 0, {0xFF, 0xFE}),
        carving::byteSignature("MPEG-2 Layer III frame", {0xFF, 0xF2}, 0, {0xFF, 0xFE}),
        carving::byteSignature("MPEG-2.5 Layer III frame", {0xFF, 0xE2}, 0, {0xFF, 0xFE}),
    };
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::StructureWalk;
    descriptor_.selfSynchronizing = true;
}

HeaderCheck Mp3Format::checkHeader(std::span<const std::byte> header) const {
    if (bytesAre(header, 0, "ID3")) {
        return detail::parseId3v2Header(header).has_value() ? HeaderCheck::accept()
                                                             : HeaderCheck::reject("not an ID3v2 header");
    }
    const std::optional<FrameHeader> first = parseHeader(header);
    if (!first.has_value()) {
        return HeaderCheck::reject("not an MPEG Layer III frame header");
    }
    if (header.size() < first->mainDataOffset()) {
        return HeaderCheck::reject("the data ends inside the first frame's side information");
    }
    if (!infoTagIn(*first, header).has_value()) {
        const SideInfo side = parseSideInfo(*first, header.subspan(first->sideInfoOffset(), first->sideInfo));
        if (!side.problem.empty()) {
            return HeaderCheck::reject("impossible side information: " + side.problem);
        }
        // A frame whose main data begins in an earlier frame is accepted: after
        // a break, the rest of a stream starts there (validation reports it).
        if (side.mainDataBits > 8ULL * (std::uint64_t{side.mainDataBegin} + first->mainDataSize())) {
            return HeaderCheck::reject("the frame's main data does not fit in it");
        }
    }
    // The frames that follow, as far as the header holds their headers.
    std::uint64_t position = first->length;
    std::uint64_t frames = 1;
    while (frames < kMinimumFrames && position + kFrameHeader <= header.size()) {
        const std::optional<FrameHeader> next = parseHeader(header.subspan(static_cast<std::size_t>(position)));
        if (!next.has_value() || next->key() != first->key()) {
            return HeaderCheck::reject("only " + std::to_string(frames) + " consecutive frames");
        }
        position += next->length;
        ++frames;
    }
    return HeaderCheck::accept();
}

Result<EndDetection> Mp3Format::findEnd(IContentReader& content) const {
    Result<Walk> walk = walkMp3(content, Depth::Layout);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::endOf(*walk, content.size());
}

Result<ValidationResult> Mp3Format::validate(IContentReader& content) const {
    Result<Walk> walk = walkMp3(content, Depth::Full);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::verdictOf(*walk, content.size());
}

}  // namespace recovery::formats
