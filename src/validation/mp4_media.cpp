// Media validation of MP4 (MOV, M4V, 3GP) and M4A files: every sample of
// every audio and video track, decoded as far as the engine's own readers
// go without the codecs' tables (media_validator.hpp):
//
//  * 'mp4a' with an ES descriptor of MPEG-4 audio or MPEG-2 AAC: the
//    AudioSpecificConfig (aac_syntax.hpp), then every sample as one raw
//    data block, which ID_END must end. Without a decoder configuration
//    the samples are read as AAC LC of the sample description's channels
//    and rate, as FFmpeg plays them;
//  * 'avc1' to 'avc4' with avcC, 'hvc1' and 'hev1' with hvcC: the parameter
//    sets of the configuration, then the NAL units of every sample
//    (avc_syntax.hpp, hevc_syntax.hpp), each also scanned for the byte
//    sequences a NAL unit may not hold (nal_units.hpp). This reads all of
//    a video track's media data;
//  * PCM sound ('raw ', 'twos', 'sowt', 'lpcm', 'ulaw', ...) has nothing
//    coded; other codecs (ALAC, MPEG-4 Part 2, AC-3, Opus, MP3 in MP4,
//    encrypted tracks, ...) are not decoded.
//
// A track stops at its first problem. The file's level is the first failed
// track; else the first track whose samples run past the data (Truncated);
// else Passed, with Partial coverage, when any track was checked.

#include "aac_syntax.hpp"
#include "avc_syntax.hpp"
#include "hevc_syntax.hpp"
#include "media_decoders.hpp"
#include "nal_units.hpp"

#include "formats/mp4_box.hpp"
#include "formats/mp4_parser.hpp"
#include "recovery/byte_order.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace recovery::validation::detail {

namespace {

namespace mp4 = formats::mp4;
using mp4::FourCc;

constexpr std::size_t kMaxRead = carving::IContentReader::kMaxReadLength;
// Fixed fields of a sample entry after its box header: VisualSampleEntry,
// and AudioSampleEntry of version 0, 1 (QuickTime) and 2 (QuickTime).
constexpr std::uint64_t kVisualFields = 78;
constexpr std::uint64_t kAudioFields = 28;
constexpr std::uint64_t kAudioFieldsV1 = 44;
constexpr std::uint64_t kAudioFieldsV2 = 64;

struct Problem {
    std::uint64_t offset = 0;
    std::string detail;
};

std::vector<std::uint8_t> toBytes(std::span<const std::byte> bytes) {
    std::vector<std::uint8_t> out(bytes.size());
    std::transform(bytes.begin(), bytes.end(), out.begin(),
                   [](std::byte value) { return std::to_integer<std::uint8_t>(value); });
    return out;
}

// Checks the samples of one sample description, in decoding order.
class SampleChecker {
public:
    SampleChecker() = default;
    SampleChecker(const SampleChecker&) = delete;
    SampleChecker& operator=(const SampleChecker&) = delete;
    SampleChecker(SampleChecker&&) = delete;
    SampleChecker& operator=(SampleChecker&&) = delete;
    virtual ~SampleChecker() = default;

    // The sample at content offset `offset` of `size` bytes, all inside the content.
    [[nodiscard]] virtual Result<std::optional<Problem>> check(carving::IContentReader& content, std::uint64_t offset,
                                                               std::uint32_t size) = 0;
    [[nodiscard]] virtual std::string summary() const = 0;
};

class AacChecker final : public SampleChecker {
public:
    explicit AacChecker(const aac::Config& config) : config_(config) {}

    [[nodiscard]] Result<std::optional<Problem>> check(carving::IContentReader& content, std::uint64_t offset,
                                                       std::uint32_t size) override {
        if (size > kMaxRead) {
            return std::optional<Problem>{
                Problem{offset, "an AAC sample of " + describeBytes(size) + ", larger than a raw data block can be"}};
        }
        Result<std::span<const std::byte>> read = content.read(offset, size);
        if (!read.ok()) {
            return read.error();
        }
        const std::vector<std::uint8_t> bytes = toBytes(*read);
        const std::uint64_t end = std::uint64_t{size} * 8;
        const aac::Block block = aac::readRawDataBlock(bytes, 0, end, config_);
        switch (block.status) {
        case aac::BlockStatus::Invalid:
            return std::optional<Problem>{Problem{offset + block.at / 8, block.detail}};
        case aac::BlockStatus::Complete: {
            const std::uint64_t aligned = (block.end + 7) / 8 * 8;
            if (aligned != end) {
                const std::uint64_t left = (end - aligned) / 8;
                return std::optional<Problem>{Problem{offset + aligned / 8,
                                                      "ID_END ends the raw data block " + std::to_string(left) +
                                                          (left == 1 ? " byte" : " bytes") +
                                                          " before the end of the sample"}};
            }
            ++complete_;
            break;
        }
        case aac::BlockStatus::Stopped:
            ++stopped_;
            break;
        }
        return std::optional<Problem>{};
    }

    [[nodiscard]] std::string summary() const override {
        return "raw data blocks read through ID_END in " + std::to_string(complete_) + ", up to coded data in " +
               std::to_string(stopped_);
    }

private:
    aac::Config config_;
    std::uint64_t complete_ = 0;
    std::uint64_t stopped_ = 0;
};

// AVC and HEVC: a length field before each NAL unit (avc::Stream, hevc::Stream).
template <typename Stream>
class NalChecker final : public SampleChecker {
public:
    NalChecker(std::unique_ptr<Stream> stream, std::uint32_t lengthSize)
        : stream_(std::move(stream)), lengthSize_(lengthSize) {}

    [[nodiscard]] Result<std::optional<Problem>> check(carving::IContentReader& content, std::uint64_t offset,
                                                       std::uint32_t size) override {
        std::uint64_t position = 0;
        std::uint32_t index = 0;
        while (position < size) {
            ++index;
            const std::string where = "NAL unit " + std::to_string(index) + ": ";
            if (size - position < lengthSize_) {
                return problem(offset + position, where + "the sample ends inside its length field");
            }
            Result<std::span<const std::byte>> field = content.read(offset + position, lengthSize_);
            if (!field.ok()) {
                return field.error();
            }
            std::uint64_t length = 0;
            for (const std::byte value : *field) {
                length = (length << 8) | std::to_integer<std::uint8_t>(value);
            }
            position += lengthSize_;
            if (length > size - position) {
                return problem(offset + position, where + "a length of " + std::to_string(length) +
                                                      " bytes runs past the end of the sample");
            }
            const std::uint64_t unit = offset + position;
            if (length == 0) {
                if (std::optional<std::string> wrong = stream_->nalUnit({}, 0)) {
                    return problem(unit, where + *wrong);
                }
                continue;
            }
            Result<std::span<const std::byte>> first = content.read(unit, 1);
            if (!first.ok()) {
                return first.error();
            }
            const auto header = std::to_integer<std::uint8_t>((*first)[0]);
            const std::string what = "NAL unit " + std::to_string(index) + " (" + Stream::kindOf(header) + "): ";
            const std::uint64_t headSize = Stream::headBytes(header, length);
            Result<std::span<const std::byte>> head = content.read(unit, static_cast<std::size_t>(headSize));
            if (!head.ok()) {
                return head.error();
            }
            head_ = toBytes(*head);
            nal::SequenceScan scan;
            scan.feed(head_, unit);
            if (scan.violation().has_value()) {
                return problem(scan.violation()->offset, what + scan.violation()->detail);
            }
            if (std::optional<std::string> wrong = stream_->nalUnit(head_, length)) {
                return problem(unit, what + *wrong);
            }
            for (std::uint64_t done = headSize; done < length && !scan.violation().has_value();) {
                const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(length - done, kMaxRead));
                Result<std::span<const std::byte>> rest = content.read(unit + done, chunk);
                if (!rest.ok()) {
                    return rest.error();
                }
                scan.feed(*rest, unit + done);
                done += chunk;
            }
            if (scan.violation().has_value()) {
                return problem(scan.violation()->offset, what + scan.violation()->detail);
            }
            position += length;
        }
        return std::optional<Problem>{};
    }

    [[nodiscard]] std::string summary() const override { return stream_->summary(); }

private:
    static Result<std::optional<Problem>> problem(std::uint64_t offset, std::string detail) {
        return std::optional<Problem>{Problem{offset, std::move(detail)}};
    }

    std::unique_ptr<Stream> stream_;
    std::uint32_t lengthSize_;
    std::vector<std::uint8_t> head_;
};

// What one sample description's samples are checked with, or why they are not.
struct DescriptionCheck {
    std::unique_ptr<SampleChecker> checker;
    // Without a checker: Unsupported, NotApplicable or Failed (the configuration).
    LevelStatus status = LevelStatus::Unsupported;
    std::string detail;
    // The codec as summaries name it: "mp4a, AAC LC, 44100 Hz, 2 channels".
    std::string label;
};

DescriptionCheck without(LevelStatus status, std::string label, std::string detail) {
    DescriptionCheck check;
    check.status = status;
    check.label = std::move(label);
    check.detail = std::move(detail);
    return check;
}

// The first box of `type` in [begin, end), looking into a QuickTime 'wave'
// box too when `intoWave`.
Result<std::optional<mp4::BoxHeader>> findChild(carving::IContentReader& content, std::uint64_t begin,
                                                std::uint64_t end, FourCc type, bool intoWave) {
    mp4::BoxSequence boxes(content, begin, end);
    for (;;) {
        Result<std::optional<mp4::BoxRead>> next = boxes.next();
        if (!next.ok()) {
            return next.error();
        }
        if (!next->has_value() || (*next)->status != mp4::BoxStatus::Valid) {
            return std::optional<mp4::BoxHeader>{};
        }
        const mp4::BoxHeader box = (*next)->header;
        if (box.type == type) {
            return std::optional<mp4::BoxHeader>{box};
        }
        if (intoWave && box.type == FourCc("wave")) {
            Result<std::optional<mp4::BoxHeader>> inner =
                findChild(content, box.payloadOffset(), box.end(), type, false);
            if (!inner.ok() || inner->has_value()) {
                return inner;
            }
        }
    }
}

// The payload of `box` (at most kMaxRead bytes; nothing for a larger one).
Result<std::optional<std::vector<std::uint8_t>>> payloadOf(carving::IContentReader& content,
                                                           const mp4::BoxHeader& box, std::uint64_t skip = 0) {
    if (box.payloadSize() < skip || box.payloadSize() - skip > kMaxRead) {
        return std::optional<std::vector<std::uint8_t>>{};
    }
    Result<std::span<const std::byte>> read =
        content.read(box.payloadOffset() + skip, static_cast<std::size_t>(box.payloadSize() - skip));
    if (!read.ok()) {
        return read.error();
    }
    return std::optional<std::vector<std::uint8_t>>{toBytes(*read)};
}

struct EsInfo {
    bool decoderConfig = false;
    std::uint8_t objectType = 0;
    std::optional<std::vector<std::uint8_t>> specificInfo;
};

// The ES_Descriptor of an esds box (after its version and flags), read as
// FFmpeg reads it: an ES_Descriptor (or just an ES_ID), then a
// DecoderConfigDescriptor with its DecoderSpecificInfo. A missing
// DecoderConfigDescriptor is not an error (FFmpeg goes on without one).
std::optional<std::string> readEsds(std::span<const std::uint8_t> bytes, EsInfo& info) {
    std::size_t position = 0;
    const auto descriptor = [&](std::uint8_t& tag, std::uint64_t& length) {
        if (position >= bytes.size()) {
            return false;
        }
        tag = bytes[position++];
        length = 0;
        for (int i = 0; i < 4; ++i) {
            if (position >= bytes.size()) {
                return false;
            }
            const std::uint8_t value = bytes[position++];
            length = (length << 7) | (value & 0x7FU);
            if ((value & 0x80U) == 0) {
                break;
            }
        }
        return true;
    };
    std::uint8_t tag = 0;
    std::uint64_t length = 0;
    if (!descriptor(tag, length)) {
        return "the esds box ends inside its ES_Descriptor";
    }
    if (tag == 0x03) {
        if (bytes.size() - position < 3) {
            return "the esds box ends inside its ES_Descriptor";
        }
        const std::uint8_t flags = bytes[position + 2];
        position += 3;
        if ((flags & 0x80U) != 0) {  // streamDependenceFlag
            position += 2;
        }
        if ((flags & 0x40U) != 0) {  // URL_Flag
            if (position >= bytes.size()) {
                return "the esds box ends inside its ES_Descriptor";
            }
            position += 1 + std::size_t{bytes[position]};
        }
        if ((flags & 0x20U) != 0) {  // OCRstreamFlag
            position += 2;
        }
    } else {
        position += 2;  // ES_ID
    }
    if (position > bytes.size() || !descriptor(tag, length) || tag != 0x04) {
        return std::nullopt;
    }
    if (bytes.size() - position < 13) {
        return "the esds box ends inside its DecoderConfigDescriptor";
    }
    info.decoderConfig = true;
    info.objectType = bytes[position];
    position += 13;
    if (!descriptor(tag, length) || tag != 0x05) {
        return std::nullopt;
    }
    if (length > bytes.size() - position) {
        return "the esds box ends inside its DecoderSpecificInfo";
    }
    info.specificInfo.emplace(bytes.begin() + static_cast<std::ptrdiff_t>(position),
                              bytes.begin() + static_cast<std::ptrdiff_t>(position + length));
    return std::nullopt;
}

bool isPcm(FourCc format) {
    static constexpr std::array<FourCc, 13> kPcm = {FourCc("raw "), FourCc("twos"), FourCc("sowt"), FourCc("lpcm"),
                                                    FourCc("in24"), FourCc("in32"), FourCc("fl32"), FourCc("fl64"),
                                                    FourCc("NONE"), FourCc("ipcm"), FourCc("fpcm"), FourCc("ulaw"),
                                                    FourCc("alaw")};
    return std::find(kPcm.begin(), kPcm.end(), format) != kPcm.end();
}

std::string channels(std::uint32_t count) {
    return std::to_string(count) + (count == 1 ? " channel" : " channels");
}

Result<DescriptionCheck> aacDescription(carving::IContentReader& content, const mp4::SampleDescription& description,
                                        std::uint64_t payload) {
    std::string label = "mp4a";
    const std::uint64_t end = description.offset + description.size;
    std::uint32_t version = 0;
    if (payload + 10 <= end) {
        Result<std::span<const std::byte>> versionField = content.read(payload + 8, 2);
        if (!versionField.ok()) {
            return versionField.error();
        }
        version = loadBe16(*versionField, 0);
    }
    const std::uint64_t fields = version == 1 ? kAudioFieldsV1 : version == 2 ? kAudioFieldsV2 : kAudioFields;
    EsInfo info;
    if (payload + fields <= end) {
        Result<std::optional<mp4::BoxHeader>> esds = findChild(content, payload + fields, end, FourCc("esds"), true);
        if (!esds.ok()) {
            return esds.error();
        }
        if (esds->has_value()) {
            Result<std::optional<std::vector<std::uint8_t>>> bytes = payloadOf(content, **esds, 4);
            if (!bytes.ok()) {
                return bytes.error();
            }
            if (!bytes->has_value()) {
                return without(LevelStatus::Failed, label,
                               "an esds box shorter than its version and flags, or larger than 1 MiB");
            }
            if (std::optional<std::string> problem = readEsds(**bytes, info)) {
                return without(LevelStatus::Failed, label, *problem);
            }
        }
    }
    if (info.decoderConfig) {
        switch (info.objectType) {
        case 0x40:  // MPEG-4 audio
        case 0x66:  // MPEG-2 AAC Main
        case 0x67:  // MPEG-2 AAC LC
        case 0x68:  // MPEG-2 AAC SSR
            break;
        case 0x69:
        case 0x6B:
            return without(LevelStatus::Unsupported, label + ", MPEG audio", "MPEG audio (MP3) is not decoded");
        default:
            return without(LevelStatus::Unsupported, label,
                           "object type indication " + std::to_string(info.objectType) + " is not decoded");
        }
    }
    aac::Config config;
    if (info.specificInfo.has_value()) {
        const aac::AscRead asc = aac::readAudioSpecificConfig(*info.specificInfo);
        if (!asc.config.has_value()) {
            return without(asc.unsupported ? LevelStatus::Unsupported : LevelStatus::Failed, label,
                           "AudioSpecificConfig: " + asc.problem);
        }
        config = asc.config->core;
        label += ", " + aac::objectTypeName(asc.config->audioObjectType) + ", " +
                 std::to_string(asc.config->samplingFrequency) + " Hz";
        if (config.channelConfig != 0) {
            label += ", " + channels(config.channelConfig == 7 ? 8 : config.channelConfig);
        }
    } else {
        // FFmpeg plays AAC without a configuration as AAC LC of the sample
        // description's channels and rate.
        if (info.objectType == 0x68) {
            return without(LevelStatus::Unsupported, label, "AAC SSR is not read");
        }
        if (description.sampleRate == 0) {
            return without(LevelStatus::Failed, label,
                           "no AudioSpecificConfig, and no sample rate in the sample description");
        }
        config.objectType = info.objectType == 0x66 ? aac::kAacMain : aac::kAacLc;
        config.samplingIndex = aac::samplingIndexOf(description.sampleRate);
        const std::uint32_t count = description.channelCount;
        config.channelConfig = count >= 1 && count <= 6 ? count : count == 8 ? 7 : 0;
        label += ", " + aac::objectTypeName(config.objectType) + " without AudioSpecificConfig, " +
                 std::to_string(description.sampleRate) + " Hz, " + channels(count);
    }
    DescriptionCheck check;
    check.checker = std::make_unique<AacChecker>(config);
    check.label = std::move(label);
    return check;
}

std::string profileOf(const avc::Sps& sps) {
    return avc::profileName(sps.profileIdc);
}

std::string profileOf(const hevc::Sps& sps) {
    return hevc::profileName(sps.profileIdc);
}

template <typename Stream, typename Read>
Result<DescriptionCheck> nalDescription(carving::IContentReader& content, const mp4::SampleDescription& description,
                                        std::uint64_t payload, FourCc configType, const char* codec, Read readConfig) {
    std::string label = description.format.text() + ", " + codec;
    const std::string size = description.width != 0 && description.height != 0
                                 ? ", " + std::to_string(description.width) + "x" + std::to_string(description.height)
                                 : std::string();
    const std::uint64_t end = description.offset + description.size;
    std::optional<mp4::BoxHeader> box;
    if (payload + kVisualFields <= end) {
        Result<std::optional<mp4::BoxHeader>> found =
            findChild(content, payload + kVisualFields, end, configType, false);
        if (!found.ok()) {
            return found.error();
        }
        box = *found;
    }
    if (!box.has_value()) {
        return without(LevelStatus::Failed, label + size,
                       "the sample description holds no '" + configType.text() + "' box");
    }
    Result<std::optional<std::vector<std::uint8_t>>> bytes = payloadOf(content, *box);
    if (!bytes.ok()) {
        return bytes.error();
    }
    if (!bytes->has_value()) {
        return without(LevelStatus::Unsupported, label + size, "a '" + configType.text() + "' box larger than 1 MiB");
    }
    const auto read = readConfig(**bytes);
    if (!read.config.has_value()) {
        return without(read.unsupported ? LevelStatus::Unsupported : LevelStatus::Failed, label + size, read.problem);
    }
    auto stream = std::make_unique<Stream>();
    if (std::optional<std::string> problem = stream->configure(*read.config)) {
        return without(LevelStatus::Failed, label + size, *problem);
    }
    if (const auto& sps = stream->firstSps(); sps.has_value()) {
        label += " " + profileOf(*sps);
    }
    label += size;
    DescriptionCheck check;
    check.checker = std::make_unique<NalChecker<Stream>>(std::move(stream), read.config->lengthSize);
    check.label = std::move(label);
    return check;
}

Result<DescriptionCheck> describe(carving::IContentReader& content, const mp4::Track& track,
                                  const mp4::SampleDescription& description) {
    const FourCc format = description.format;
    const std::string name = format.text();
    Result<mp4::BoxRead> entry = mp4::readBoxHeader(content, description.offset, description.offset + description.size);
    if (!entry.ok()) {
        return entry.error();
    }
    const std::uint64_t payload = entry->header.payloadOffset();
    if (track.kind == mp4::TrackKind::Audio) {
        if (format == FourCc("mp4a")) {
            return aacDescription(content, description, payload);
        }
        if (isPcm(format)) {
            return without(LevelStatus::NotApplicable, name, "PCM sound: nothing is coded");
        }
        if (format == FourCc("enca")) {
            return without(LevelStatus::Unsupported, name, "encrypted sound is not decoded");
        }
        return without(LevelStatus::Unsupported, name, "'" + name + "' sound is not decoded");
    }
    if (format == FourCc("avc1") || format == FourCc("avc2") || format == FourCc("avc3") || format == FourCc("avc4")) {
        return nalDescription<avc::Stream>(content, description, payload, FourCc("avcC"), "AVC",
                                           [](std::span<const std::uint8_t> bytes) {
                                               return avc::readDecoderConfig(bytes);
                                           });
    }
    if (format == FourCc("hvc1") || format == FourCc("hev1")) {
        return nalDescription<hevc::Stream>(content, description, payload, FourCc("hvcC"), "HEVC",
                                            [](std::span<const std::uint8_t> bytes) {
                                                return hevc::readDecoderConfig(bytes);
                                            });
    }
    if (format == FourCc("encv")) {
        return without(LevelStatus::Unsupported, name, "encrypted video is not decoded");
    }
    if (format == FourCc("mp4v")) {
        return without(LevelStatus::Unsupported, name, "MPEG-4 Part 2 video is not decoded");
    }
    return without(LevelStatus::Unsupported, name, "'" + name + "' video is not decoded");
}

struct TrackOutcome {
    LevelStatus status = LevelStatus::NotApplicable;
    std::optional<std::uint64_t> offset;
    std::string detail;
};

Result<TrackOutcome> checkTrack(carving::IContentReader& content, const mp4::Track& track, std::size_t number) {
    const std::string name = "track " + std::to_string(number);
    if (track.totalSamples() == 0) {
        return TrackOutcome{LevelStatus::NotApplicable, std::nullopt, name + ": no samples"};
    }
    const std::vector<mp4::SampleDescription>& descriptions = track.samples.descriptions;
    std::vector<std::optional<DescriptionCheck>> checks(descriptions.size());
    std::uint64_t checked = 0;
    std::uint64_t skipped = 0;
    std::optional<TrackOutcome> outcome;

    const auto visit = [&](std::uint32_t descriptionIndex, std::uint64_t sample, std::uint64_t offset,
                           std::uint32_t size) -> Status {
        if (descriptionIndex == 0 || descriptionIndex > descriptions.size()) {
            outcome = TrackOutcome{LevelStatus::Failed, offset,
                                   name + ", sample " + std::to_string(sample + 1) + ": sample description " +
                                       std::to_string(descriptionIndex) + " does not exist"};
            return success();
        }
        std::optional<DescriptionCheck>& check = checks[descriptionIndex - 1];
        if (!check.has_value()) {
            Result<DescriptionCheck> made = describe(content, track, descriptions[descriptionIndex - 1]);
            if (!made.ok()) {
                return made.error();
            }
            check = std::move(*made);
            if (check->status == LevelStatus::Failed && check->checker == nullptr) {
                outcome = TrackOutcome{LevelStatus::Failed, descriptions[descriptionIndex - 1].offset,
                                       name + " (" + check->label + "): " + check->detail};
                return success();
            }
        }
        if (check->checker == nullptr) {
            ++skipped;
            return success();
        }
        if (offset > content.size() || size > content.size() - offset) {
            outcome = TrackOutcome{LevelStatus::Truncated, std::min(offset, content.size()),
                                   name + " (" + check->label + "): the data ends inside sample " +
                                       std::to_string(sample + 1) + " of " + std::to_string(track.totalSamples()) +
                                       "; " + check->checker->summary()};
            return success();
        }
        Result<std::optional<Problem>> problem = check->checker->check(content, offset, size);
        if (!problem.ok()) {
            return problem.error();
        }
        if (problem->has_value()) {
            outcome = TrackOutcome{LevelStatus::Failed, (*problem)->offset,
                                   name + " (" + check->label + "), sample " + std::to_string(sample + 1) + ": " +
                                       (*problem)->detail};
            return success();
        }
        ++checked;
        return success();
    };

    for (const mp4::Chunk& chunk : track.chunks) {
        std::uint64_t offset = chunk.offset;
        for (std::uint32_t i = 0; i < chunk.sampleCount && !outcome.has_value(); ++i) {
            const std::uint32_t index = chunk.firstSample + i;
            const std::uint32_t size = track.samples.sampleSize(index);
            if (Status status = visit(chunk.sampleDescriptionIndex, index, offset, size); !status.ok()) {
                return status.error();
            }
            offset += size;
        }
    }
    const std::uint64_t base = track.samples.sampleCount;
    for (const mp4::Chunk& run : track.fragmentRuns) {
        std::uint64_t offset = run.offset;
        for (std::uint32_t i = 0; i < run.sampleCount && !outcome.has_value(); ++i) {
            const std::uint32_t index = run.firstSample + i;
            const std::uint32_t size = track.fragmentSampleSizes[index];
            if (Status status = visit(run.sampleDescriptionIndex, base + index, offset, size); !status.ok()) {
                return status.error();
            }
            offset += size;
        }
    }
    if (outcome.has_value()) {
        return *outcome;
    }
    std::string labels;
    std::string summaries;
    LevelStatus notChecked = LevelStatus::NotApplicable;
    std::string notCheckedDetail;
    for (const std::optional<DescriptionCheck>& check : checks) {
        if (!check.has_value()) {
            continue;
        }
        labels += (labels.empty() ? "" : " | ") + check->label;
        if (check->checker != nullptr) {
            summaries += (summaries.empty() ? "" : "; ") + check->checker->summary();
        } else if (check->status == LevelStatus::Unsupported || notCheckedDetail.empty()) {
            notChecked = check->status;
            notCheckedDetail = check->detail;
        }
    }
    if (checked > 0) {
        std::string detail = name + " (" + labels + "): " + std::to_string(checked) +
                             (checked == 1 ? " sample, " : " samples, ") + summaries;
        if (skipped > 0) {
            detail += "; " + std::to_string(skipped) + " samples not decoded (" + notCheckedDetail + ")";
        }
        return TrackOutcome{LevelStatus::Passed, std::nullopt, std::move(detail)};
    }
    return TrackOutcome{notChecked, std::nullopt, name + " (" + labels + "): " + notCheckedDetail};
}

class Mp4MediaValidator final : public IMediaValidator {
public:
    Mp4MediaValidator(std::string formatId, std::string checker)
        : formatId_(std::move(formatId)), checker_(std::move(checker)) {}

    [[nodiscard]] std::string_view formatId() const noexcept override { return formatId_; }

    [[nodiscard]] Result<LevelResult> validate(carving::IContentReader& content,
                                               const MediaLimits& limits) const override {
        const MediaVerdict verdict(checker_);
        Result<mp4::Mp4File> parsed = mp4::parseFile(content, limits.mp4);
        if (!parsed.ok()) {
            return parsed.error();
        }
        if (!parsed->movie.has_value()) {
            return verdict.truncated(content.size(), "no movie (moov) to find the samples by");
        }
        std::vector<TrackOutcome> outcomes;
        std::size_t number = 0;
        for (const mp4::Track& track : parsed->movie->tracks) {
            ++number;
            if (track.kind == mp4::TrackKind::Other) {
                outcomes.push_back(TrackOutcome{LevelStatus::NotRun, std::nullopt,
                                                "track " + std::to_string(number) + " ('" + track.handler.text() +
                                                    "'): not audio or video"});
                continue;
            }
            Result<TrackOutcome> outcome = checkTrack(content, track, number);
            if (!outcome.ok()) {
                return outcome.error();
            }
            outcomes.push_back(std::move(*outcome));
        }
        for (const LevelStatus status : {LevelStatus::Failed, LevelStatus::Truncated}) {
            for (const TrackOutcome& outcome : outcomes) {
                if (outcome.status == status) {
                    const std::uint64_t offset = outcome.offset.value_or(0);
                    return status == LevelStatus::Failed ? verdict.failed(offset, outcome.detail)
                                                         : verdict.truncated(offset, outcome.detail);
                }
            }
        }
        std::string detail;
        bool passed = false;
        bool unsupported = false;
        for (const TrackOutcome& outcome : outcomes) {
            detail += (detail.empty() ? "" : "; ") + outcome.detail;
            passed = passed || outcome.status == LevelStatus::Passed;
            unsupported = unsupported || outcome.status == LevelStatus::Unsupported;
        }
        if (outcomes.empty()) {
            return verdict.unsupported("the movie has no tracks");
        }
        if (passed) {
            return verdict.passed(std::move(detail), Coverage::Partial);
        }
        if (unsupported || std::none_of(outcomes.begin(), outcomes.end(), [](const TrackOutcome& outcome) {
                return outcome.status == LevelStatus::NotApplicable;
            })) {
            return verdict.unsupported(std::move(detail));
        }
        return verdict.notApplicable(std::move(detail));
    }

private:
    std::string formatId_;
    std::string checker_;
};

}  // namespace

std::shared_ptr<const IMediaValidator> makeM4aMediaValidator() {
    return std::make_shared<Mp4MediaValidator>("m4a", "m4a decoder");
}

std::shared_ptr<const IMediaValidator> makeMp4MediaValidator() {
    return std::make_shared<Mp4MediaValidator>("mp4", "mp4 decoder");
}

}  // namespace recovery::validation::detail
