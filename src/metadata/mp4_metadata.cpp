// MP4 and M4A metadata: the movie as P11's parser reads it (brands, tracks,
// sample tables, movie fragments), and what the parser leaves out, read here
// box by box: the movie header's times, the track header's matrix (the
// rotation), the codec configurations (avcC, hvcC, esds), the sample
// durations of movie fragments, and the tags of udta (iTunes ilst items and
// QuickTime text atoms) with their cover art.

#include "extraction.hpp"

#include "formats/mp4_analysis.hpp"
#include "formats/mp4_box.hpp"
#include "formats/mp4_parser.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"

#include <array>
#include <bit>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <string>

namespace recovery::metadata::detail {

namespace {

using carving::IContentReader;
using formats::mp4::BoxHeader;
using formats::mp4::BoxRead;
using formats::mp4::BoxSequence;
using formats::mp4::BoxSize;
using formats::mp4::BoxStatus;
using formats::mp4::FourCc;
using formats::mp4::Movie;
using formats::mp4::Mp4File;
using formats::mp4::Track;
using formats::mp4::TrackKind;

// Children of one box visited, at most.
constexpr std::uint32_t kMaxChildren = 4096;
// Seconds from 1904-01-01 (the movie header's epoch) to 1970-01-01.
constexpr std::uint64_t kEpoch1904 = 2'082'844'800;

constexpr FourCc kUdta{"udta"};
constexpr FourCc kMeta{"meta"};
constexpr FourCc kIlst{"ilst"};
constexpr FourCc kData{"data"};
constexpr FourCc kEsds{"esds"};
constexpr FourCc kWave{"wave"};
constexpr FourCc kCovr{"covr"};
constexpr FourCc kTrkn{"trkn"};
constexpr FourCc kGnre{"gnre"};
constexpr FourCc kTitle{"\xA9" "nam"};
constexpr FourCc kArtist{"\xA9" "ART"};
constexpr FourCc kAuthor{"\xA9" "aut"};
constexpr FourCc kAlbum{"\xA9" "alb"};
constexpr FourCc kDay{"\xA9" "day"};
constexpr FourCc kGenre{"\xA9" "gen"};

// Visits the Valid boxes in [begin, end) in order, until `visit` returns false.
Status forEachChild(IContentReader& content, std::uint64_t begin, std::uint64_t end,
                    const std::function<Result<bool>(const BoxHeader&)>& visit) {
    BoxSequence boxes(content, begin, end);
    for (std::uint32_t i = 0; i < kMaxChildren; ++i) {
        Result<std::optional<BoxRead>> next = boxes.next();
        if (!next.ok()) {
            return next.error();
        }
        if (!next->has_value() || (*next)->status != BoxStatus::Valid) {
            return success();
        }
        Result<bool> more = visit((*next)->header);
        if (!more.ok()) {
            return more.error();
        }
        if (!*more) {
            return success();
        }
    }
    return success();
}

Result<std::optional<BoxHeader>> findChild(IContentReader& content, std::uint64_t begin, std::uint64_t end,
                                           FourCc type) {
    std::optional<BoxHeader> found;
    Status visited = forEachChild(content, begin, end, [&](const BoxHeader& box) -> Result<bool> {
        if (box.type == type) {
            found = box;
            return false;
        }
        return true;
    });
    if (!visited.ok()) {
        return visited.error();
    }
    return found;
}

std::uint64_t gcd(std::uint64_t a, std::uint64_t b) noexcept {
    return std::gcd(a, b);
}

// ---------------------------------------------------------------------------
// Codecs
// ---------------------------------------------------------------------------

struct CodecName {
    FourCc entry;
    std::string_view codec;
};

constexpr std::array<CodecName, 37> kCodecs = {{
    {FourCc("avc1"), "h264"},  {FourCc("avc3"), "h264"},      {FourCc("hvc1"), "hevc"},  {FourCc("hev1"), "hevc"},
    {FourCc("mp4v"), "mpeg4"}, {FourCc("s263"), "h263"},      {FourCc("h263"), "h263"},  {FourCc("jpeg"), "mjpeg"},
    {FourCc("mjpa"), "mjpeg"}, {FourCc("mjpb"), "mjpeg"},     {FourCc("av01"), "av1"},   {FourCc("vp09"), "vp9"},
    {FourCc("vp08"), "vp8"},   {FourCc("apch"), "prores"},    {FourCc("apcn"), "prores"}, {FourCc("apcs"), "prores"},
    {FourCc("apco"), "prores"}, {FourCc("ap4h"), "prores"},   {FourCc("mp4a"), "aac"},   {FourCc("alac"), "alac"},
    {FourCc("ac-3"), "ac3"},   {FourCc("ec-3"), "eac3"},      {FourCc("Opus"), "opus"},  {FourCc("fLaC"), "flac"},
    {FourCc("samr"), "amr_nb"}, {FourCc("sawb"), "amr_wb"},   {FourCc(".mp3"), "mp3"},   {FourCc("lpcm"), "pcm"},
    {FourCc("sowt"), "pcm"},   {FourCc("twos"), "pcm"},       {FourCc("in24"), "pcm"},   {FourCc("in32"), "pcm"},
    {FourCc("raw "), "pcm"},   {FourCc("ipcm"), "pcm"},       {FourCc("fl32"), "pcm_float"},
    {FourCc("fl64"), "pcm_float"}, {FourCc("fpcm"), "pcm_float"},
}};

std::string codecName(FourCc entry) {
    for (const CodecName& name : kCodecs) {
        if (name.entry == entry) {
            return std::string(name.codec);
        }
    }
    return entry.text();
}

bool recordsSampleSize(std::string_view codec) noexcept {
    return codec == "pcm" || codec == "pcm_float" || codec == "alac";
}

std::string levelText(std::uint32_t major, std::uint32_t minor) {
    return std::to_string(major) + "." + std::to_string(minor);
}

std::string avcProfile(std::uint8_t profile, std::uint8_t constraints) {
    switch (profile) {
        case 66:
            return (constraints & 0x40U) != 0 ? "Constrained Baseline" : "Baseline";
        case 77:
            return "Main";
        case 88:
            return "Extended";
        case 100:
            return "High";
        case 110:
            return "High 10";
        case 122:
            return "High 4:2:2";
        case 244:
            return "High 4:4:4 Predictive";
        case 44:
            return "CAVLC 4:4:4";
        default:
            return "profile " + std::to_string(profile);
    }
}

std::string hevcProfile(std::uint8_t profile) {
    switch (profile) {
        case 1:
            return "Main";
        case 2:
            return "Main 10";
        case 3:
            return "Main Still Picture";
        case 4:
            return "Rext";
        case 9:
            return "SCC";
        default:
            return "profile " + std::to_string(profile);
    }
}

std::string aacProfile(std::uint32_t objectType) {
    switch (objectType) {
        case 1:
            return "AAC Main";
        case 2:
            return "AAC LC";
        case 3:
            return "AAC SSR";
        case 4:
            return "AAC LTP";
        case 5:
            return "HE-AAC";
        case 6:
            return "AAC Scalable";
        case 17:
            return "ER AAC LC";
        case 19:
            return "ER AAC LTP";
        case 23:
            return "AAC LD";
        case 29:
            return "HE-AAC v2";
        case 39:
            return "AAC ELD";
        case 42:
            return "USAC";
        default:
            return "AOT " + std::to_string(objectType);
    }
}

class Bits {
public:
    explicit Bits(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    // `count` (at most 24) bits, or none past the end.
    std::optional<std::uint32_t> read(std::uint32_t count) {
        if (count > 24 || std::uint64_t{bytes_.size()} * 8 - position_ < count) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < count; ++i, ++position_) {
            const auto byte = static_cast<std::uint8_t>(bytes_[static_cast<std::size_t>(position_ / 8)]);
            value = (value << 1) | ((byte >> (7 - position_ % 8)) & 1U);
        }
        return value;
    }

private:
    std::span<const std::byte> bytes_;
    std::uint64_t position_ = 0;
};

constexpr std::array<std::uint32_t, 13> kAacRates = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                     22050, 16000, 12000, 11025, 8000,  7350};

struct AudioConfig {
    std::string codec;
    std::string profile;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
};

// An AudioSpecificConfig (ISO/IEC 14496-3): object type, rate, channels,
// and SBR or PS signalled explicitly.
std::optional<AudioConfig> audioSpecificConfig(std::span<const std::byte> bytes) {
    Bits bits(bytes);
    const auto objectType = [&]() -> std::optional<std::uint32_t> {
        const std::optional<std::uint32_t> type = bits.read(5);
        if (type == 31U) {
            const std::optional<std::uint32_t> extended = bits.read(6);
            return extended.has_value() ? std::optional<std::uint32_t>(32 + *extended) : std::nullopt;
        }
        return type;
    };
    const auto rate = [&]() -> std::optional<std::uint32_t> {
        const std::optional<std::uint32_t> index = bits.read(4);
        if (!index.has_value()) {
            return std::nullopt;
        }
        if (*index == 15) {
            return bits.read(24);
        }
        return *index < kAacRates.size() ? kAacRates[*index] : 0;
    };
    std::optional<std::uint32_t> type = objectType();
    const std::optional<std::uint32_t> coreRate = rate();
    const std::optional<std::uint32_t> configuration = bits.read(4);
    if (!type.has_value() || !coreRate.has_value() || !configuration.has_value() || *type == 0) {
        return std::nullopt;
    }
    AudioConfig config;
    config.codec = "aac";
    config.sampleRate = *coreRate;
    static constexpr std::array<std::uint32_t, 16> kChannels = {0, 1, 2, 3, 4, 5, 6, 8, 0, 0, 0, 7, 8, 24, 8, 0};
    config.channels = kChannels[*configuration];
    if (*type == 5 || *type == 29) {
        const std::optional<std::uint32_t> extensionRate = rate();
        if (extensionRate.has_value() && *extensionRate != 0) {
            config.sampleRate = *extensionRate;
        }
        // PS: two channels decoded from one.
        if (*type == 29 && config.channels == 1) {
            config.channels = 2;
        }
    }
    config.profile = aacProfile(*type);
    return config;
}

// The length of an MPEG-4 descriptor at bytes[at] (its tag read already at
// bytes[at - 1]): 1 to 4 bytes of 7 bits each.
std::optional<std::pair<std::size_t, std::size_t>> descriptorLength(std::span<const std::byte> bytes,
                                                                    std::size_t at) {
    std::size_t length = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        if (at + i >= bytes.size()) {
            return std::nullopt;
        }
        const std::uint8_t byte = loadU8(bytes, at + i);
        length = (length << 7) | (byte & 0x7FU);
        if ((byte & 0x80U) == 0) {
            return std::make_pair(i + 1, length);
        }
    }
    return std::nullopt;
}

// An esds box's payload: its ES descriptor's decoder configuration.
std::optional<AudioConfig> esdsConfig(std::span<const std::byte> payload) {
    std::size_t pos = 4;  // version and flags
    if (pos >= payload.size() || loadU8(payload, pos) != 0x03) {
        return std::nullopt;
    }
    const auto es = descriptorLength(payload, pos + 1);
    if (!es.has_value()) {
        return std::nullopt;
    }
    pos += 1 + es->first;
    if (pos + 3 > payload.size()) {
        return std::nullopt;
    }
    const std::uint8_t flags = loadU8(payload, pos + 2);
    pos += 3;
    if ((flags & 0x80U) != 0) {
        pos += 2;
    }
    if ((flags & 0x40U) != 0) {
        if (pos >= payload.size()) {
            return std::nullopt;
        }
        pos += 1 + std::size_t{loadU8(payload, pos)};
    }
    if ((flags & 0x20U) != 0) {
        pos += 2;
    }
    if (pos >= payload.size() || loadU8(payload, pos) != 0x04) {
        return std::nullopt;
    }
    const auto decoder = descriptorLength(payload, pos + 1);
    if (!decoder.has_value()) {
        return std::nullopt;
    }
    pos += 1 + decoder->first;
    if (pos + 13 > payload.size()) {
        return std::nullopt;
    }
    const std::uint8_t objectType = loadU8(payload, pos);
    AudioConfig config;
    if (objectType == 0x69 || objectType == 0x6B) {
        config.codec = "mp3";
        return config;
    }
    config.codec = "aac";
    if (objectType == 0x66 || objectType == 0x67 || objectType == 0x68) {
        config.profile = aacProfile(objectType - 0x65U);
        return config;
    }
    pos += 13;
    if (objectType != 0x40 || pos >= payload.size() || loadU8(payload, pos) != 0x05) {
        return config;
    }
    const auto specific = descriptorLength(payload, pos + 1);
    if (!specific.has_value()) {
        return config;
    }
    pos += 1 + specific->first;
    const std::size_t length = std::min(specific->second, payload.size() - std::min(pos, payload.size()));
    if (std::optional<AudioConfig> parsed = audioSpecificConfig(payload.subspan(pos, length)); parsed.has_value()) {
        return parsed;
    }
    return config;
}

// ---------------------------------------------------------------------------
// The reader
// ---------------------------------------------------------------------------

class Mp4Reader {
public:
    explicit Mp4Reader(Extraction& x) : x_(x), content_(x.content) {}

    [[nodiscard]] Status read();

private:
    [[nodiscard]] Result<std::span<const std::byte>> payload(const BoxHeader& box, std::size_t length) {
        return readUpTo(content_, box.payloadOffset(),
                        static_cast<std::size_t>(std::min<std::uint64_t>(box.payloadSize(), length)));
    }
    [[nodiscard]] Status movieTimes(const Movie& movie, MovieMetadata& out);
    [[nodiscard]] Status track(const Track& track, MovieMetadata& out);
    [[nodiscard]] Result<std::optional<std::uint16_t>> rotation(const Track& track);
    [[nodiscard]] Status videoConfig(const formats::mp4::SampleDescription& entry, VideoStreamMetadata& video);
    [[nodiscard]] Status audioConfig(const formats::mp4::SampleDescription& entry, AudioStreamMetadata& audio);
    [[nodiscard]] Status fragmentDurations(const Mp4File& file);
    [[nodiscard]] Status tags(const Movie& movie);
    [[nodiscard]] Status metaBox(const BoxHeader& meta);
    [[nodiscard]] Status item(const BoxHeader& item);
    [[nodiscard]] Status textAtom(const BoxHeader& atom);
    [[nodiscard]] std::optional<MediaDateTime> date(const std::string& text, std::uint64_t offset);

    Extraction& x_;
    IContentReader& content_;
    // Track id -> the summed durations of its samples in movie fragments.
    std::map<std::uint32_t, std::uint64_t> fragmentUnits_;
    bool fragmented_ = false;
};

Status Mp4Reader::movieTimes(const Movie& movie, MovieMetadata& out) {
    Result<std::optional<BoxHeader>> mvhd =
        findChild(content_, movie.box.payloadOffset(), movie.box.end(), formats::mp4::box::kMvhd);
    if (!mvhd.ok()) {
        return mvhd.error();
    }
    if (!mvhd->has_value()) {
        return success();
    }
    Result<std::span<const std::byte>> bytes = payload(**mvhd, 20);
    if (!bytes.ok()) {
        return bytes.error();
    }
    const std::span<const std::byte> p = *bytes;
    if (p.size() < 12 || (loadU8(p, 0) == 1 && p.size() < 20)) {
        return success();
    }
    const bool wide = loadU8(p, 0) == 1;
    const std::uint64_t created = wide ? loadBe64(p, 4) : loadBe32(p, 4);
    const std::uint64_t modified = wide ? loadBe64(p, 12) : loadBe32(p, 8);
    const auto convert = [&](std::uint64_t value, std::string_view what) -> std::optional<MediaDateTime> {
        if (value == 0) {
            return std::nullopt;
        }
        std::optional<MediaDateTime> time;
        if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            time = utcDateTime(static_cast<std::int64_t>(value) - static_cast<std::int64_t>(kEpoch1904));
        }
        if (!time.has_value()) {
            x_.issue((*mvhd)->offset, "the movie's " + std::string(what) + " time is out of range");
        }
        return time;
    };
    out.created = convert(created, "creation");
    out.modified = convert(modified, "modification");
    return success();
}

Result<std::optional<std::uint16_t>> Mp4Reader::rotation(const Track& track) {
    Result<std::optional<BoxHeader>> tkhd =
        findChild(content_, track.box.payloadOffset(), track.box.end(), formats::mp4::box::kTkhd);
    if (!tkhd.ok()) {
        return tkhd.error();
    }
    if (!tkhd->has_value()) {
        return std::optional<std::uint16_t>{};
    }
    Result<std::span<const std::byte>> bytes = payload(**tkhd, 96);
    if (!bytes.ok()) {
        return bytes.error();
    }
    const std::span<const std::byte> p = *bytes;
    const std::size_t matrix = !p.empty() && loadU8(p, 0) == 1 ? 52 : 40;
    if (p.size() < matrix + 36) {
        return std::optional<std::uint16_t>{};
    }
    const auto at = [&](std::size_t i) { return static_cast<std::int32_t>(loadBe32(p, matrix + 4 * i)); };
    const std::int32_t a = at(0);
    const std::int32_t b = at(1);
    const std::int32_t c = at(3);
    const std::int32_t d = at(4);
    // x' = a x + c y, y' = b x + d y (y down): (0, 1, -1, 0) turns right
    // into down, a quarter turn clockwise.
    if (b == 0 && c == 0 && a > 0 && d > 0) {
        return std::optional<std::uint16_t>{std::uint16_t{0}};
    }
    if (a == 0 && d == 0 && b > 0 && c < 0) {
        return std::optional<std::uint16_t>{std::uint16_t{90}};
    }
    if (b == 0 && c == 0 && a < 0 && d < 0) {
        return std::optional<std::uint16_t>{std::uint16_t{180}};
    }
    if (a == 0 && d == 0 && b < 0 && c > 0) {
        return std::optional<std::uint16_t>{std::uint16_t{270}};
    }
    x_.issue((*tkhd)->offset, "track " + std::to_string(track.header.trackId) +
                                  ": the matrix is not a turn by a multiple of 90 degrees");
    return std::optional<std::uint16_t>{};
}

Status Mp4Reader::videoConfig(const formats::mp4::SampleDescription& entry, VideoStreamMetadata& video) {
    constexpr std::uint64_t kVisualEntry = 8 + 78;
    if (entry.size < kVisualEntry) {
        return success();
    }
    return forEachChild(content_, entry.offset + kVisualEntry, entry.offset + entry.size,
                        [&](const BoxHeader& box) -> Result<bool> {
                            if (box.type != formats::mp4::box::kAvcC && box.type != formats::mp4::box::kHvcC) {
                                return true;
                            }
                            Result<std::span<const std::byte>> bytes = payload(box, 13);
                            if (!bytes.ok()) {
                                return bytes.error();
                            }
                            const std::span<const std::byte> p = *bytes;
                            if (box.type == formats::mp4::box::kAvcC && p.size() >= 4) {
                                const std::uint8_t profile = loadU8(p, 1);
                                const std::uint8_t constraints = loadU8(p, 2);
                                const std::uint8_t level = loadU8(p, 3);
                                video.profile = avcProfile(profile, constraints);
                                const bool level1b = level == 11 && (constraints & 0x10U) != 0 &&
                                                     (profile == 66 || profile == 77 || profile == 88);
                                video.level = level1b ? "1b" : levelText(level / 10U, level % 10U);
                            } else if (box.type == formats::mp4::box::kHvcC && p.size() >= 13) {
                                const std::uint8_t general = loadU8(p, 1);
                                const std::uint8_t level = loadU8(p, 12);
                                video.profile = hevcProfile(general & 0x1FU);
                                video.level = levelText(level / 30U, (level % 30U) / 3U);
                                if ((general & 0x20U) != 0) {
                                    video.level += ", High tier";
                                }
                            }
                            return false;
                        });
}

Status Mp4Reader::audioConfig(const formats::mp4::SampleDescription& entry, AudioStreamMetadata& audio) {
    constexpr std::uint64_t kAudioEntry = 8 + 28;
    if (entry.size < kAudioEntry) {
        return success();
    }
    Result<std::span<const std::byte>> head = readUpTo(content_, entry.offset + 16, 2);
    if (!head.ok()) {
        return head.error();
    }
    const std::uint16_t version = head->size() == 2 ? loadBe16(*head, 0) : 0;
    const std::uint64_t children = kAudioEntry + (version == 1 ? 16 : (version == 2 ? 36 : 0));
    if (entry.size < children) {
        return success();
    }
    const auto visit = [&](const auto& self, std::uint64_t begin, std::uint64_t end, int depth) -> Status {
        return forEachChild(content_, begin, end, [&](const BoxHeader& box) -> Result<bool> {
            if (box.type == kWave && depth == 0) {
                if (Status inner = self(self, box.payloadOffset(), box.end(), 1); !inner.ok()) {
                    return inner.error();
                }
                return true;
            }
            if (box.type != kEsds) {
                return true;
            }
            Result<std::span<const std::byte>> bytes = payload(box, 256);
            if (!bytes.ok()) {
                return bytes.error();
            }
            if (const std::optional<AudioConfig> config = esdsConfig(*bytes); config.has_value()) {
                audio.codec = config->codec;
                audio.profile = config->profile;
                if (config->sampleRate != 0) {
                    audio.sampleRate = config->sampleRate;
                }
                if (config->channels != 0) {
                    audio.channels = config->channels;
                }
            }
            return false;
        });
    };
    return visit(visit, entry.offset + children, entry.offset + entry.size, 0);
}

Status Mp4Reader::fragmentDurations(const Mp4File& file) {
    if (!file.movie.has_value() || file.fragments.empty()) {
        return success();
    }
    std::map<std::uint32_t, std::uint32_t> defaults;
    for (const formats::mp4::TrackExtends& extends : file.movie->trackExtends) {
        defaults[extends.trackId] = extends.sampleDuration;
    }
    std::uint64_t records = 0;
    const std::uint64_t maxRecords = x_.options.mp4.maxTableEntries;
    for (const formats::mp4::MovieFragment& fragment : file.fragments) {
        Status walked = forEachChild(
            content_, fragment.box.payloadOffset(), fragment.box.end(), [&](const BoxHeader& traf) -> Result<bool> {
                if (traf.type != formats::mp4::box::kTraf) {
                    return true;
                }
                std::optional<std::uint32_t> trackId;
                std::uint32_t defaultDuration = 0;
                Status inner = forEachChild(
                    content_, traf.payloadOffset(), traf.end(), [&](const BoxHeader& box) -> Result<bool> {
                        if (box.type == formats::mp4::box::kTfhd) {
                            Result<std::span<const std::byte>> bytes = payload(box, 32);
                            if (!bytes.ok()) {
                                return bytes.error();
                            }
                            const std::span<const std::byte> p = *bytes;
                            if (p.size() < 8) {
                                return true;
                            }
                            const std::uint32_t flags = loadBe32(p, 0) & 0xFFFFFFU;
                            trackId = loadBe32(p, 4);
                            defaultDuration = defaults.contains(*trackId) ? defaults[*trackId] : 0;
                            std::size_t at = 8 + ((flags & 0x01U) != 0 ? 8 : 0) + ((flags & 0x02U) != 0 ? 4 : 0);
                            if ((flags & 0x08U) != 0 && p.size() >= at + 4) {
                                defaultDuration = loadBe32(p, at);
                            }
                            return true;
                        }
                        if (box.type != formats::mp4::box::kTrun || !trackId.has_value()) {
                            return true;
                        }
                        Result<std::span<const std::byte>> bytes = payload(box, 16);
                        if (!bytes.ok()) {
                            return bytes.error();
                        }
                        if (bytes->size() < 8) {
                            return true;
                        }
                        const std::uint32_t flags = loadBe32(*bytes, 0) & 0xFFFFFFU;
                        const std::uint32_t count = loadBe32(*bytes, 4);
                        std::uint64_t& units = fragmentUnits_[*trackId];
                        if ((flags & 0x100U) == 0) {
                            units += std::uint64_t{count} * defaultDuration;
                            return true;
                        }
                        const std::uint64_t record = 4 * static_cast<std::uint64_t>(std::popcount(flags & 0xF00U));
                        const std::uint64_t first =
                            8 + ((flags & 0x01U) != 0 ? 4 : 0) + ((flags & 0x04U) != 0 ? 4 : 0);
                        if (box.payloadSize() < first ||
                            std::uint64_t{count} > (box.payloadSize() - first) / record) {
                            x_.issue(box.offset, "a movie fragment's run lists more samples than it holds");
                            return true;
                        }
                        if (records + count > maxRecords) {
                            x_.issue(box.offset, "movie fragment samples beyond the parse limit not counted");
                            return false;
                        }
                        records += count;
                        // The duration is the first field of each record.
                        const std::uint64_t perRead = IContentReader::kMaxReadLength / record;
                        for (std::uint64_t done = 0; done < count;) {
                            const std::uint64_t take = std::min<std::uint64_t>(count - done, perRead);
                            Result<std::span<const std::byte>> table =
                                content_.read(box.payloadOffset() + first + done * record,
                                              static_cast<std::size_t>(take * record));
                            if (!table.ok()) {
                                return table.error();
                            }
                            for (std::uint64_t i = 0; i < take; ++i) {
                                units += loadBe32(*table, static_cast<std::size_t>(i * record));
                            }
                            done += take;
                        }
                        return true;
                    });
                if (!inner.ok()) {
                    return inner.error();
                }
                return records < maxRecords;
            });
        if (!walked.ok()) {
            return walked;
        }
    }
    return success();
}

Status Mp4Reader::track(const Track& track, MovieMetadata& out) {
    TrackMetadata meta;
    meta.id = track.header.trackId;
    meta.kind = MediaKind::Unknown;
    if (track.kind == TrackKind::Video) {
        meta.kind = MediaKind::Video;
    } else if (track.kind == TrackKind::Audio) {
        meta.kind = MediaKind::Audio;
    }
    meta.handler = track.handler.text();
    meta.language = track.media.isoLanguage();
    meta.enabled = track.header.enabled();
    meta.samples = track.totalSamples();
    for (const formats::mp4::Chunk& chunk : track.chunks) {
        meta.bytes += chunk.size;
    }
    for (const formats::mp4::Chunk& run : track.fragmentRuns) {
        meta.bytes += run.size;
    }
    // Durations: the sample tables' and the fragments' sample durations, or
    // the media header's.
    std::uint64_t units = 0;
    bool overflow = false;
    for (const formats::mp4::TimeToSample& entry : track.samples.timeToSample) {
        const std::optional<std::uint64_t> sum =
            checkedAdd<std::uint64_t>(units, std::uint64_t{entry.sampleCount} * entry.sampleDelta);
        overflow = overflow || !sum.has_value();
        units = sum.value_or(units);
    }
    if (const auto found = fragmentUnits_.find(meta.id); found != fragmentUnits_.end()) {
        const std::optional<std::uint64_t> sum = checkedAdd<std::uint64_t>(units, found->second);
        overflow = overflow || !sum.has_value();
        units = sum.value_or(units);
    }
    const std::uint64_t unset = track.media.version == 1 ? std::numeric_limits<std::uint64_t>::max() : 0xFFFFFFFFU;
    const bool headerKnown = track.media.duration != 0 && track.media.duration != unset;
    std::uint64_t durationUnits = units;
    if (!fragmented_ && headerKnown) {
        durationUnits = track.media.duration;
    } else if (units == 0 && headerKnown) {
        durationUnits = track.media.duration;
    }
    if (overflow) {
        x_.issue(track.box.offset, "track " + std::to_string(meta.id) + ": its sample durations overflow");
    } else if (durationUnits > 0) {
        meta.duration = durationOf(durationUnits, track.media.timescale);
    }
    if (!track.samples.descriptions.empty()) {
        const formats::mp4::SampleDescription& entry = track.samples.descriptions.front();
        meta.sampleEntry = entry.format.text();
        if (track.kind == TrackKind::Video) {
            VideoStreamMetadata video;
            video.codec = codecName(entry.format);
            video.width = entry.width;
            video.height = entry.height;
            video.displayWidth = track.header.width >> 16;
            video.displayHeight = track.header.height >> 16;
            if (Status read = videoConfig(entry, video); !read.ok()) {
                return read;
            }
            Result<std::optional<std::uint16_t>> turn = rotation(track);
            if (!turn.ok()) {
                return turn.error();
            }
            video.rotation = turn->value_or(0);
            if (units > 0 && meta.samples > 0 && track.media.timescale > 0 && !overflow) {
                const std::uint64_t frames = meta.samples * track.media.timescale;
                const std::uint64_t divisor = gcd(frames, units);
                video.frameRate = FrameRate{frames / divisor, units / divisor};
            }
            if (meta.duration.has_value()) {
                video.bitrate = bitrateOf(meta.bytes, *meta.duration);
            }
            meta.video = std::move(video);
        } else if (track.kind == TrackKind::Audio) {
            AudioStreamMetadata audio;
            audio.codec = codecName(entry.format);
            audio.sampleRate = entry.sampleRate;
            audio.channels = entry.channelCount;
            if (recordsSampleSize(audio.codec)) {
                audio.bitsPerSample = entry.sampleSize;
            }
            if (Status read = audioConfig(entry, audio); !read.ok()) {
                return read;
            }
            if (meta.duration.has_value()) {
                audio.bitrate = bitrateOf(meta.bytes, *meta.duration);
            }
            meta.audio = std::move(audio);
        }
    }
    out.tracks.push_back(std::move(meta));
    return success();
}

std::optional<MediaDateTime> Mp4Reader::date(const std::string& text, std::uint64_t offset) {
    std::optional<MediaDateTime> parsed = parseIsoDateTime(text);
    if (!parsed.has_value() && !text.empty()) {
        x_.issue(offset, "the MP4 date tag is not a date");
    }
    return parsed;
}

Status Mp4Reader::item(const BoxHeader& item) {
    MediaTags& tags = x_.out.tags;
    return forEachChild(content_, item.payloadOffset(), item.end(), [&](const BoxHeader& data) -> Result<bool> {
        if (data.type != kData || data.payloadSize() < 8) {
            return true;
        }
        const std::uint64_t value = data.payloadOffset() + 8;
        const std::uint64_t length = data.payloadSize() - 8;
        if (item.type == kCovr) {
            Result<std::optional<PreviewSource>> cover =
                probePicture(x_, PreviewKind::CoverArt, value, length, "the MP4 cover art");
            if (!cover.ok()) {
                return cover.error();
            }
            if (cover->has_value()) {
                x_.out.previews.push_back(std::move(**cover));
            }
            return true;  // every picture
        }
        Result<std::span<const std::byte>> head = payload(data, 4);
        if (!head.ok()) {
            return head.error();
        }
        const std::uint32_t type = head->size() == 4 ? loadBe32(*head, 0) & 0xFFFFFFU : 0;
        const auto want = static_cast<std::size_t>(
            std::min<std::uint64_t>(length, std::uint64_t{x_.options.maxTextLength} * 4 + 16));
        Result<std::vector<std::byte>> bytes = readBlock(content_, value, want);
        if (!bytes.ok()) {
            return bytes.error();
        }
        if (item.type == kTrkn) {
            if (bytes->size() >= 6) {
                tags.track = loadBe16(*bytes, 2);
                tags.trackTotal = loadBe16(*bytes, 4);
            }
            return false;
        }
        if (item.type == kGnre) {
            if (bytes->size() >= 2 && loadBe16(*bytes, 0) > 0 && tags.genre.empty()) {
                tags.genre = std::string(id3v1Genre(loadBe16(*bytes, 0) - 1U));
            }
            return false;
        }
        const TextEncoding encoding = type == 2 ? TextEncoding::Utf16Be : TextEncoding::Utf8;
        const std::string text = decodeText(*bytes, encoding, x_.options.maxTextLength);
        if (item.type == kTitle) {
            tags.title = text;
        } else if (item.type == kArtist) {
            tags.artist = text;
        } else if (item.type == kAuthor && tags.artist.empty()) {
            tags.artist = text;
        } else if (item.type == kAlbum) {
            tags.album = text;
        } else if (item.type == kGenre) {
            tags.genre = text;
        } else if (item.type == kDay) {
            tags.date = date(text, data.offset);
        }
        return false;
    });
}

Status Mp4Reader::textAtom(const BoxHeader& atom) {
    // QuickTime user data text: [16-bit length][16-bit language][text]; or
    // an iTunes-style item with data boxes, as some writers put there.
    Result<std::span<const std::byte>> head = payload(atom, 8);
    if (!head.ok()) {
        return head.error();
    }
    if (head->size() >= 8 && startsWith(head->subspan(4), "data")) {
        return item(atom);
    }
    if (head->size() < 4) {
        return success();
    }
    const std::uint16_t length = loadBe16(*head, 0);
    const std::uint16_t language = loadBe16(*head, 2);
    if (std::uint64_t{length} + 4 > atom.payloadSize()) {
        x_.issue(atom.offset, "a QuickTime text atom runs past its box");
        return success();
    }
    Result<std::vector<std::byte>> bytes =
        readBlock(content_, atom.payloadOffset() + 4,
                  std::min<std::uint64_t>(length, std::uint64_t{x_.options.maxTextLength} * 4 + 16));
    if (!bytes.ok()) {
        return bytes.error();
    }
    // Language codes below 0x400 are Macintosh ones, whose text is in a
    // Macintosh encoding (read as ISO 8859-1); others are UTF-8 or UTF-16.
    TextEncoding encoding = language < 0x400 ? TextEncoding::Utf8OrLatin1 : TextEncoding::Utf8;
    if (bytes->size() >= 2 && loadU8(*bytes, 0) == 0xFE && loadU8(*bytes, 1) == 0xFF) {
        encoding = TextEncoding::Utf16;
    }
    const std::string text = decodeText(*bytes, encoding, x_.options.maxTextLength);
    MediaTags& tags = x_.out.tags;
    const auto set = [](std::string& field, const std::string& value) {
        if (field.empty()) {
            field = value;
        }
    };
    if (atom.type == kTitle) {
        set(tags.title, text);
    } else if (atom.type == kArtist || atom.type == kAuthor) {
        set(tags.artist, text);
    } else if (atom.type == kAlbum) {
        set(tags.album, text);
    } else if (atom.type == kGenre) {
        set(tags.genre, text);
    } else if (atom.type == kDay && !tags.date.has_value()) {
        tags.date = date(text, atom.offset);
    }
    return success();
}

Status Mp4Reader::metaBox(const BoxHeader& meta) {
    // ISO and iTunes write meta as a full box (version and flags first);
    // QuickTime as a plain one. The handler box tells which.
    Result<std::span<const std::byte>> head = payload(meta, 12);
    if (!head.ok()) {
        return head.error();
    }
    std::uint64_t begin = meta.payloadOffset();
    if (head->size() >= 12 && startsWith(head->subspan(8), "hdlr")) {
        begin += 4;
    } else if (!(head->size() >= 8 && startsWith(head->subspan(4), "hdlr")) && head->size() >= 4 &&
               loadBe32(*head, 0) == 0) {
        begin += 4;
    }
    Result<std::optional<BoxHeader>> ilst = findChild(content_, begin, meta.end(), kIlst);
    if (!ilst.ok()) {
        return ilst.error();
    }
    if (!ilst->has_value()) {
        return success();
    }
    const auto visit = [&](const BoxHeader& entry) -> Result<bool> {
        if (Status read = item(entry); !read.ok()) {
            return read.error();
        }
        return true;
    };
    return forEachChild(content_, (*ilst)->payloadOffset(), (*ilst)->end(), visit);
}

Status Mp4Reader::tags(const Movie& movie) {
    Result<std::optional<BoxHeader>> udta = findChild(content_, movie.box.payloadOffset(), movie.box.end(), kUdta);
    if (!udta.ok()) {
        return udta.error();
    }
    if (!udta->has_value()) {
        return success();
    }
    // Walked box by box: text is read up to the text limit, and pictures
    // are only located, so no limit applies to the box as a whole.
    return forEachChild(content_, (*udta)->payloadOffset(), (*udta)->end(), [&](const BoxHeader& box) -> Result<bool> {
        Status read = success();
        if (box.type == kMeta) {
            read = metaBox(box);
        } else if ((box.type.value() >> 24) == 0xA9U) {
            read = textAtom(box);
        }
        if (!read.ok()) {
            return read.error();
        }
        return true;
    });
}

Status Mp4Reader::read() {
    Result<Mp4File> parsed = formats::mp4::parseFile(content_, x_.options.mp4);
    if (!parsed.ok()) {
        return parsed.error();
    }
    const Mp4File& file = *parsed;
    if (file.status != formats::mp4::FileStatus::Valid) {
        x_.issue(std::nullopt, "the MP4 structure: " + file.detail);
    }
    if (!file.fileType.has_value() && !file.movie.has_value()) {
        // Nothing in the content tells audio from video: the format's kind stands.
        x_.issue(std::nullopt, "no ftyp or moov box: nothing to describe the movie");
        return success();
    }
    const formats::mp4::Classification classification =
        formats::mp4::classify(file.fileType, file.movie.has_value() ? &*file.movie : nullptr);
    switch (classification.kind) {
        case formats::mp4::MediaKind::Audio:
            x_.out.kind = MediaKind::Audio;
            break;
        case formats::mp4::MediaKind::Video:
            x_.out.kind = MediaKind::Video;
            break;
        case formats::mp4::MediaKind::Neither:
            x_.out.kind = MediaKind::Unknown;
            break;
    }
    std::string majorBrand;
    if (file.fileType.has_value()) {
        majorBrand = file.fileType->majorBrand.text();
    }
    const std::string_view brand(majorBrand);
    if (x_.out.kind == MediaKind::Video) {
        x_.out.mediaType = brand == "qt  " ? "video/quicktime"
                                           : (brand.starts_with("3gp") ? "video/3gpp"
                                                                       : (brand.starts_with("3g2") ? "video/3gpp2"
                                                                                                   : "video/mp4"));
    } else if (x_.out.kind == MediaKind::Audio) {
        x_.out.mediaType = brand.starts_with("3gp") ? "audio/3gpp" : (brand.starts_with("3g2") ? "audio/3gpp2"
                                                                                               : "audio/mp4");
    } else {
        x_.out.mediaType.clear();
    }
    MovieMetadata movie;
    if (file.fileType.has_value()) {
        movie.majorBrand = majorBrand;
        movie.minorVersion = file.fileType->minorVersion;
        for (const FourCc compatible : file.fileType->compatibleBrands) {
            movie.compatibleBrands.push_back(compatible.text());
        }
    }
    if (!file.movie.has_value()) {
        x_.issue(std::nullopt, "no moov box: the tracks are not known");
        x_.out.movie = std::move(movie);
        return success();
    }
    fragmented_ = file.movie->fragmented;
    movie.fragmented = fragmented_;
    if (Status times = movieTimes(*file.movie, movie); !times.ok()) {
        return times;
    }
    if (Status durations = fragmentDurations(file); !durations.ok()) {
        return durations;
    }
    for (const Track& entry : file.movie->tracks) {
        if (Status read = track(entry, movie); !read.ok()) {
            return read;
        }
    }
    // The movie's duration: its header's, or the longest track's.
    const formats::mp4::MovieHeader& header = file.movie->header;
    const std::uint64_t unset = header.version == 1 ? std::numeric_limits<std::uint64_t>::max() : 0xFFFFFFFFU;
    std::optional<MediaDuration> longest;
    for (const TrackMetadata& entry : movie.tracks) {
        if (entry.duration.has_value() && (!longest.has_value() || *entry.duration > *longest)) {
            longest = entry.duration;
        }
    }
    if (!fragmented_ && header.duration != 0 && header.duration != unset) {
        x_.out.duration = durationOf(header.duration, header.timescale);
    }
    if (!x_.out.duration.has_value()) {
        x_.out.duration = longest;
    }
    // The first video and audio tracks, enabled ones first.
    for (const bool enabledOnly : {true, false}) {
        for (const TrackMetadata& entry : movie.tracks) {
            if (enabledOnly && !entry.enabled) {
                continue;
            }
            if (entry.video.has_value() && !x_.out.video.has_value()) {
                x_.out.video = entry.video;
            }
            if (entry.audio.has_value() && !x_.out.audio.has_value()) {
                x_.out.audio = entry.audio;
            }
        }
    }
    x_.out.movie = std::move(movie);
    return tags(*file.movie);
}

}  // namespace

Status extractMp4(Extraction& x) {
    Mp4Reader reader(x);
    return reader.read();
}

}  // namespace recovery::metadata::detail
