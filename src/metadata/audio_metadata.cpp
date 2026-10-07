// Audio metadata: MP3 and ADTS frame streams (their first frame, an MP3 info
// tag, or a walk of the frame headers), WAV chunks, and the tags around and
// inside them (ID3v2, ID3v1, RIFF INFO).

#include "extraction.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"

#include <array>
#include <cstdio>
#include <set>
#include <string>

namespace recovery::metadata::detail {

namespace {

using carving::IContentReader;

// Zero bytes after the leading tags that are still padding, and how far the
// first frame is looked for beyond them.
constexpr std::uint64_t kMaxPadding = 64 * 1024;
constexpr std::uint64_t kFrameSearch = 64 * 1024;
constexpr int kMaxLeadingTags = 16;

std::string mib(std::uint64_t bytes) {
    return std::to_string(bytes / (1024 * 1024)) + " MiB";
}

// Skips the ID3v2 tags at the start (reading them) and the zero padding
// after them. Returns where the frames should start.
Result<std::uint64_t> leadingTags(Extraction& x, MediaTags& tags) {
    std::uint64_t pos = 0;
    for (int i = 0; i < kMaxLeadingTags; ++i) {
        Result<Id3v2Result> tag = readId3v2(x, pos, tags);
        if (!tag.ok()) {
            return tag.error();
        }
        if (!tag->found) {
            break;
        }
        pos = tag->end;
    }
    for (std::uint64_t skipped = 0; skipped < kMaxPadding;) {
        Result<std::span<const std::byte>> bytes = readUpTo(x.content, pos, 4096);
        if (!bytes.ok()) {
            return bytes.error();
        }
        std::size_t zeros = 0;
        while (zeros < bytes->size() && (*bytes)[zeros] == std::byte{0}) {
            ++zeros;
        }
        pos += zeros;
        skipped += zeros;
        if (zeros < bytes->size() || bytes->empty()) {
            break;
        }
    }
    return pos;
}

// A frame header of a stream, generic over MP3 and ADTS.
struct FrameHeader {
    std::uint32_t length = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    // Samples per channel the frame codes.
    std::uint32_t samples = 0;
    // MP3: kbit/s.
    std::uint32_t bitrate = 0;
    // What every frame of one stream shares (version, layer, rate, ...).
    std::uint32_t stream = 0;
    // MP3: version (3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5), protection, channel mode.
    std::uint8_t version = 0;
    bool crc = false;
    bool mono = false;
    // ADTS: the profile (audio object type - 1).
    std::uint8_t profile = 0;
};

using HeaderParser = std::optional<FrameHeader> (*)(std::span<const std::byte> bytes);

constexpr std::array<std::uint32_t, 16> kMpeg1Bitrates = {0,   32,  40,  48,  56,  64,  80,  96,
                                                          112, 128, 160, 192, 224, 256, 320, 0};
constexpr std::array<std::uint32_t, 16> kMpeg2Bitrates = {0,  8,  16, 24,  32,  40,  48,  56,
                                                          64, 80, 96, 112, 128, 144, 160, 0};

// An MPEG audio Layer III frame header (the layer the engine carves).
std::optional<FrameHeader> mp3Header(std::span<const std::byte> bytes) {
    if (bytes.size() < 4) {
        return std::nullopt;
    }
    const std::uint32_t bits = loadBe32(bytes, 0);
    const std::uint32_t version = (bits >> 19) & 3U;
    const std::uint32_t layer = (bits >> 17) & 3U;
    const std::uint32_t bitrateIndex = (bits >> 12) & 15U;
    const std::uint32_t rateIndex = (bits >> 10) & 3U;
    if ((bits >> 21) != 0x7FF || version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 ||
        rateIndex == 3 || (bits & 3U) == 2) {
        return std::nullopt;
    }
    static constexpr std::array<std::uint32_t, 3> kRates = {44100, 48000, 32000};
    FrameHeader header;
    header.version = static_cast<std::uint8_t>(version);
    header.sampleRate = kRates[rateIndex] >> (version == 3 ? 0 : (version == 2 ? 1 : 2));
    header.bitrate = version == 3 ? kMpeg1Bitrates[bitrateIndex] : kMpeg2Bitrates[bitrateIndex];
    const std::uint32_t padding = (bits >> 9) & 1U;
    header.length = (version == 3 ? 144000 : 72000) * header.bitrate / header.sampleRate + padding;
    header.samples = version == 3 ? 1152 : 576;
    header.crc = ((bits >> 16) & 1U) == 0;
    header.mono = ((bits >> 6) & 3U) == 3;
    header.channels = header.mono ? 1 : 2;
    header.stream = bits & 0xFFFE0C00U;  // sync, version, layer, rate
    return header;
}

// An ADTS frame header.
std::optional<FrameHeader> adtsHeader(std::span<const std::byte> bytes) {
    if (bytes.size() < 7) {
        return std::nullopt;
    }
    if (loadU8(bytes, 0) != 0xFF || (loadU8(bytes, 1) & 0xF6U) != 0xF0) {
        return std::nullopt;
    }
    static constexpr std::array<std::uint32_t, 13> kRates = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                             22050, 16000, 12000, 11025, 8000,  7350};
    const std::uint8_t b2 = loadU8(bytes, 2);
    const std::uint8_t b3 = loadU8(bytes, 3);
    const std::uint32_t rateIndex = (b2 >> 2) & 15U;
    if (rateIndex >= kRates.size()) {
        return std::nullopt;
    }
    FrameHeader header;
    header.crc = (loadU8(bytes, 1) & 1U) == 0;
    header.profile = static_cast<std::uint8_t>(b2 >> 6);
    header.sampleRate = kRates[rateIndex];
    const std::uint32_t configuration = ((b2 & 1U) << 2) | (b3 >> 6);
    header.channels = configuration == 7 ? 8 : configuration;
    header.length = ((b3 & 3U) << 11) | (std::uint32_t{loadU8(bytes, 4)} << 3) | (loadU8(bytes, 5) >> 5);
    header.samples = 1024 * ((loadU8(bytes, 6) & 3U) + 1);
    if (header.length < (header.crc ? 9U : 7U)) {
        return std::nullopt;
    }
    header.stream = (loadBe32(bytes, 0) & 0xFFFEFDC0U);  // sync, id, layer, profile, rate, channels
    return header;
}

// The first frame at or after `pos` (within kFrameSearch) whose header is
// followed by another of the same stream, or by the end of the audio.
Result<std::optional<std::uint64_t>> findFirstFrame(IContentReader& content, std::uint64_t pos, std::uint64_t end,
                                                    HeaderParser parse) {
    const std::uint64_t last = std::min(end, pos + kFrameSearch);
    for (std::uint64_t at = pos; at < last; ++at) {
        Result<std::span<const std::byte>> bytes = readUpTo(content, at, 8);
        if (!bytes.ok()) {
            return bytes.error();
        }
        if (bytes->empty() || loadU8(*bytes, 0) != 0xFF) {
            continue;
        }
        const std::optional<FrameHeader> first = parse(*bytes);
        if (!first.has_value()) {
            continue;
        }
        const std::uint64_t next = at + first->length;
        if (next == end) {
            return std::optional<std::uint64_t>{at};
        }
        Result<std::span<const std::byte>> following = readUpTo(content, next, 8);
        if (!following.ok()) {
            return following.error();
        }
        const std::optional<FrameHeader> second = parse(*following);
        if (second.has_value() && second->stream == first->stream) {
            return std::optional<std::uint64_t>{at};
        }
    }
    return std::optional<std::uint64_t>{};
}

struct Walk {
    std::uint64_t frames = 0;
    std::uint64_t samples = 0;
    std::uint64_t bytes = 0;
    std::set<std::uint32_t> bitrates;
    // Where the walk stopped, and whether it stopped at the scan limit.
    std::uint64_t end = 0;
    bool limited = false;
};

// Walks the frames of the stream whose first frame is at `start`, up to
// `end`, the scan limit, or the first header that does not belong.
Result<Walk> walkFrames(Extraction& x, std::uint64_t start, std::uint64_t end, const FrameHeader& first,
                        HeaderParser parse) {
    Walk walk;
    std::uint64_t pos = start;
    while (pos < end) {
        if (pos - start >= x.options.maxScanBytes) {
            walk.limited = true;
            break;
        }
        Result<std::span<const std::byte>> bytes = readUpTo(x.content, pos, 8);
        if (!bytes.ok()) {
            return bytes.error();
        }
        const std::optional<FrameHeader> header = parse(*bytes);
        if (!header.has_value() || header->stream != first.stream || pos + header->length > end) {
            break;
        }
        ++walk.frames;
        walk.samples += header->samples;
        walk.bytes += header->length;
        walk.bitrates.insert(header->bitrate);
        pos += header->length;
    }
    walk.end = pos;
    return walk;
}

// The duration of a walked stream, extrapolated over [start, end) when the
// walk stopped at the scan limit.
void walkDuration(Extraction& x, const Walk& walk, std::uint64_t start, std::uint64_t end, std::uint32_t rate) {
    std::optional<MediaDuration> walked = durationOf(walk.samples, rate);
    if (!walked.has_value()) {
        return;
    }
    if (walk.limited && walk.bytes > 0) {
        const double scale = static_cast<double>(end - start) / static_cast<double>(walk.bytes);
        walked = MediaDuration{static_cast<MediaDuration::rep>(static_cast<double>(walked->count()) * scale)};
        x.out.durationEstimated = true;
        x.issue(start, "frames beyond the first " + mib(x.options.maxScanBytes) +
                           " not counted: the duration is extrapolated");
    }
    x.out.duration = walked;
}

}  // namespace

// ---------------------------------------------------------------------------
// MP3
// ---------------------------------------------------------------------------

Status extractMp3(Extraction& x) {
    MediaTags tags;
    Result<std::uint64_t> start = leadingTags(x, tags);
    if (!start.ok()) {
        return start.error();
    }
    Result<std::uint64_t> audioEnd = readId3v1(x, x.content.size(), tags);
    if (!audioEnd.ok()) {
        return audioEnd.error();
    }
    x.out.tags = std::move(tags);
    Result<std::optional<std::uint64_t>> found = findFirstFrame(x.content, *start, *audioEnd, mp3Header);
    if (!found.ok()) {
        return found.error();
    }
    if (!found->has_value()) {
        x.issue(*start, "no MPEG audio Layer III frame");
        return success();
    }
    const std::uint64_t frameAt = **found;
    Result<std::span<const std::byte>> read = readUpTo(x.content, frameAt, 200);
    if (!read.ok()) {
        return read.error();
    }
    const std::vector<std::byte> frame(read->begin(), read->end());
    const FrameHeader first = *mp3Header(frame);
    AudioStreamMetadata audio;
    audio.codec = "mp3";
    audio.profile = std::string(first.version == 3 ? "MPEG-1" : (first.version == 2 ? "MPEG-2" : "MPEG-2.5")) +
                    " Layer III";
    audio.sampleRate = first.sampleRate;
    audio.channels = first.channels;

    // An info tag in the first frame: Xing (variable bitrate) or Info
    // (constant), after the side information; or VBRI, 32 bytes in.
    const std::size_t sideInfo = first.version == 3 ? (first.mono ? 17 : 32) : (first.mono ? 9 : 17);
    const std::size_t xingAt = 4 + (first.crc ? 2 : 0) + sideInfo;
    std::uint64_t taggedFrames = 0;
    std::uint64_t taggedBytes = 0;
    bool tagged = false;
    bool variable = false;
    const std::span<const std::byte> bytes(frame);
    if (bytes.size() >= xingAt + 8 && (startsWith(bytes.subspan(xingAt), "Xing") ||
                                       startsWith(bytes.subspan(xingAt), "Info"))) {
        tagged = true;
        variable = startsWith(bytes.subspan(xingAt), "Xing");
        const std::uint32_t flags = loadBe32(bytes, xingAt + 4);
        std::size_t field = xingAt + 8;
        if ((flags & 1U) != 0 && bytes.size() >= field + 4) {
            taggedFrames = loadBe32(bytes, field);
            field += 4;
        }
        if ((flags & 2U) != 0 && bytes.size() >= field + 4) {
            taggedBytes = loadBe32(bytes, field);
        }
    } else if (bytes.size() >= 36 + 18 && startsWith(bytes.subspan(36), "VBRI")) {
        tagged = true;
        variable = true;
        taggedBytes = loadBe32(bytes, 36 + 10);
        taggedFrames = loadBe32(bytes, 36 + 14);
    }
    if (tagged && taggedFrames > 0) {
        x.out.duration = durationOf(taggedFrames * first.samples, first.sampleRate);
        const std::uint64_t audioBytes =
            taggedBytes > 0 ? taggedBytes : *audioEnd - std::min<std::uint64_t>(*audioEnd, frameAt);
        if (variable && x.out.duration.has_value()) {
            audio.bitrate = bitrateOf(audioBytes, *x.out.duration);
        } else {
            audio.bitrate = std::uint64_t{first.bitrate} * 1000;
        }
        audio.variableBitrate = variable;
    } else {
        // No info tag (or one without a frame count): the frames are counted.
        const std::uint64_t walkStart = tagged ? frameAt + first.length : frameAt;
        Result<Walk> walk = walkFrames(x, walkStart, *audioEnd, first, mp3Header);
        if (!walk.ok()) {
            return walk.error();
        }
        walkDuration(x, *walk, walkStart, *audioEnd, first.sampleRate);
        audio.variableBitrate = walk->bitrates.size() > 1;
        if (walk->bitrates.size() == 1) {
            audio.bitrate = std::uint64_t{*walk->bitrates.begin()} * 1000;
        } else if (const std::optional<MediaDuration> counted = durationOf(walk->samples, first.sampleRate);
                   counted.has_value()) {
            audio.bitrate = bitrateOf(walk->bytes, *counted);
        }
    }
    x.out.audio = std::move(audio);
    return success();
}

// ---------------------------------------------------------------------------
// ADTS
// ---------------------------------------------------------------------------

Status extractAdts(Extraction& x) {
    MediaTags tags;
    Result<std::uint64_t> start = leadingTags(x, tags);
    if (!start.ok()) {
        return start.error();
    }
    Result<std::uint64_t> audioEnd = readId3v1(x, x.content.size(), tags);
    if (!audioEnd.ok()) {
        return audioEnd.error();
    }
    x.out.tags = std::move(tags);
    Result<std::optional<std::uint64_t>> found = findFirstFrame(x.content, *start, *audioEnd, adtsHeader);
    if (!found.ok()) {
        return found.error();
    }
    if (!found->has_value()) {
        x.issue(*start, "no ADTS frame");
        return success();
    }
    Result<std::span<const std::byte>> read = readUpTo(x.content, **found, 8);
    if (!read.ok()) {
        return read.error();
    }
    const FrameHeader first = *adtsHeader(*read);
    static constexpr std::array<std::string_view, 4> kProfiles = {"AAC Main", "AAC LC", "AAC SSR", "AAC LTP"};
    AudioStreamMetadata audio;
    audio.codec = "aac";
    audio.profile = std::string(kProfiles[first.profile & 3U]);
    audio.sampleRate = first.sampleRate;
    audio.channels = first.channels;
    Result<Walk> walk = walkFrames(x, **found, *audioEnd, first, adtsHeader);
    if (!walk.ok()) {
        return walk.error();
    }
    walkDuration(x, *walk, **found, *audioEnd, first.sampleRate);
    if (const std::optional<MediaDuration> counted = durationOf(walk->samples, first.sampleRate);
        counted.has_value()) {
        audio.bitrate = bitrateOf(walk->bytes, *counted);
    }
    x.out.audio = std::move(audio);
    return success();
}

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kMaxRiffChunks = 4096;

std::string waveCodec(std::uint16_t tag) {
    switch (tag) {
        case 0x0001:
            return "pcm";
        case 0x0002:
            return "adpcm_ms";
        case 0x0003:
            return "pcm_float";
        case 0x0006:
            return "alaw";
        case 0x0007:
            return "mulaw";
        case 0x0011:
            return "adpcm_ima";
        case 0x0050:
            return "mp2";
        case 0x0055:
            return "mp3";
        default: {
            std::array<char, 16> text{};
            std::snprintf(text.data(), text.size(), "wav_0x%04x", static_cast<unsigned>(tag));
            return text.data();
        }
    }
}

// The subchunks of a LIST INFO chunk (held in memory).
void readInfo(Extraction& x, std::span<const std::byte> list, std::uint64_t base, MediaTags& tags) {
    std::size_t pos = 0;
    for (std::uint32_t items = 0; list.size() - pos >= 8 && items < kMaxRiffChunks; ++items) {
        const std::span<const std::byte> id = list.subspan(pos, 4);
        const std::uint32_t size = loadLe32(list, pos + 4);
        if (size > list.size() - pos - 8) {
            x.issue(base + pos, "a RIFF INFO item runs past its list");
            return;
        }
        const std::span<const std::byte> value = list.subspan(pos + 8, size);
        const auto text = [&] { return decodeText(value, TextEncoding::Utf8OrLatin1, x.options.maxTextLength); };
        if (startsWith(id, "INAM")) {
            tags.title = text();
        } else if (startsWith(id, "IART")) {
            tags.artist = text();
        } else if (startsWith(id, "IPRD")) {
            tags.album = text();
        } else if (startsWith(id, "IGNR")) {
            tags.genre = text();
        } else if (startsWith(id, "ICRD")) {
            const std::string date = text();
            tags.date = parseIsoDateTime(date);
            if (!tags.date.has_value() && !date.empty()) {
                x.issue(base + pos, "the RIFF INFO creation date is not a date");
            }
        } else if (startsWith(id, "IPRT") || startsWith(id, "ITRK")) {
            parseTrackNumber(text(), tags);
        }
        pos += 8 + std::size_t{size} + (size & 1U);
        if (pos > list.size()) {
            return;
        }
    }
}

}  // namespace

Status extractWav(Extraction& x) {
    carving::IContentReader& content = x.content;
    Result<std::span<const std::byte>> read = readUpTo(content, 0, 12);
    if (!read.ok()) {
        return read.error();
    }
    if (read->size() < 12 || !startsWith(*read, "RIFF") || !startsWith(read->subspan(8), "WAVE")) {
        x.issue(0, "no RIFF WAVE header");
        return success();
    }
    const std::uint32_t riffSize = loadLe32(*read, 4);
    // A size of 0 or 0xFFFFFFFF: written while streaming; the content decides.
    std::uint64_t end = content.size();
    if (riffSize != 0 && riffSize != 0xFFFFFFFFU) {
        end = std::min<std::uint64_t>(std::uint64_t{8} + riffSize, content.size());
    }
    struct Format {
        std::uint16_t tag = 0;
        std::uint16_t channels = 0;
        std::uint32_t rate = 0;
        std::uint32_t byteRate = 0;
        std::uint16_t blockAlign = 0;
        std::uint16_t bits = 0;
    };
    std::optional<Format> format;
    std::optional<std::uint32_t> factFrames;
    std::optional<std::uint64_t> dataPresent;
    bool dataComplete = true;
    MediaTags info;
    MediaTags id3;
    std::uint64_t pos = 12;
    for (std::uint32_t chunks = 0; pos + 8 <= end; ++chunks) {
        if (chunks >= kMaxRiffChunks) {
            x.issue(pos, "more than " + std::to_string(kMaxRiffChunks) + " RIFF chunks (the rest not read)");
            break;
        }
        Result<std::span<const std::byte>> header = readUpTo(content, pos, 8 + 40);
        if (!header.ok()) {
            return header.error();
        }
        const std::span<const std::byte> chunk = *header;
        const std::uint32_t size = loadLe32(chunk, 4);
        const std::uint64_t body = pos + 8;
        const std::span<const std::byte> head =
            chunk.subspan(8, static_cast<std::size_t>(std::min<std::uint64_t>(size, chunk.size() - 8)));
        if (startsWith(chunk, "fmt ") && !format.has_value()) {
            if (head.size() < 16) {
                x.issue(pos, "the WAV fmt chunk is cut short");
            } else {
                Format f;
                f.tag = loadLe16(head, 0);
                f.channels = loadLe16(head, 2);
                f.rate = loadLe32(head, 4);
                f.byteRate = loadLe32(head, 8);
                f.blockAlign = loadLe16(head, 12);
                f.bits = loadLe16(head, 14);
                if (f.tag == 0xFFFE && head.size() >= 40) {
                    // WAVE_FORMAT_EXTENSIBLE: the valid bits, and the format
                    // in the first two bytes of the sub-format GUID.
                    const std::uint16_t valid = loadLe16(head, 18);
                    f.bits = valid != 0 ? valid : f.bits;
                    f.tag = loadLe16(head, 24);
                }
                format = f;
            }
        } else if (startsWith(chunk, "fact") && head.size() >= 4) {
            factFrames = loadLe32(head, 0);
        } else if (startsWith(chunk, "data") && !dataPresent.has_value()) {
            const std::uint64_t available = content.size() - std::min(content.size(), body);
            if (size == 0xFFFFFFFFU) {
                dataPresent = available;
            } else {
                dataPresent = std::min<std::uint64_t>(size, available);
                if (size > available) {
                    dataComplete = false;
                    x.issue(pos, "the WAV data chunk runs past the end of the content");
                }
            }
        } else if (startsWith(chunk, "LIST") && head.size() >= 4 && startsWith(head, "INFO")) {
            if (size > x.options.maxTagBytes) {
                x.issue(pos, "the RIFF INFO list is larger than the tag limit: not read");
            } else {
                Result<std::vector<std::byte>> list = readBlock(content, body + 4, size - 4);
                if (!list.ok()) {
                    return list.error();
                }
                readInfo(x, *list, body + 4, info);
            }
        } else if (startsWith(chunk, "id3 ") || startsWith(chunk, "ID3 ")) {
            Result<Id3v2Result> tag = readId3v2(x, body, id3);
            if (!tag.ok()) {
                return tag.error();
            }
        }
        pos = body + size + (size & 1U);
    }
    // An ID3v2 tag says more than the INFO list; INFO fills the rest.
    fillTags(id3, info);
    x.out.tags = std::move(id3);
    if (!format.has_value()) {
        x.issue(std::nullopt, "the WAV file has no fmt chunk");
        return success();
    }
    AudioStreamMetadata audio;
    audio.codec = waveCodec(format->tag);
    audio.sampleRate = format->rate;
    audio.channels = format->channels;
    audio.bitsPerSample = format->bits;
    audio.bitrate = std::uint64_t{format->byteRate} * 8;
    const std::uint64_t data = dataPresent.value_or(0);
    const bool pcmLike = format->tag == 0x0001 || format->tag == 0x0003 || format->tag == 0x0006 ||
                         format->tag == 0x0007;
    if (!dataPresent.has_value()) {
        x.issue(std::nullopt, "the WAV file has no data chunk");
    } else if (pcmLike && format->blockAlign > 0) {
        x.out.duration = durationOf(data / format->blockAlign, format->rate);
        if (audio.bitrate == 0 && x.out.duration.has_value()) {
            audio.bitrate = bitrateOf(data, *x.out.duration);
        }
    } else if (factFrames.has_value() && dataComplete) {
        x.out.duration = durationOf(*factFrames, format->rate);
    } else if (format->byteRate > 0) {
        x.out.duration = durationOf(data, format->byteRate);
        x.out.durationEstimated = true;
    }
    x.out.audio = std::move(audio);
    return success();
}

}  // namespace recovery::metadata::detail
