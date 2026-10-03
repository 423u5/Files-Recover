#include "support/audio_builders.hpp"

#include "support/image_builders.hpp"
#include "support/test_files.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace recovery::test {

namespace {

// ---------------------------------------------------------------------------
// Byte and bit helpers
// ---------------------------------------------------------------------------

void put8(std::vector<std::byte>& out, std::uint64_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFF));
}

void putBe16(std::vector<std::byte>& out, std::uint64_t value) {
    put8(out, value >> 8);
    put8(out, value);
}

void putBe32(std::vector<std::byte>& out, std::uint64_t value) {
    putBe16(out, (value >> 16) & 0xFFFF);
    putBe16(out, value & 0xFFFF);
}

void putBe64(std::vector<std::byte>& out, std::uint64_t value) {
    putBe32(out, value >> 32);
    putBe32(out, value & 0xFFFFFFFF);
}

void putLe16(std::vector<std::byte>& out, std::uint64_t value) {
    put8(out, value);
    put8(out, value >> 8);
}

void putLe32(std::vector<std::byte>& out, std::uint64_t value) {
    putLe16(out, value & 0xFFFF);
    putLe16(out, (value >> 16) & 0xFFFF);
}

void putText(std::vector<std::byte>& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
}

// `text` in a field of `width` bytes, padded with zeros.
void putField(std::vector<std::byte>& out, std::string_view text, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        out.push_back(i < text.size() ? static_cast<std::byte>(text[i]) : std::byte{0});
    }
}

void append(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void patchBe16(std::vector<std::byte>& out, std::size_t offset, std::uint32_t value) {
    out.at(offset) = static_cast<std::byte>((value >> 8) & 0xFF);
    out.at(offset + 1) = static_cast<std::byte>(value & 0xFF);
}

std::uint8_t u8(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint8_t>(bytes[offset]);
}

std::uint32_t be32(std::span<const std::byte> bytes, std::size_t offset) {
    return (std::uint32_t{u8(bytes, offset)} << 24) | (std::uint32_t{u8(bytes, offset + 1)} << 16) |
           (std::uint32_t{u8(bytes, offset + 2)} << 8) | u8(bytes, offset + 3);
}

std::uint32_t le32(std::span<const std::byte> bytes, std::size_t offset) {
    return u8(bytes, offset) | (std::uint32_t{u8(bytes, offset + 1)} << 8) |
           (std::uint32_t{u8(bytes, offset + 2)} << 16) | (std::uint32_t{u8(bytes, offset + 3)} << 24);
}

// splitmix64: small, fast and good enough for test content.
class Random {
public:
    explicit Random(std::uint64_t seed) : state_(seed * 0x9E3779B97F4A7C15ULL + 1) {}

    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // In [0, bound).
    std::uint32_t below(std::uint32_t bound) { return bound == 0 ? 0 : static_cast<std::uint32_t>(next() % bound); }

private:
    std::uint64_t state_;
};

// Bits, most significant first.
class BitWriter {
public:
    void put(std::uint32_t value, unsigned count) {
        for (unsigned i = count; i-- > 0;) {
            if (bits_ % 8 == 0) {
                bytes_.push_back(std::byte{0});
            }
            if (((value >> i) & 1U) != 0) {
                bytes_.back() |= static_cast<std::byte>(0x80U >> (bits_ % 8));
            }
            ++bits_;
        }
    }

    void alignToByte() { bits_ = bytes_.size() * 8; }
    [[nodiscard]] std::size_t bits() const noexcept { return bits_; }
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_;
    std::size_t bits_ = 0;
};

// The CRC-16s of MPEG audio, bit by bit (independent of the format module's tables).
std::uint16_t crc16Mpeg(std::span<const std::byte> data, std::uint16_t crc = 0xFFFF) {
    for (const std::byte byte : data) {
        crc = static_cast<std::uint16_t>(crc ^ (static_cast<unsigned>(byte) << 8));
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint16_t>((crc & 0x8000) != 0 ? (crc << 1) ^ 0x8005 : crc << 1);
        }
    }
    return crc;
}

std::uint16_t crc16Arc(std::span<const std::byte> data, std::uint16_t crc = 0) {
    for (const std::byte byte : data) {
        crc = static_cast<std::uint16_t>(crc ^ static_cast<unsigned>(byte));
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint16_t>((crc & 1) != 0 ? (crc >> 1) ^ 0xA001 : crc >> 1);
        }
    }
    return crc;
}

std::vector<std::byte> syncsafe(std::uint32_t value) {
    std::vector<std::byte> out;
    for (int shift = 21; shift >= 0; shift -= 7) {
        put8(out, (value >> shift) & 0x7F);
    }
    return out;
}

// ---------------------------------------------------------------------------
// MP3
// ---------------------------------------------------------------------------

constexpr std::array<std::array<std::uint16_t, 16>, 2> kMp3Bitrates = {{
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
}};

struct Mp3Stream {
    bool mpeg1 = true;
    std::uint32_t versionBits = 3;
    std::uint32_t rateIndex = 0;
    std::uint32_t sampleRate = 44100;
    unsigned channels = 2;
    unsigned granules = 2;
    std::uint32_t sideInfo = 32;
    std::uint32_t mode = 1;
    std::uint32_t modeExtension = 2;
    bool crc = false;

    [[nodiscard]] std::uint32_t bitrate(std::uint32_t index) const { return kMp3Bitrates[mpeg1 ? 0 : 1][index]; }
    [[nodiscard]] std::uint32_t slots() const { return mpeg1 ? 144 : 72; }
    [[nodiscard]] std::uint32_t length(std::uint32_t index, std::uint32_t padding) const {
        return slots() * bitrate(index) * 1000 / sampleRate + padding;
    }
    [[nodiscard]] std::uint32_t overhead() const { return 4 + (crc ? 2 : 0) + sideInfo; }
    [[nodiscard]] std::uint32_t header(std::uint32_t index, std::uint32_t padding) const {
        return (0x7FFU << 21) | (versionBits << 19) | (1U << 17) | ((crc ? 0U : 1U) << 16) | (index << 12) |
               (rateIndex << 10) | (padding << 9) | (mode << 6) | (modeExtension << 4) | (1U << 2);
    }
};

Mp3Stream mp3Stream(const Mp3Options& options) {
    Mp3Stream stream;
    std::array<std::uint32_t, 3> rates = {44100, 48000, 32000};
    switch (options.version) {
    case MpegVersion::Mpeg1:
        break;
    case MpegVersion::Mpeg2:
        stream.mpeg1 = false;
        stream.versionBits = 2;
        rates = {22050, 24000, 16000};
        break;
    case MpegVersion::Mpeg25:
        stream.mpeg1 = false;
        stream.versionBits = 0;
        rates = {11025, 12000, 8000};
        break;
    }
    const auto rate = std::find(rates.begin(), rates.end(), options.sampleRate);
    if (rate == rates.end()) {
        throw std::invalid_argument("MP3 sample rate not allowed for the version");
    }
    stream.rateIndex = static_cast<std::uint32_t>(rate - rates.begin());
    stream.sampleRate = options.sampleRate;
    stream.channels = options.channels == Mp3Channels::Mono ? 1 : 2;
    stream.granules = stream.mpeg1 ? 2 : 1;
    stream.sideInfo = stream.mpeg1 ? (stream.channels == 1 ? 17 : 32) : (stream.channels == 1 ? 9 : 17);
    stream.mode = static_cast<std::uint32_t>(options.channels);
    stream.modeExtension = options.channels == Mp3Channels::JointStereo ? 2 : 0;  // mid/side, no intensity
    stream.crc = options.crc;
    return stream;
}

struct Mp3AudioFrame {
    std::uint32_t header = 0;
    std::uint32_t length = 0;
    std::vector<std::byte> sideInfo;
    // Where the frame's main data area starts in the main data stream.
    std::size_t area = 0;
};

// Side information for granules that use only the count1 region with table B.
std::vector<std::byte> mp3SideInfo(const Mp3Stream& stream, std::uint32_t mainDataBegin,
                                   const std::vector<std::uint32_t>& partBits, Random& random) {
    BitWriter bits;
    if (stream.mpeg1) {
        bits.put(mainDataBegin, 9);
        bits.put(0, stream.channels == 1 ? 5 : 3);
        bits.put(0, 4 * stream.channels);  // scfsi
    } else {
        bits.put(mainDataBegin, 8);
        bits.put(0, stream.channels == 1 ? 1 : 2);
    }
    for (const std::uint32_t part : partBits) {
        bits.put(part, 12);                       // part2_3_length
        bits.put(0, 9);                           // big_values
        bits.put(120 + random.below(30), 8);      // global_gain
        bits.put(0, stream.mpeg1 ? 4 : 9);        // scalefac_compress: no scale factor bits
        bits.put(0, 1);                           // window_switching_flag
        bits.put(0, 15 + 4 + 3);                  // table_select, region0_count, region1_count
        bits.put(0, stream.mpeg1 ? 2 : 1);        // preflag (MPEG-1), scalefac_scale
        bits.put(1, 1);                           // count1table_select: table B
    }
    std::vector<std::byte> out = bits.bytes();
    out.resize(stream.sideInfo, std::byte{0});
    return out;
}

// Main data of one granule and channel: count1 quadruples coded with table B
// (4 bits, the quadruple inverted, then one sign bit per non-zero value).
void mp3Part(BitWriter& bits, std::uint32_t budget, Random& random, std::uint32_t& used) {
    used = 0;
    for (int quadruple = 0; quadruple < 144; ++quadruple) {
        const std::uint32_t value = random.below(16) & random.below(16);  // mostly small
        const auto cost = 4U + static_cast<std::uint32_t>(std::popcount(value));
        if (used + cost > budget) {
            break;
        }
        bits.put(15 - value, 4);
        bits.put(random.below(1U << std::popcount(value)), static_cast<unsigned>(std::popcount(value)));
        used += cost;
    }
}

std::vector<std::byte> mp3InfoFrame(const Mp3Stream& stream, const Mp3Options& options,
                                    const std::vector<std::vector<std::byte>>& audio) {
    const bool lame = options.infoTag == Mp3InfoTag::Lame;
    const std::uint32_t xing = 4 + stream.sideInfo;
    const std::uint32_t needed = xing + 120 + (lame ? 36U : 0U);
    std::uint32_t index = 1;
    if (options.bitrate != 0) {
        const auto& table = kMp3Bitrates[stream.mpeg1 ? 0 : 1];
        index = static_cast<std::uint32_t>(std::find(table.begin() + 1, table.end() - 1, options.bitrate) -
                                           table.begin());
    }
    while (index < 15 && stream.length(index, 0) < needed) {
        ++index;
    }
    if (index == 15) {
        throw std::invalid_argument("no MPEG bitrate holds the info tag");
    }
    const std::uint32_t length = stream.length(index, 0);
    std::uint64_t streamBytes = length;
    std::uint16_t musicCrc = 0;
    for (const auto& frame : audio) {
        streamBytes += frame.size();
        musicCrc = crc16Arc(frame, musicCrc);
    }
    std::vector<std::byte> out;
    putBe32(out, stream.header(index, 0));
    out.resize(length, std::byte{0});
    std::vector<std::byte> tag;
    putText(tag, options.bitrate != 0 ? "Info" : "Xing");
    putBe32(tag, 0x0F);
    putBe32(tag, audio.size());
    putBe32(tag, streamBytes);
    std::uint64_t offset = length;
    std::size_t next = 0;
    for (std::size_t i = 0; i < 100; ++i) {
        const std::size_t frame = i * audio.size() / 100;
        for (; next < frame; ++next) {
            offset += audio[next].size();
        }
        put8(tag, std::min<std::uint64_t>(255, offset * 256 / streamBytes));
    }
    putBe32(tag, 100);  // quality
    if (lame) {
        putText(tag, "LAME3.100");
        put8(tag, options.bitrate != 0 ? 0x01 : 0x04);  // tag revision 0; CBR or VBR method
        put8(tag, 0xC4);                                  // lowpass / 100 Hz
        putBe32(tag, 0);                                  // peak amplitude
        putBe32(tag, 0);                                  // radio and audiophile replay gain
        put8(tag, 0);                                     // encoding flags, ATH type
        put8(tag, std::min<std::uint32_t>(255, options.bitrate));
        put8(tag, 0x24);  // encoder delay (576) and padding
        put8(tag, 0x00);
        put8(tag, 0x00);
        put8(tag, 0);       // misc
        put8(tag, 0);       // MP3 gain
        putBe16(tag, 0);    // preset and surround
        putBe32(tag, streamBytes);
        putBe16(tag, musicCrc);
        putBe16(tag, 0);    // the tag's own CRC, below
    }
    std::copy(tag.begin(), tag.end(), out.begin() + xing);
    if (stream.crc) {
        std::uint16_t crc = crc16Mpeg(std::span(out).subspan(2, 2));
        crc = crc16Mpeg(std::span(out).subspan(6, stream.sideInfo), crc);
        patchBe16(out, 4, crc);
    }
    if (lame) {
        const std::size_t crcOffset = xing + tag.size() - 2;
        patchBe16(out, crcOffset, crc16Arc(std::span(out).first(crcOffset)));
    }
    return out;
}

std::vector<std::vector<std::byte>> mp3AudioFrames(const Mp3Stream& stream, const Mp3Options& options) {
    Random random(options.seed);
    const auto& table = kMp3Bitrates[stream.mpeg1 ? 0 : 1];
    std::uint32_t cbrIndex = 0;
    if (options.bitrate != 0) {
        const auto found = std::find(table.begin() + 1, table.end() - 1, options.bitrate);
        if (found == table.end() - 1) {
            throw std::invalid_argument("MP3 bitrate not allowed for the version");
        }
        cbrIndex = static_cast<std::uint32_t>(found - table.begin());
    }
    const std::uint32_t maxBegin = stream.mpeg1 ? 511 : 255;
    const std::size_t parts = std::size_t{stream.granules} * stream.channels;
    std::vector<Mp3AudioFrame> frames;
    std::vector<std::byte> mainData;
    std::size_t dataEnd = 0;
    std::uint64_t remainder = 0;
    for (std::size_t n = 0; n < options.frames; ++n) {
        std::uint32_t index = cbrIndex;
        if (index == 0) {
            do {
                index = 1 + random.below(14);
            } while (stream.length(index, 0) < stream.overhead() + 4);
        }
        // Padding keeps the average bitrate exact, as encoders do.
        const std::uint64_t exact = std::uint64_t{stream.slots()} * stream.bitrate(index) * 1000;
        remainder += exact % stream.sampleRate;
        std::uint32_t padding = 0;
        if (remainder >= stream.sampleRate) {
            remainder -= stream.sampleRate;
            padding = 1;
        }
        Mp3AudioFrame frame;
        frame.header = stream.header(index, padding);
        frame.length = stream.length(index, padding);
        frame.area = mainData.size();
        const std::uint32_t area = frame.length - stream.overhead();
        mainData.resize(mainData.size() + area, std::byte{0});
        const auto begin = options.reservoir
                               ? static_cast<std::uint32_t>(std::min<std::size_t>(frame.area - dataEnd, maxBegin))
                               : 0U;
        // Use 30 to 90 percent of what is available, and never more than the parts can hold.
        const std::uint32_t capacity = (begin + area) * 8;
        std::uint32_t budget = capacity * (30 + random.below(61)) / 100;
        budget = std::min<std::uint32_t>(budget, static_cast<std::uint32_t>(parts) * 144 * 8);
        BitWriter bits;
        std::vector<std::uint32_t> partBits;
        for (std::size_t part = 0; part < parts; ++part) {
            std::uint32_t used = 0;
            mp3Part(bits, budget / static_cast<std::uint32_t>(parts), random, used);
            partBits.push_back(used);
        }
        const std::size_t start = frame.area - begin;
        std::copy(bits.bytes().begin(), bits.bytes().end(), mainData.begin() + static_cast<std::ptrdiff_t>(start));
        dataEnd = start + bits.bytes().size();
        frame.sideInfo = mp3SideInfo(stream, begin, partBits, random);
        frames.push_back(std::move(frame));
    }
    std::vector<std::vector<std::byte>> out;
    for (const Mp3AudioFrame& frame : frames) {
        std::vector<std::byte> bytes;
        putBe32(bytes, frame.header);
        if (stream.crc) {
            putBe16(bytes, 0);
        }
        append(bytes, frame.sideInfo);
        const std::uint32_t area = frame.length - stream.overhead();
        append(bytes, std::span(mainData).subspan(frame.area, area));
        if (stream.crc) {
            std::uint16_t crc = crc16Mpeg(std::span(bytes).subspan(2, 2));
            crc = crc16Mpeg(frame.sideInfo, crc);
            patchBe16(bytes, 4, crc);
        }
        out.push_back(std::move(bytes));
    }
    return out;
}

// ---------------------------------------------------------------------------
// AAC
// ---------------------------------------------------------------------------

constexpr std::array<std::uint32_t, 13> kAacRates = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                     22050, 16000, 12000, 11025, 8000,  7350};

std::uint32_t aacRateIndex(std::uint32_t rate) {
    const auto found = std::find(kAacRates.begin(), kAacRates.end(), rate);
    if (found == kAacRates.end()) {
        throw std::invalid_argument("not an AAC sampling frequency");
    }
    return static_cast<std::uint32_t>(found - kAacRates.begin());
}

// An individual channel stream with no scale factor bands: silence.
void silentChannel(BitWriter& bits) {
    bits.put(100, 8);  // global_gain
    bits.put(0, 1);    // ics_reserved_bit
    bits.put(0, 2);    // window_sequence: ONLY_LONG_SEQUENCE
    bits.put(0, 1);    // window_shape
    bits.put(0, 6);    // max_sfb
    bits.put(0, 1);    // predictor_data_present
    bits.put(0, 1);    // pulse_data_present
    bits.put(0, 1);    // tns_data_present
    bits.put(0, 1);    // gain_control_data_present
}

std::vector<std::vector<std::byte>> aacFrames(const AacOptions& options) {
    if (options.channels != 1 && options.channels != 2) {
        throw std::invalid_argument("AAC builder: 1 or 2 channels");
    }
    Random random(options.seed);
    std::vector<std::vector<std::byte>> frames;
    for (std::size_t i = 0; i < options.frames; ++i) {
        frames.push_back(aacSilentFrame(options.channels, 20 + random.below(280)));
    }
    return frames;
}

// ---------------------------------------------------------------------------
// ISO BMFF
// ---------------------------------------------------------------------------

std::vector<std::byte> fullBox(std::string_view type, std::uint32_t version, std::uint32_t flags,
                               std::span<const std::byte> payload) {
    std::vector<std::byte> body;
    putBe32(body, (version << 24) | flags);
    append(body, payload);
    return mp4Box(type, body);
}

std::vector<std::byte> unityMatrix() {
    std::vector<std::byte> out;
    for (const std::uint32_t value : {0x00010000U, 0U, 0U, 0U, 0x00010000U, 0U, 0U, 0U, 0x40000000U}) {
        putBe32(out, value);
    }
    return out;
}

std::vector<std::byte> descriptor(std::uint8_t tag, std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    put8(out, tag);
    put8(out, payload.size());  // every descriptor here is shorter than 128 bytes
    append(out, payload);
    return out;
}

std::vector<std::byte> esds(const AacOptions& audio) {
    std::vector<std::byte> config;
    BitWriter bits;
    bits.put(audio.profile + 1U, 5);
    bits.put(aacRateIndex(audio.sampleRate), 4);
    bits.put(audio.channels, 4);
    bits.put(0, 3);  // frameLengthFlag, dependsOnCoreCoder, extensionFlag
    append(config, bits.bytes());
    std::vector<std::byte> decoder;
    put8(decoder, 0x40);  // MPEG-4 audio
    put8(decoder, 0x15);  // audio stream
    put8(decoder, 0);     // bufferSizeDB (24 bits)
    putBe16(decoder, 0x0300);
    putBe32(decoder, 128000);
    putBe32(decoder, 96000);
    append(decoder, descriptor(0x05, config));
    std::vector<std::byte> es;
    putBe16(es, 1);  // ES_ID
    put8(es, 0);     // flags
    append(es, descriptor(0x04, decoder));
    const std::vector<std::byte> sl = {std::byte{0x02}};
    append(es, descriptor(0x06, sl));
    return fullBox("esds", 0, 0, descriptor(0x03, es));
}

struct TrackData {
    std::vector<std::uint32_t> sizes;
    // Samples per chunk, and the chunks' offsets in the file.
    std::size_t samplesPerChunk = 1;
    std::vector<std::uint64_t> chunkOffsets;
};

std::vector<std::byte> sampleTables(std::span<const std::byte> description, std::uint32_t delta,
                                    const TrackData& track, bool co64) {
    std::vector<std::byte> stbl;
    std::vector<std::byte> stsd;
    putBe32(stsd, 1);
    append(stsd, description);
    append(stbl, fullBox("stsd", 0, 0, stsd));
    std::vector<std::byte> stts;
    putBe32(stts, 1);
    putBe32(stts, track.sizes.size());
    putBe32(stts, delta);
    append(stbl, fullBox("stts", 0, 0, stts));
    std::vector<std::byte> stsc;
    const std::size_t chunks = track.chunkOffsets.size();
    const std::size_t last = track.sizes.size() - (chunks - 1) * track.samplesPerChunk;
    const bool uneven = last != track.samplesPerChunk;
    putBe32(stsc, uneven && chunks > 1 ? 2 : 1);
    putBe32(stsc, 1);
    putBe32(stsc, chunks > 1 || !uneven ? track.samplesPerChunk : last);
    putBe32(stsc, 1);
    if (uneven && chunks > 1) {
        putBe32(stsc, chunks);
        putBe32(stsc, last);
        putBe32(stsc, 1);
    }
    append(stbl, fullBox("stsc", 0, 0, stsc));
    std::vector<std::byte> stsz;
    putBe32(stsz, 0);
    putBe32(stsz, track.sizes.size());
    for (const std::uint32_t size : track.sizes) {
        putBe32(stsz, size);
    }
    append(stbl, fullBox("stsz", 0, 0, stsz));
    std::vector<std::byte> offsets;
    putBe32(offsets, chunks);
    for (const std::uint64_t offset : track.chunkOffsets) {
        if (co64) {
            putBe64(offsets, offset);
        } else {
            putBe32(offsets, offset);
        }
    }
    append(stbl, fullBox(co64 ? "co64" : "stco", 0, 0, offsets));
    return mp4Box("stbl", stbl);
}

std::vector<std::byte> dataInformation() {
    std::vector<std::byte> dref;
    putBe32(dref, 1);
    append(dref, fullBox("url ", 0, 1, {}));
    return mp4Box("dinf", fullBox("dref", 0, 0, dref));
}

std::vector<std::byte> handler(std::string_view type, std::string_view name) {
    std::vector<std::byte> payload;
    putBe32(payload, 0);
    putText(payload, type);
    putField(payload, "", 12);
    putText(payload, name);
    put8(payload, 0);
    return fullBox("hdlr", 0, 0, payload);
}

std::vector<std::byte> trackHeader(std::uint32_t id, std::uint32_t duration, bool video) {
    std::vector<std::byte> payload;
    putBe32(payload, 0);  // creation
    putBe32(payload, 0);  // modification
    putBe32(payload, id);
    putBe32(payload, 0);
    putBe32(payload, duration);
    putField(payload, "", 8);
    putBe16(payload, 0);                  // layer
    putBe16(payload, video ? 0 : 1);      // alternate group
    putBe16(payload, video ? 0 : 0x0100);  // volume
    putBe16(payload, 0);
    append(payload, unityMatrix());
    putBe32(payload, video ? 320U << 16 : 0U);
    putBe32(payload, video ? 240U << 16 : 0U);
    return fullBox("tkhd", 0, 3, payload);
}

std::vector<std::byte> mediaHeader(std::uint32_t timescale, std::uint32_t duration) {
    std::vector<std::byte> payload;
    putBe32(payload, 0);
    putBe32(payload, 0);
    putBe32(payload, timescale);
    putBe32(payload, duration);
    putBe16(payload, 0x55C4);  // "und"
    putBe16(payload, 0);
    return fullBox("mdhd", 0, 0, payload);
}

std::vector<std::byte> soundTrack(const AacOptions& audio, const TrackData& data, bool co64) {
    const auto duration = static_cast<std::uint32_t>(data.sizes.size() * 1024);
    std::vector<std::byte> entry;
    putField(entry, "", 6);
    putBe16(entry, 1);  // data reference index
    putField(entry, "", 8);
    putBe16(entry, audio.channels);
    putBe16(entry, 16);
    putBe32(entry, 0);
    putBe32(entry, audio.sampleRate << 16);
    append(entry, esds(audio));
    const std::vector<std::byte> description = mp4Box("mp4a", entry);
    std::vector<std::byte> smhd;
    putBe32(smhd, 0);
    std::vector<std::byte> minf = fullBox("smhd", 0, 0, smhd);
    append(minf, dataInformation());
    append(minf, sampleTables(description, 1024, data, co64));
    std::vector<std::byte> mdia = mediaHeader(audio.sampleRate, duration);
    append(mdia, handler("soun", "SoundHandler"));
    append(mdia, mp4Box("minf", minf));
    std::vector<std::byte> trak = trackHeader(1, duration * 1000 / audio.sampleRate, false);
    append(trak, mp4Box("mdia", mdia));
    return mp4Box("trak", trak);
}

std::vector<std::byte> videoTrack(const TrackData& data, bool co64) {
    std::vector<std::byte> entry;
    putField(entry, "", 6);
    putBe16(entry, 1);
    putField(entry, "", 16);
    putBe16(entry, 320);
    putBe16(entry, 240);
    putBe32(entry, 0x00480000);
    putBe32(entry, 0x00480000);
    putBe32(entry, 0);
    putBe16(entry, 1);
    putField(entry, "", 32);
    putBe16(entry, 0x0018);
    putBe16(entry, 0xFFFF);
    const std::vector<std::byte> description = mp4Box("avc1", entry);
    std::vector<std::byte> vmhd;
    putBe32(vmhd, 0);
    putBe32(vmhd, 0);
    std::vector<std::byte> minf = fullBox("vmhd", 0, 1, vmhd);
    append(minf, dataInformation());
    append(minf, sampleTables(description, 1001, data, co64));
    const auto duration = static_cast<std::uint32_t>(data.sizes.size() * 1001);
    std::vector<std::byte> mdia = mediaHeader(30000, duration);
    append(mdia, handler("vide", "VideoHandler"));
    append(mdia, mp4Box("minf", minf));
    std::vector<std::byte> trak = trackHeader(2, duration / 30, true);
    append(trak, mp4Box("mdia", mdia));
    return mp4Box("trak", trak);
}

std::vector<std::byte> metadata() {
    std::vector<std::byte> data;
    putBe32(data, 1);  // UTF-8
    putBe32(data, 0);
    putText(data, "Title");
    std::vector<std::byte> name = mp4Box("data", data);
    std::vector<std::byte> item;
    put8(item, 0xA9);
    putText(item, "nam");
    std::vector<std::byte> ilst = mp4Box(std::string_view(reinterpret_cast<const char*>(item.data()), 4), name);
    std::vector<std::byte> hdlr;
    putBe32(hdlr, 0);
    putText(hdlr, "mdirappl");
    putField(hdlr, "", 9);
    std::vector<std::byte> meta = fullBox("hdlr", 0, 0, hdlr);
    append(meta, mp4Box("ilst", ilst));
    return mp4Box("udta", fullBox("meta", 0, 0, meta));
}

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------

constexpr std::array<std::uint8_t, 14> kSubtypeSuffix = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                         0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

}  // namespace

// ---------------------------------------------------------------------------
// MP3
// ---------------------------------------------------------------------------

std::vector<std::byte> id3v2Tag(std::uint8_t version, std::span<const std::byte> picture) {
    if (version < 2 || version > 4) {
        throw std::invalid_argument("ID3v2 version 2, 3 or 4");
    }
    std::vector<std::byte> frames;
    const auto frame = [&](std::string_view id3, std::string_view id, std::span<const std::byte> body) {
        if (version == 2) {
            putText(frames, id3);
            put8(frames, body.size() >> 16);
            putBe16(frames, body.size() & 0xFFFF);
        } else {
            putText(frames, id);
            if (version == 4) {
                append(frames, syncsafe(static_cast<std::uint32_t>(body.size())));
            } else {
                putBe32(frames, body.size());
            }
            putBe16(frames, 0);
        }
        append(frames, body);
    };
    std::vector<std::byte> title;
    put8(title, 0);
    putText(title, "Test title");
    frame("TT2", "TIT2", title);
    std::vector<std::byte> artist;
    put8(artist, 0);
    putText(artist, "Test artist");
    frame("TP1", "TPE1", artist);
    if (!picture.empty()) {
        std::vector<std::byte> body;
        put8(body, 0);
        if (version == 2) {
            putText(body, "JPG");
        } else {
            putText(body, "image/jpeg");
            put8(body, 0);
        }
        put8(body, 3);  // front cover
        put8(body, 0);  // empty description
        append(body, picture);
        frame("PIC", "APIC", body);
    }
    frames.resize(frames.size() + 64, std::byte{0});
    std::vector<std::byte> out;
    putText(out, "ID3");
    put8(out, version);
    put8(out, 0);
    put8(out, 0);
    append(out, syncsafe(static_cast<std::uint32_t>(frames.size())));
    append(out, frames);
    return out;
}

std::vector<std::byte> id3v1Tag() {
    std::vector<std::byte> out;
    putText(out, "TAG");
    putField(out, "Test title", 30);
    putField(out, "Test artist", 30);
    putField(out, "Test album", 30);
    putField(out, "2026", 4);
    putField(out, "Test comment", 28);
    put8(out, 0);
    put8(out, 1);   // track
    put8(out, 12);  // genre
    return out;
}

std::vector<std::byte> apeTag() {
    std::vector<std::byte> items;
    const auto item = [&](std::string_view key, std::string_view value) {
        putLe32(items, value.size());
        putLe32(items, 0);
        putText(items, key);
        put8(items, 0);
        putText(items, value);
    };
    item("Title", "Test title");
    item("Artist", "Test artist");
    const auto marker = [&](std::vector<std::byte>& out, std::uint32_t flags) {
        putText(out, "APETAGEX");
        putLe32(out, 2000);
        putLe32(out, items.size() + 32);
        putLe32(out, 2);
        putLe32(out, flags);
        putField(out, "", 8);
    };
    std::vector<std::byte> out;
    marker(out, 0xA0000000U);  // has a header; this is the header
    append(out, items);
    marker(out, 0x80000000U);  // has a header; this is the footer
    return out;
}

std::vector<std::byte> lyrics3Tag() {
    std::vector<std::byte> out;
    putText(out, "LYRICSBEGIN");
    putText(out, "IND0000210");
    putText(out, "LYR00015[00:01]Test line");
    const std::string size = std::to_string(out.size());
    putText(out, std::string(6 - size.size(), '0') + size);
    putText(out, "LYRICS200");
    return out;
}

std::vector<std::byte> makeMp3(const Mp3Options& options) {
    if (options.frames == 0) {
        throw std::invalid_argument("empty MP3");
    }
    const Mp3Stream stream = mp3Stream(options);
    const std::vector<std::vector<std::byte>> audio = mp3AudioFrames(stream, options);
    std::vector<std::byte> out;
    if (options.id3v2 != 0) {
        append(out, id3v2Tag(options.id3v2, options.picture));
    }
    if (options.infoTag != Mp3InfoTag::None) {
        append(out, mp3InfoFrame(stream, options, audio));
    }
    for (const auto& frame : audio) {
        append(out, frame);
    }
    if (options.apeTag) {
        append(out, apeTag());
    }
    if (options.lyrics3) {
        append(out, lyrics3Tag());
    }
    if (options.id3v1) {
        append(out, id3v1Tag());
    }
    return out;
}

std::vector<Mp3Frame> mp3Frames(std::span<const std::byte> file) {
    std::vector<Mp3Frame> frames;
    std::size_t position = 0;
    if (file.size() >= 10 && u8(file, 0) == 'I' && u8(file, 1) == 'D' && u8(file, 2) == '3') {
        const std::uint32_t size = (std::uint32_t{u8(file, 6)} << 21) | (std::uint32_t{u8(file, 7)} << 14) |
                                   (std::uint32_t{u8(file, 8)} << 7) | u8(file, 9);
        position = 10 + size;
    }
    std::uint32_t key = 0;
    while (position + 4 <= file.size()) {
        const std::uint32_t header = be32(file, position);
        const std::uint32_t version = (header >> 19) & 3;
        const std::uint32_t index = (header >> 12) & 0xF;
        const std::uint32_t rate = (header >> 10) & 3;
        if ((header >> 21) != 0x7FF || version == 1 || ((header >> 17) & 3) != 1 || index == 0 || index == 15 ||
            rate == 3 || (key != 0 && ((header & 0xFFFF0C00) | ((header >> 6) & 3) / 3) != key)) {
            break;
        }
        // Version, layer, protection, sample rate, and whether the frame is mono.
        key = (header & 0xFFFF0C00) | ((header >> 6) & 3) / 3;
        const bool mpeg1 = version == 3;
        const std::uint32_t sampleRate = std::array<std::uint32_t, 3>{44100, 48000, 32000}[rate] >>
                                         (mpeg1 ? 0 : version == 2 ? 1 : 2);
        const std::uint32_t length =
            (mpeg1 ? 144 : 72) * kMp3Bitrates[mpeg1 ? 0 : 1][index] * 1000 / sampleRate + ((header >> 9) & 1);
        frames.push_back(Mp3Frame{position, length});
        position += length;
    }
    return frames;
}

// ---------------------------------------------------------------------------
// AAC
// ---------------------------------------------------------------------------

std::vector<std::byte> aacSilentFrame(std::uint8_t channels, std::size_t size) {
    BitWriter bits;
    if (channels == 1) {
        bits.put(0, 3);  // ID_SCE
        bits.put(0, 4);
        silentChannel(bits);
    } else {
        bits.put(1, 3);  // ID_CPE
        bits.put(0, 4);
        bits.put(0, 1);  // common_window
        silentChannel(bits);
        silentChannel(bits);
    }
    // Fill elements up to the size: EXT_FILL_DATA, a zero nibble, then 0xA5 bytes.
    for (;;) {
        const std::size_t used = (bits.bits() + 3 + 7) / 8;  // with the END element
        if (used + 2 > size) {
            break;
        }
        const std::size_t count = std::min<std::size_t>(size - used - 1, 269);
        bits.put(6, 3);  // ID_FIL
        if (count < 15) {
            bits.put(static_cast<std::uint32_t>(count), 4);
        } else {
            bits.put(15, 4);
            bits.put(static_cast<std::uint32_t>(count - 14), 8);
        }
        if (count > 0) {
            bits.put(1, 4);  // EXT_FILL_DATA
            bits.put(0, 4);  // fill_nibble
            for (std::size_t i = 1; i < count; ++i) {
                bits.put(0xA5, 8);
            }
        }
    }
    bits.put(7, 3);  // ID_END
    bits.alignToByte();
    return bits.bytes();
}

std::vector<std::byte> makeAdts(const AacOptions& options) {
    const std::uint32_t rate = aacRateIndex(options.sampleRate);
    std::vector<std::byte> out;
    if (options.id3v2 != 0) {
        append(out, id3v2Tag(options.id3v2));
    }
    for (const auto& frame : aacFrames(options)) {
        const auto length = static_cast<std::uint32_t>(frame.size() + 7);
        BitWriter header;
        header.put(0xFFF, 12);
        header.put(options.mpeg2 ? 1 : 0, 1);
        header.put(0, 2);  // layer
        header.put(1, 1);  // protection_absent
        header.put(options.profile, 2);
        header.put(rate, 4);
        header.put(0, 1);  // private_bit
        header.put(options.channels, 3);
        header.put(0, 4);  // original_copy, home, copyright bits
        header.put(length, 13);
        header.put(0x7FF, 11);  // buffer fullness: variable bitrate
        header.put(0, 2);       // one raw data block
        append(out, header.bytes());
        append(out, frame);
    }
    if (options.id3v1) {
        append(out, id3v1Tag());
    }
    return out;
}

std::vector<std::size_t> adtsFrames(std::span<const std::byte> file) {
    std::vector<std::size_t> frames;
    std::size_t position = 0;
    if (file.size() >= 10 && u8(file, 0) == 'I' && u8(file, 1) == 'D' && u8(file, 2) == '3') {
        position = 10 + ((std::uint32_t{u8(file, 6)} << 21) | (std::uint32_t{u8(file, 7)} << 14) |
                         (std::uint32_t{u8(file, 8)} << 7) | u8(file, 9));
    }
    while (position + 7 <= file.size() && u8(file, position) == 0xFF && (u8(file, position + 1) & 0xF6) == 0xF0) {
        frames.push_back(position);
        const std::uint32_t length = ((u8(file, position + 3) & 3U) << 11) | (std::uint32_t{u8(file, position + 4)} << 3) |
                                     (u8(file, position + 5) >> 5);
        position += length;
    }
    return frames;
}

std::vector<std::byte> mp4Box(std::string_view type, std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    putBe32(out, payload.size() + 8);
    putText(out, type);
    append(out, payload);
    return out;
}

std::vector<std::byte> makeM4a(const M4aOptions& options) {
    if (options.samplesPerChunk == 0 || options.audio.frames == 0) {
        throw std::invalid_argument("invalid M4A options");
    }
    // The media data: audio chunks, then one chunk of video samples.
    std::vector<std::byte> media;
    TrackData audio;
    audio.samplesPerChunk = options.samplesPerChunk;
    std::vector<std::uint64_t> audioChunks;
    const std::vector<std::vector<std::byte>> frames = aacFrames(options.audio);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (i % options.samplesPerChunk == 0) {
            audioChunks.push_back(media.size());
        }
        audio.sizes.push_back(static_cast<std::uint32_t>(frames[i].size()));
        append(media, frames[i]);
    }
    TrackData video;
    std::vector<std::uint64_t> videoChunks;
    if (options.videoTrack) {
        videoChunks.push_back(media.size());
        for (std::uint64_t i = 0; i < 3; ++i) {
            video.sizes.push_back(100);
            append(media, makePattern(100, options.audio.seed + i));
        }
        video.samplesPerChunk = 3;
    }

    std::vector<std::byte> ftypPayload;
    putText(ftypPayload, options.majorBrand);
    putBe32(ftypPayload, 0x200);
    for (const std::string& brand : options.compatibleBrands) {
        putText(ftypPayload, brand);
    }
    const std::vector<std::byte> ftyp = mp4Box("ftyp", ftypPayload);
    const std::vector<std::byte> free = options.freeBox ? mp4Box("free", {}) : std::vector<std::byte>{};
    const std::size_t mdatHeader = options.largeMdat ? 16 : 8;

    const auto buildMoov = [&](std::uint64_t mediaStart) {
        const auto place = [&](TrackData& track, const std::vector<std::uint64_t>& chunks) {
            track.chunkOffsets.clear();
            for (const std::uint64_t chunk : chunks) {
                track.chunkOffsets.push_back(mediaStart + chunk);
            }
        };
        place(audio, audioChunks);
        place(video, videoChunks);
        const auto duration = static_cast<std::uint32_t>(audio.sizes.size() * 1024 * 1000 / options.audio.sampleRate);
        std::vector<std::byte> mvhd;
        putBe32(mvhd, 0);
        putBe32(mvhd, 0);
        putBe32(mvhd, 1000);
        putBe32(mvhd, duration);
        putBe32(mvhd, 0x00010000);
        putBe16(mvhd, 0x0100);
        putField(mvhd, "", 10);
        append(mvhd, unityMatrix());
        putField(mvhd, "", 24);
        putBe32(mvhd, 3);
        std::vector<std::byte> moov = fullBox("mvhd", 0, 0, mvhd);
        if (options.soundTrack) {
            append(moov, soundTrack(options.audio, audio, options.co64));
        }
        if (options.videoTrack) {
            append(moov, videoTrack(video, options.co64));
        }
        if (options.metadata) {
            append(moov, metadata());
        }
        return mp4Box("moov", moov);
    };
    const std::size_t moovSize = buildMoov(0).size();
    const std::uint64_t mediaStart =
        ftyp.size() + (options.moovFirst ? moovSize : 0) + free.size() + mdatHeader;
    const std::vector<std::byte> moov = buildMoov(mediaStart);

    std::vector<std::byte> out = ftyp;
    if (options.moovFirst) {
        append(out, moov);
    }
    append(out, free);
    if (options.largeMdat) {
        putBe32(out, 1);
        putText(out, "mdat");
        putBe64(out, media.size() + 16);
    } else {
        putBe32(out, media.size() + 8);
        putText(out, "mdat");
    }
    append(out, media);
    if (!options.moovFirst) {
        append(out, moov);
    }
    return out;
}

std::vector<BoxPosition> m4aBoxes(std::span<const std::byte> file) {
    std::vector<BoxPosition> boxes;
    const auto walk = [&](const auto& self, std::size_t from, std::size_t to, const std::string& parent) -> void {
        std::size_t position = from;
        while (position + 8 <= to) {
            std::uint64_t size = be32(file, position);
            std::size_t header = 8;
            if (size == 1) {
                size = (std::uint64_t{be32(file, position + 8)} << 32) | be32(file, position + 12);
                header = 16;
            }
            if (size < header || size > to - position) {
                return;
            }
            std::string type;
            for (std::size_t i = 4; i < 8; ++i) {
                type += static_cast<char>(u8(file, position + i));
            }
            const std::string path = parent.empty() ? type : parent + "/" + type;
            boxes.push_back(BoxPosition{path, position, static_cast<std::size_t>(size)});
            static const std::array<std::string_view, 8> kContainers = {"moov", "trak", "mdia", "minf",
                                                                         "stbl", "dinf", "udta", "edts"};
            if (std::find(kContainers.begin(), kContainers.end(), type) != kContainers.end()) {
                self(self, position + header, position + static_cast<std::size_t>(size), path);
            } else if (type == "meta" && !parent.empty()) {
                self(self, position + header + 4, position + static_cast<std::size_t>(size), path);
            }
            position += static_cast<std::size_t>(size);
        }
    };
    walk(walk, 0, file.size(), "");
    return boxes;
}

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------

std::vector<std::byte> makeWav(const WavOptions& options) {
    if (options.channels == 0 || options.sampleRate == 0) {
        throw std::invalid_argument("invalid WAV options");
    }
    std::uint16_t tag = 1;
    std::uint16_t bits = options.bitsPerSample;
    std::uint16_t blockAlign = 0;
    std::uint16_t samplesPerBlock = 0;
    switch (options.encoding) {
    case WavEncoding::Pcm:
        break;
    case WavEncoding::Float:
        tag = 3;
        break;
    case WavEncoding::ALaw:
        tag = 6;
        bits = 8;
        break;
    case WavEncoding::MuLaw:
        tag = 7;
        bits = 8;
        break;
    case WavEncoding::ImaAdpcm:
        tag = 0x11;
        bits = 4;
        blockAlign = static_cast<std::uint16_t>(
            256 * options.channels * (options.sampleRate <= 11025 ? 1 : options.sampleRate <= 22050 ? 2 : 4));
        samplesPerBlock = static_cast<std::uint16_t>((blockAlign - 4 * options.channels) * 8 / (4 * options.channels) + 1);
        break;
    }
    if (blockAlign == 0) {
        blockAlign = static_cast<std::uint16_t>(options.channels * ((bits + 7) / 8));
    }
    const std::uint32_t byteRate =
        samplesPerBlock != 0 ? options.sampleRate * blockAlign / samplesPerBlock : options.sampleRate * blockAlign;

    std::vector<std::byte> fmt;
    putLe16(fmt, options.extensible ? 0xFFFE : tag);
    putLe16(fmt, options.channels);
    putLe32(fmt, options.sampleRate);
    putLe32(fmt, byteRate);
    putLe16(fmt, blockAlign);
    putLe16(fmt, bits);
    if (options.extensible) {
        putLe16(fmt, 22);
        putLe16(fmt, bits);
        putLe32(fmt, options.channels == 1 ? 0x4 : options.channels == 2 ? 0x3 : (1U << options.channels) - 1);
        putLe16(fmt, tag);
        for (const std::uint8_t byte : kSubtypeSuffix) {
            put8(fmt, byte);
        }
    } else if (samplesPerBlock != 0) {
        putLe16(fmt, 2);
        putLe16(fmt, samplesPerBlock);
    } else if (tag != 1) {
        putLe16(fmt, 0);
    }

    std::vector<std::byte> data;
    Random random(options.seed);
    if (options.encoding == WavEncoding::ImaAdpcm) {
        for (std::size_t block = 0; block < options.frames; ++block) {
            std::vector<std::byte> bytes = makePattern(blockAlign, options.seed + block);
            for (std::size_t channel = 0; channel < options.channels; ++channel) {
                bytes[channel * 4 + 2] = static_cast<std::byte>(random.below(89));  // step index
                bytes[channel * 4 + 3] = std::byte{0};
            }
            append(data, bytes);
        }
    } else {
        data = makePattern(options.frames * blockAlign, options.seed);
    }

    std::vector<std::byte> chunks;
    if (options.junk) {
        append(chunks, riffChunk("JUNK", std::vector<std::byte>(28)));
    }
    if (options.bext) {
        std::vector<std::byte> bext;
        putField(bext, "Test description", 256);
        putField(bext, "recovery tests", 32);
        putField(bext, "REF0001", 32);
        putField(bext, "2026-09-24", 10);
        putField(bext, "12:00:00", 8);
        putField(bext, "", 8);
        putLe16(bext, 1);
        putField(bext, "", 64 + 10 + 180);
        append(chunks, riffChunk("bext", bext));
    }
    append(chunks, riffChunk("fmt ", fmt));
    if (options.fact || options.encoding == WavEncoding::ImaAdpcm) {
        std::vector<std::byte> fact;
        putLe32(fact, samplesPerBlock != 0 ? options.frames * samplesPerBlock : options.frames);
        append(chunks, riffChunk("fact", fact));
    }
    append(chunks, riffChunk("data", data));
    if (options.listInfo) {
        std::vector<std::byte> list;
        putText(list, "INFO");
        std::vector<std::byte> name;
        putText(name, "Test title");
        put8(name, 0);
        append(list, riffChunk("INAM", name));
        std::vector<std::byte> software;
        putText(software, "recovery tests");
        put8(software, 0);
        append(list, riffChunk("ISFT", software));
        append(chunks, riffChunk("LIST", list));
    }
    if (options.cue) {
        std::vector<std::byte> cue;
        putLe32(cue, 1);
        putLe32(cue, 1);  // id
        putLe32(cue, 0);  // position
        putText(cue, "data");
        putLe32(cue, 0);
        putLe32(cue, 0);
        putLe32(cue, options.frames / 2);
        append(chunks, riffChunk("cue ", cue));
    }
    std::vector<std::byte> out;
    putText(out, "RIFF");
    putLe32(out, chunks.size() + 4);
    putText(out, "WAVE");
    append(out, chunks);
    return out;
}

std::vector<RiffChunk> riffChunks(std::span<const std::byte> file) {
    std::vector<RiffChunk> chunks;
    std::size_t position = 12;
    while (position + 8 <= file.size()) {
        std::string id;
        for (std::size_t i = 0; i < 4; ++i) {
            id += static_cast<char>(u8(file, position + i));
        }
        const std::uint32_t size = le32(file, position + 4);
        chunks.push_back(RiffChunk{id, position, size});
        position += 8 + size + (size & 1U);
    }
    return chunks;
}

}  // namespace recovery::test
