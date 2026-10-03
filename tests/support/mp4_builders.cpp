#include "support/mp4_builders.hpp"

#include "support/test_files.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace recovery::test {

namespace {

using Bytes = std::vector<std::byte>;

void put8(Bytes& out, std::uint64_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFF));
}

void putBe16(Bytes& out, std::uint64_t value) {
    put8(out, value >> 8);
    put8(out, value);
}

void putBe32(Bytes& out, std::uint64_t value) {
    putBe16(out, (value >> 16) & 0xFFFF);
    putBe16(out, value & 0xFFFF);
}

void putBe64(Bytes& out, std::uint64_t value) {
    putBe32(out, value >> 32);
    putBe32(out, value & 0xFFFFFFFF);
}

void putText(Bytes& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
}

void putZeros(Bytes& out, std::size_t count) {
    out.insert(out.end(), count, std::byte{0});
}

void append(Bytes& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

std::uint64_t be(std::span<const std::byte> bytes, std::size_t offset, std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value = (value << 8) | static_cast<std::uint8_t>(bytes[offset + i]);
    }
    return value;
}

std::uint64_t splitMix(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// A box to write: its payload, then its children.
struct Node {
    std::string type;
    Bytes payload;
    std::vector<Node> children;
    bool large = false;
    bool toEnd = false;
};

Node leaf(std::string type, Bytes payload) {
    Node node;
    node.type = std::move(type);
    node.payload = std::move(payload);
    return node;
}

Node full(std::string type, std::uint8_t version, std::uint32_t flags, std::span<const std::byte> payload) {
    Bytes body;
    putBe32(body, (std::uint32_t{version} << 24) | flags);
    append(body, payload);
    return leaf(std::move(type), std::move(body));
}

Node container(std::string type, std::vector<Node> children) {
    Node node;
    node.type = std::move(type);
    node.children = std::move(children);
    return node;
}

void serialize(const Node& node, Bytes& out) {
    Bytes body = node.payload;
    for (const Node& child : node.children) {
        serialize(child, body);
    }
    if (node.toEnd) {
        putBe32(out, 0);
        putText(out, node.type);
    } else if (node.large) {
        putBe32(out, 1);
        putText(out, node.type);
        putBe64(out, body.size() + 16);
    } else {
        putBe32(out, body.size() + 8);
        putText(out, node.type);
    }
    append(out, body);
}

Bytes serialized(const Node& node) {
    Bytes out;
    serialize(node, out);
    return out;
}

void makeLarge(Node& node) {
    for (Node& child : node.children) {
        child.large = true;
        makeLarge(child);
    }
}

// moov's first child (mvhd) goes last, keeping the tracks in order; the
// children of the other containers are reversed.
void reverseContainers(Node& node) {
    static const std::array<std::string_view, 4> kReversed = {"trak", "mdia", "minf", "stbl"};
    if (node.type == "moov" && !node.children.empty()) {
        std::rotate(node.children.begin(), node.children.begin() + 1, node.children.end());
    } else if (std::find(kReversed.begin(), kReversed.end(), node.type) != kReversed.end()) {
        std::reverse(node.children.begin(), node.children.end());
    }
    for (Node& child : node.children) {
        reverseContainers(child);
    }
}

void putMatrix(Bytes& out) {
    for (const std::uint32_t value : {0x00010000U, 0U, 0U, 0U, 0x00010000U, 0U, 0U, 0U, 0x40000000U}) {
        putBe32(out, value);
    }
}

std::uint32_t aacRateIndex(std::uint32_t rate) {
    static constexpr std::array<std::uint32_t, 13> kRates = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                             22050, 16000, 12000, 11025, 8000,  7350};
    const auto found = std::find(kRates.begin(), kRates.end(), rate);
    if (found == kRates.end()) {
        throw std::invalid_argument("MP4 builder: not an AAC sample rate");
    }
    return static_cast<std::uint32_t>(found - kRates.begin());
}

Bytes descriptor(std::uint8_t tag, std::span<const std::byte> payload) {
    Bytes out;
    put8(out, tag);
    put8(out, payload.size());  // every descriptor here is shorter than 128 bytes
    append(out, payload);
    return out;
}

Node esds(const Mp4TrackOptions& track) {
    // AudioSpecificConfig: AAC LC, the rate index, the channel configuration.
    const std::uint32_t config = (2U << 11) | (aacRateIndex(track.sampleRate) << 7) | (track.channels << 3U);
    Bytes specific;
    putBe16(specific, config);
    if (!track.codecConfig.empty()) {
        specific = track.codecConfig;
    }
    Bytes decoder;
    put8(decoder, 0x40);  // MPEG-4 audio
    put8(decoder, 0x15);  // audio stream
    put8(decoder, 0);     // bufferSizeDB (24 bits)
    putBe16(decoder, 0x0300);
    putBe32(decoder, 128000);
    putBe32(decoder, 96000);
    append(decoder, descriptor(0x05, specific));
    Bytes es;
    putBe16(es, 1);  // ES_ID
    put8(es, 0);
    append(es, descriptor(0x04, decoder));
    const Bytes sl = {std::byte{0x02}};
    append(es, descriptor(0x06, sl));
    return full("esds", 0, 0, descriptor(0x03, es));
}

Node sampleEntry(const Mp4TrackOptions& track) {
    Bytes entry;
    putZeros(entry, 6);
    putBe16(entry, 1);  // data reference index
    switch (track.kind) {
    case Mp4TrackKind::Video: {
        putZeros(entry, 16);
        putBe16(entry, track.width);
        putBe16(entry, track.height);
        putBe32(entry, 0x00480000);
        putBe32(entry, 0x00480000);
        putBe32(entry, 0);
        putBe16(entry, 1);  // frame count
        putZeros(entry, 32);
        putBe16(entry, 0x0018);
        putBe16(entry, 0xFFFF);
        if (track.hevc) {
            // Main profile, level 3.1, 4:2:0, 8 bits; the NAL unit length size; no parameter set arrays.
            Bytes config = {std::byte{1},    std::byte{0x01}, std::byte{0x60}, std::byte{0},    std::byte{0},
                            std::byte{0},    std::byte{0x90}, std::byte{0},    std::byte{0},    std::byte{0},
                            std::byte{0},    std::byte{0},    std::byte{93},   std::byte{0xF0}, std::byte{0},
                            std::byte{0xFC}, std::byte{0xFD}, std::byte{0xF8}, std::byte{0xF8}, std::byte{0},
                            std::byte{0}};
            put8(config, 0x0C | (track.nalLengthSize - 1));
            put8(config, 0);
            if (!track.codecConfig.empty()) {
                config = track.codecConfig;
            }
            Node node = leaf("hvc1", entry);
            node.children.push_back(leaf("hvcC", config));
            return node;
        }
        // Baseline profile, level 1.0, the NAL unit length size; an SPS for the coded size's macroblocks and a PPS.
        Bytes config = {std::byte{1}, std::byte{66}, std::byte{0xC0}, std::byte{10},
                        static_cast<std::byte>(0xFC | (track.nalLengthSize - 1)), std::byte{0xE1}};
        const Bytes sps = {std::byte{0x67}, std::byte{0x42}, std::byte{0xC0}, std::byte{0x0A},
                           std::byte{0xDA}, std::byte{0x11}, std::byte{0xE4}};
        const Bytes pps = {std::byte{0x68}, std::byte{0xCE}, std::byte{0x3C}, std::byte{0x80}};
        putBe16(config, sps.size());
        append(config, sps);
        put8(config, 1);
        putBe16(config, pps.size());
        append(config, pps);
        if (!track.codecConfig.empty()) {
            config = track.codecConfig;
        }
        Node node = leaf("avc1", entry);
        node.children.push_back(leaf("avcC", config));
        return node;
    }
    case Mp4TrackKind::Audio: {
        putZeros(entry, 8);
        putBe16(entry, track.channels);
        putBe16(entry, 16);
        putBe32(entry, 0);
        putBe32(entry, std::uint64_t{track.sampleRate} << 16);
        Node node = leaf("mp4a", entry);
        node.children.push_back(esds(track));
        return node;
    }
    case Mp4TrackKind::Text:
        break;
    }
    // 3GPP timed text (tx3g): display flags, justification, background,
    // default text box, default style, and a font table.
    putBe32(entry, 0);
    put8(entry, 1);
    put8(entry, 0xFF);
    putBe32(entry, 0);
    putZeros(entry, 8);
    putBe16(entry, 0);
    putBe16(entry, 0);
    putBe16(entry, 1);  // font id
    put8(entry, 0);
    put8(entry, 18);
    putBe32(entry, 0xFFFFFFFF);
    Bytes fonts;
    putBe16(fonts, 1);
    putBe16(fonts, 1);
    put8(fonts, 4);
    putText(fonts, "Sans");
    Node node = leaf("tx3g", entry);
    node.children.push_back(leaf("ftab", fonts));
    return node;
}

struct TrackPlan {
    Mp4TrackOptions options;
    std::uint32_t id = 0;
    std::vector<Bytes> samples;
    std::uint32_t timescale = 0;
    std::uint32_t delta = 0;
    // Absolute chunk offsets (0 until the layout is known).
    std::vector<std::uint64_t> chunkOffsets;

    [[nodiscard]] std::size_t chunkCount() const {
        return (samples.size() + options.samplesPerChunk - 1) / options.samplesPerChunk;
    }
    [[nodiscard]] std::size_t chunkBegin(std::size_t chunk) const { return chunk * options.samplesPerChunk; }
    [[nodiscard]] std::size_t chunkEnd(std::size_t chunk) const {
        return std::min(samples.size(), (chunk + 1) * options.samplesPerChunk);
    }
    [[nodiscard]] std::uint64_t chunkSize(std::size_t chunk) const {
        std::uint64_t size = 0;
        for (std::size_t i = chunkBegin(chunk); i < chunkEnd(chunk); ++i) {
            size += samples[i].size();
        }
        return size;
    }
};

// Turns a video sample's bytes into NAL units: each a big-endian length of
// `lengthSize` bytes, then a header byte with the forbidden zero bit clear
// and the rest of the unit. Some samples hold two units. A sample too small
// for one unit is left as it is.
void frameNalUnits(Bytes& sample, std::size_t lengthSize, std::size_t index) {
    const std::size_t largest = lengthSize == 1 ? 0xFF : lengthSize == 2 ? 0xFFFF : 0xFFFFFFFF;
    if (sample.size() < lengthSize + 1) {
        return;
    }
    std::size_t position = 0;
    bool first = true;
    while (position < sample.size()) {
        const std::size_t left = sample.size() - position;
        std::size_t length = std::min(left - lengthSize, largest);
        if (first && index % 3 == 1 && length > 64) {
            length /= 2;
        }
        // What is left must hold another unit, or nothing.
        const std::size_t rest = left - lengthSize - length;
        if (rest > 0 && rest < lengthSize + 1) {
            length -= lengthSize + 1 - rest;
        }
        for (std::size_t i = 0; i < lengthSize; ++i) {
            sample[position + i] = static_cast<std::byte>((length >> (8 * (lengthSize - 1 - i))) & 0xFF);
        }
        const std::uint8_t header = first ? (index % 5 == 0 ? 0x65 : 0x41) : 0x06;
        sample[position + lengthSize] = static_cast<std::byte>(header);
        position += lengthSize + length;
        first = false;
    }
}

TrackPlan planTrack(const Mp4TrackOptions& options, std::size_t index, std::uint64_t seed) {
    if (options.samplesPerChunk == 0) {
        throw std::invalid_argument("MP4 builder: samplesPerChunk must not be 0");
    }
    if (options.nalLengthSize != 1 && options.nalLengthSize != 2 && options.nalLengthSize != 4) {
        throw std::invalid_argument("MP4 builder: a NAL unit length size is 1, 2 or 4");
    }
    TrackPlan plan;
    plan.options = options;
    plan.id = options.trackId != 0 ? options.trackId : static_cast<std::uint32_t>(index + 1);
    std::uint64_t state = seed * 1000003 + index;
    if (!options.sampleData.empty()) {
        plan.samples = options.sampleData;
    }
    for (std::size_t i = 0; i < options.samples && options.sampleData.empty(); ++i) {
        const std::uint64_t random = splitMix(state);
        const std::uint64_t sampleSeed = (seed << 20) ^ (index << 16) ^ i;
        if (options.fixedSampleSize != 0) {
            plan.samples.push_back(makePattern(options.fixedSampleSize, sampleSeed));
            if (options.kind == Mp4TrackKind::Video) {
                frameNalUnits(plan.samples.back(), options.nalLengthSize, i);
            }
            continue;
        }
        switch (options.kind) {
        case Mp4TrackKind::Video:
            plan.samples.push_back(makePattern(options.uniformSize ? 500 : 100 + random % 900, sampleSeed));
            frameNalUnits(plan.samples.back(), options.nalLengthSize, i);
            break;
        case Mp4TrackKind::Audio:
            plan.samples.push_back(aacSilentFrame(options.channels, options.uniformSize ? 200 : 20 + random % 280));
            break;
        case Mp4TrackKind::Text: {
            const std::string text = options.uniformSize ? "Same" : "Line " + std::to_string(i + 1);
            Bytes sample;
            putBe16(sample, text.size());
            putText(sample, text);
            plan.samples.push_back(std::move(sample));
            break;
        }
        }
    }
    if (options.uniformSize) {
        for (const Bytes& sample : plan.samples) {
            if (sample.size() != plan.samples.front().size()) {
                throw std::invalid_argument("MP4 builder: a uniform size needs samples of one size");
            }
        }
    }
    switch (options.kind) {
    case Mp4TrackKind::Video:
        plan.timescale = 30000;
        plan.delta = 1001;
        break;
    case Mp4TrackKind::Audio:
        plan.timescale = options.sampleRate;
        plan.delta = 1024;
        break;
    case Mp4TrackKind::Text:
        plan.timescale = 1000;
        plan.delta = 500;
        break;
    }
    return plan;
}

Node sampleTable(const TrackPlan& plan, bool co64) {
    const Mp4TrackOptions& options = plan.options;
    std::vector<Node> children;
    Bytes stsd;
    putBe32(stsd, 1);
    append(stsd, serialized(sampleEntry(options)));
    children.push_back(full("stsd", 0, 0, stsd));

    Bytes stts;
    if (plan.samples.empty()) {
        putBe32(stts, 0);
    } else {
        putBe32(stts, 1);
        putBe32(stts, plan.samples.size());
        putBe32(stts, plan.delta);
    }
    children.push_back(full("stts", 0, 0, stts));

    Bytes stsc;
    const std::size_t chunks = plan.chunkCount();
    if (chunks == 0) {
        putBe32(stsc, 0);
    } else {
        const std::size_t last = plan.chunkEnd(chunks - 1) - plan.chunkBegin(chunks - 1);
        const bool shortLast = chunks > 1 && last != options.samplesPerChunk;
        putBe32(stsc, shortLast ? 2 : 1);
        putBe32(stsc, 1);
        putBe32(stsc, chunks > 1 ? options.samplesPerChunk : last);
        putBe32(stsc, 1);
        if (shortLast) {
            putBe32(stsc, chunks);
            putBe32(stsc, last);
            putBe32(stsc, 1);
        }
    }
    children.push_back(full("stsc", 0, 0, stsc));

    std::size_t largest = 0;
    for (const Bytes& sample : plan.samples) {
        largest = std::max(largest, sample.size());
    }
    if (options.compactSizes) {
        const std::uint8_t field = largest <= 15 ? 4 : largest <= 255 ? 8 : 16;
        Bytes stz2;
        putZeros(stz2, 3);
        put8(stz2, field);
        putBe32(stz2, plan.samples.size());
        for (std::size_t i = 0; i < plan.samples.size(); ++i) {
            const std::size_t size = plan.samples[i].size();
            if (field == 16) {
                putBe16(stz2, size);
            } else if (field == 8) {
                put8(stz2, size);
            } else if (i % 2 == 0) {
                put8(stz2, size << 4);
            } else {
                stz2.back() |= static_cast<std::byte>(size);
            }
        }
        children.push_back(full("stz2", 0, 0, stz2));
    } else {
        Bytes stsz;
        putBe32(stsz, options.uniformSize && !plan.samples.empty() ? plan.samples.front().size() : 0);
        putBe32(stsz, plan.samples.size());
        if (!options.uniformSize) {
            for (const Bytes& sample : plan.samples) {
                putBe32(stsz, sample.size());
            }
        }
        children.push_back(full("stsz", 0, 0, stsz));
    }

    Bytes offsets;
    putBe32(offsets, plan.chunkOffsets.size());
    for (const std::uint64_t offset : plan.chunkOffsets) {
        if (co64) {
            putBe64(offsets, offset);
        } else {
            putBe32(offsets, offset);
        }
    }
    children.push_back(full(co64 ? "co64" : "stco", 0, 0, offsets));
    return container("stbl", std::move(children));
}

Node trackBox(const TrackPlan& plan, const Mp4Options& options) {
    const Mp4TrackOptions& track = plan.options;
    const std::uint64_t mediaDuration = std::uint64_t{plan.delta} * plan.samples.size();
    const std::uint64_t movieDuration = mediaDuration * 1000 / plan.timescale;
    const std::uint8_t version = track.version1 ? 1 : 0;

    Bytes tkhd;
    const auto time = [&](Bytes& out, std::uint64_t value) {
        if (version == 1) {
            putBe64(out, value);
        } else {
            putBe32(out, value);
        }
    };
    time(tkhd, 0);
    time(tkhd, 0);
    putBe32(tkhd, plan.id);
    putBe32(tkhd, 0);
    time(tkhd, movieDuration);
    putZeros(tkhd, 8);
    putBe16(tkhd, 0);
    putBe16(tkhd, 0);
    putBe16(tkhd, track.kind == Mp4TrackKind::Audio ? 0x0100 : 0);
    putBe16(tkhd, 0);
    putMatrix(tkhd);
    putBe32(tkhd, track.kind == Mp4TrackKind::Video ? std::uint64_t{track.width} << 16 : 0);
    putBe32(tkhd, track.kind == Mp4TrackKind::Video ? std::uint64_t{track.height} << 16 : 0);

    Bytes mdhd;
    time(mdhd, 0);
    time(mdhd, 0);
    putBe32(mdhd, plan.timescale);
    time(mdhd, mediaDuration);
    putBe16(mdhd, 0x55C4);  // "und"
    putBe16(mdhd, 0);

    std::string_view handlerType = "vide";
    std::string_view handlerName = "VideoHandler";
    Node mediaHeader;
    if (track.kind == Mp4TrackKind::Video) {
        mediaHeader = full("vmhd", 0, 1, Bytes(8));
    } else if (track.kind == Mp4TrackKind::Audio) {
        handlerType = "soun";
        handlerName = "SoundHandler";
        mediaHeader = full("smhd", 0, 0, Bytes(4));
    } else {
        handlerType = "text";
        handlerName = "TextHandler";
        mediaHeader = full("nmhd", 0, 0, {});
    }
    Bytes hdlr;
    putBe32(hdlr, 0);
    putText(hdlr, handlerType);
    putZeros(hdlr, 12);
    putText(hdlr, handlerName);
    put8(hdlr, 0);

    Bytes dref;
    putBe32(dref, 1);
    append(dref, serialized(full("url ", 0, 1, {})));
    Node minf = container("minf", {mediaHeader, container("dinf", {full("dref", 0, 0, dref)}),
                                   sampleTable(plan, options.co64)});
    Node mdia = container("mdia", {full("mdhd", version, 0, mdhd), full("hdlr", 0, 0, hdlr), std::move(minf)});
    std::vector<Node> children = {full("tkhd", version, 3, tkhd)};
    if (options.extraBoxes) {
        Bytes elst;
        putBe32(elst, 1);
        putBe32(elst, movieDuration);
        putBe32(elst, 0);
        putBe32(elst, 0x00010000);
        children.push_back(container("edts", {full("elst", 0, 0, elst)}));
    }
    children.push_back(std::move(mdia));
    return container("trak", std::move(children));
}

Node movieBox(const std::vector<TrackPlan>& tracks, const Mp4Options& options) {
    std::uint64_t duration = 0;
    std::uint32_t nextId = 1;
    for (const TrackPlan& plan : tracks) {
        duration = std::max(duration, std::uint64_t{plan.delta} * plan.samples.size() * 1000 / plan.timescale);
        nextId = std::max(nextId, plan.id + 1);
    }
    const bool wide = options.movieVersion1;
    Bytes mvhd;
    if (wide) {
        putBe64(mvhd, 0);
        putBe64(mvhd, 0);
        putBe32(mvhd, 1000);
        putBe64(mvhd, duration);
    } else {
        putBe32(mvhd, 0);
        putBe32(mvhd, 0);
        putBe32(mvhd, 1000);
        putBe32(mvhd, duration);
    }
    putBe32(mvhd, 0x00010000);
    putBe16(mvhd, 0x0100);
    putZeros(mvhd, 10);
    putMatrix(mvhd);
    putZeros(mvhd, 24);
    putBe32(mvhd, nextId);
    std::vector<Node> children = {full("mvhd", wide ? 1 : 0, 0, mvhd)};
    for (const TrackPlan& plan : tracks) {
        children.push_back(trackBox(plan, options));
    }
    if (options.extraBoxes) {
        Bytes name;
        putText(name, "Test movie");
        children.push_back(container("udta", {leaf("name", name)}));
    }
    Node moov = container("moov", std::move(children));
    moov.large = options.largeMoov;
    if (options.largeNested) {
        makeLarge(moov);
    }
    if (options.reverseChildren) {
        reverseContainers(moov);
    }
    return moov;
}

Bytes fileTypeBox(const Mp4Options& options) {
    Bytes ftyp;
    putText(ftyp, options.majorBrand);
    putBe32(ftyp, 0x200);
    for (const std::string& brand : options.compatibleBrands) {
        putText(ftyp, brand);
    }
    return serialized(leaf("ftyp", ftyp));
}

// A movie fragment: mfhd, then a traf per track with samples in it. `runs`
// gives, per track, the fragment's samples (first index into the track's
// samples, count) and where its run starts; `moof` is where the box starts.
struct FragmentRun {
    std::size_t track = 0;
    std::size_t first = 0;
    std::size_t count = 0;
    std::uint64_t offset = 0;
};

Node fragmentBox(const std::vector<TrackPlan>& tracks, const std::vector<FragmentRun>& runs, std::uint32_t sequence,
                 std::uint64_t moof, Mp4FragmentBase base) {
    Bytes mfhd;
    putBe32(mfhd, sequence);
    std::vector<Node> children = {full("mfhd", 0, 0, mfhd)};
    for (std::size_t r = 0; r < runs.size(); ++r) {
        const FragmentRun& run = runs[r];
        const TrackPlan& plan = tracks[run.track];
        const Mp4TrackOptions& track = plan.options;
        const bool audio = track.kind == Mp4TrackKind::Audio;
        const bool uniform = track.uniformSize;

        std::uint32_t tfhdFlags = (audio ? 0x08 : 0) | (uniform ? 0x10 : 0);
        if (base == Mp4FragmentBase::Moof) {
            tfhdFlags |= 0x020000;
        } else if (base == Mp4FragmentBase::Explicit) {
            tfhdFlags |= 0x01;
        }
        Bytes tfhd;
        putBe32(tfhd, plan.id);
        if (base == Mp4FragmentBase::Explicit) {
            putBe64(tfhd, run.offset);
        }
        if (audio) {
            putBe32(tfhd, plan.delta);
        }
        if (uniform) {
            putBe32(tfhd, plan.samples[run.first].size());
        }
        Bytes tfdt;
        putBe64(tfdt, std::uint64_t{plan.delta} * run.first);

        // Video: durations, sizes and first-sample flags; audio: sizes (the
        // duration is tfhd's); text: sizes and composition offsets.
        const bool dataOffset = base == Mp4FragmentBase::Moof || (base == Mp4FragmentBase::Implicit && r == 0);
        std::uint32_t trunFlags = (dataOffset ? 0x001 : 0) | (uniform ? 0 : 0x200);
        if (track.kind == Mp4TrackKind::Video) {
            trunFlags |= 0x004 | 0x100;
        } else if (track.kind == Mp4TrackKind::Text) {
            trunFlags |= 0x800;
        }
        Bytes trun;
        putBe32(trun, run.count);
        if (dataOffset) {
            putBe32(trun, run.offset - moof);
        }
        if ((trunFlags & 0x004) != 0) {
            putBe32(trun, 0x02000000);  // the first sample is a sync sample
        }
        for (std::size_t i = run.first; i < run.first + run.count; ++i) {
            if ((trunFlags & 0x100) != 0) {
                putBe32(trun, plan.delta);
            }
            if ((trunFlags & 0x200) != 0) {
                putBe32(trun, plan.samples[i].size());
            }
            if ((trunFlags & 0x800) != 0) {
                putBe32(trun, 0);
            }
        }
        children.push_back(container("traf", {full("tfhd", 0, tfhdFlags, tfhd), full("tfdt", 1, 0, tfdt),
                                              full("trun", 0, trunFlags, trun)}));
    }
    return container("moof", std::move(children));
}

// ftyp, moov with mvex (and the first samples, in an mdat after it), then
// moof and mdat pairs.
Mp4Built makeFragmentedMp4(const Mp4Options& options) {
    if (options.moov != Mp4MoovPlace::First || options.mediaDataBoxes != 1 || options.mediaDataToEnd ||
        options.largeMediaData) {
        throw std::invalid_argument("MP4 builder: movie fragments need moov first and one plain mdat");
    }
    std::vector<TrackPlan> tracks;
    std::vector<TrackPlan> inMoov;
    for (std::size_t t = 0; t < options.tracks.size(); ++t) {
        tracks.push_back(planTrack(options.tracks[t], t, options.seed));
        TrackPlan head = tracks.back();
        head.samples.resize(std::min(head.samples.size(), options.samplesInMoov));
        inMoov.push_back(std::move(head));
    }
    const auto movie = [&]() {
        Node moov = movieBox(inMoov, options);
        std::vector<Node> extends;
        for (const TrackPlan& plan : tracks) {
            Bytes trex;
            putBe32(trex, plan.id);
            putBe32(trex, 1);
            putBe32(trex, plan.delta);
            putBe32(trex, 0);
            putBe32(trex, 0);
            extends.push_back(full("trex", 0, 0, trex));
        }
        moov.children.push_back(container("mvex", std::move(extends)));
        return moov;
    };

    Mp4Built built;
    built.samples.resize(tracks.size());
    built.chunks.resize(tracks.size());
    built.runs.resize(tracks.size());
    if (!options.majorBrand.empty()) {
        append(built.bytes, fileTypeBox(options));
    }
    // moov's own samples: in one mdat after it, chunks of the tracks in turn.
    for (TrackPlan& plan : inMoov) {
        plan.chunkOffsets.assign(plan.chunkCount(), 0);
    }
    const std::uint64_t moovSize = serialized(movie()).size();
    std::uint64_t position = built.bytes.size() + moovSize + 8;
    Bytes media;
    std::size_t mostChunks = 0;
    for (const TrackPlan& plan : inMoov) {
        mostChunks = std::max(mostChunks, plan.chunkCount());
    }
    for (std::size_t c = 0; c < mostChunks; ++c) {
        for (std::size_t t = 0; t < inMoov.size(); ++t) {
            TrackPlan& plan = inMoov[t];
            if (c >= plan.chunkCount()) {
                continue;
            }
            plan.chunkOffsets[c] = position;
            built.chunks[t].push_back(Mp4ChunkTruth{position, plan.chunkSize(c),
                                                    static_cast<std::uint32_t>(plan.chunkBegin(c)),
                                                    static_cast<std::uint32_t>(plan.chunkEnd(c) - plan.chunkBegin(c))});
            for (std::size_t s = plan.chunkBegin(c); s < plan.chunkEnd(c); ++s) {
                append(media, plan.samples[s]);
                position += plan.samples[s].size();
            }
        }
    }
    const Bytes moov = serialized(movie());
    if (moov.size() != moovSize) {
        throw std::logic_error("MP4 builder: moov changed size");
    }
    append(built.bytes, moov);
    if (!media.empty()) {
        putBe32(built.bytes, media.size() + 8);
        putText(built.bytes, "mdat");
        append(built.bytes, media);
    }
    // The truth for moov's samples, in chunk order within each track.
    for (std::size_t t = 0; t < inMoov.size(); ++t) {
        for (const Mp4ChunkTruth& chunk : built.chunks[t]) {
            std::uint64_t offset = chunk.offset;
            for (std::size_t s = chunk.firstSample; s < chunk.firstSample + chunk.sampleCount; ++s) {
                built.samples[t].push_back(Mp4Sample{offset, static_cast<std::uint32_t>(inMoov[t].samples[s].size())});
                offset += inMoov[t].samples[s].size();
            }
        }
    }

    // The fragments.
    std::vector<std::size_t> next(tracks.size(), options.samplesInMoov);
    for (std::uint32_t sequence = 1;; ++sequence) {
        std::vector<FragmentRun> runs;
        for (std::size_t t = 0; t < tracks.size(); ++t) {
            const std::size_t left = tracks[t].samples.size() - std::min(next[t], tracks[t].samples.size());
            if (left > 0) {
                runs.push_back(FragmentRun{t, next[t], std::min(left, options.fragmentSamples), 0});
            }
        }
        if (runs.empty()) {
            break;
        }
        const std::uint64_t moof = built.bytes.size();
        const std::uint64_t moofSize =
            serialized(fragmentBox(tracks, runs, sequence, moof, options.fragmentBase)).size();
        std::uint64_t at = moof + moofSize + 8;
        Bytes payload;
        for (FragmentRun& run : runs) {
            run.offset = at;
            const TrackPlan& plan = tracks[run.track];
            std::uint64_t size = 0;
            for (std::size_t i = run.first; i < run.first + run.count; ++i) {
                const auto sampleSize = static_cast<std::uint32_t>(plan.samples[i].size());
                built.samples[run.track].push_back(Mp4Sample{at + size, sampleSize});
                size += plan.samples[i].size();
                append(payload, plan.samples[i]);
            }
            built.runs[run.track].push_back(
                Mp4ChunkTruth{at, size, static_cast<std::uint32_t>(run.first - options.samplesInMoov),
                              static_cast<std::uint32_t>(run.count)});
            at += size;
            next[run.track] += run.count;
        }
        const Bytes box = serialized(fragmentBox(tracks, runs, sequence, moof, options.fragmentBase));
        if (box.size() != moofSize) {
            throw std::logic_error("MP4 builder: moof changed size");
        }
        append(built.bytes, box);
        putBe32(built.bytes, payload.size() + 8);
        putText(built.bytes, "mdat");
        append(built.bytes, payload);
    }
    return built;
}

}  // namespace

std::vector<std::byte> makeBox(std::string_view type, std::span<const std::byte> payload, bool large) {
    Node node = leaf(std::string(type), Bytes(payload.begin(), payload.end()));
    node.large = large;
    return serialized(node);
}

std::vector<std::byte> makeFullBox(std::string_view type, std::uint8_t version, std::uint32_t flags,
                                   std::span<const std::byte> payload) {
    return serialized(full(std::string(type), version, flags, payload));
}

Mp4Built makeMp4(const Mp4Options& options) {
    if (options.fragmentSamples != 0) {
        return makeFragmentedMp4(options);
    }
    if (options.mediaDataBoxes == 0) {
        throw std::invalid_argument("MP4 builder: at least one mdat box");
    }
    if (options.moov == Mp4MoovPlace::Between && options.mediaDataBoxes < 2) {
        throw std::invalid_argument("MP4 builder: moov between mdat boxes needs two of them");
    }
    if (options.mediaDataToEnd && options.moov == Mp4MoovPlace::Last) {
        throw std::invalid_argument("MP4 builder: an mdat to the end of the file must be the last box");
    }
    std::vector<TrackPlan> tracks;
    for (std::size_t t = 0; t < options.tracks.size(); ++t) {
        tracks.push_back(planTrack(options.tracks[t], t, options.seed));
    }

    // The order of the chunks in the media data, and how they are spread over the mdat boxes.
    struct ChunkRef {
        std::size_t track;
        std::size_t chunk;
    };
    std::vector<ChunkRef> order;
    std::size_t mostChunks = 0;
    for (const TrackPlan& plan : tracks) {
        mostChunks = std::max(mostChunks, plan.chunkCount());
    }
    if (options.interleave) {
        for (std::size_t c = 0; c < mostChunks; ++c) {
            for (std::size_t t = 0; t < tracks.size(); ++t) {
                if (c < tracks[t].chunkCount()) {
                    order.push_back(ChunkRef{t, c});
                }
            }
        }
    } else {
        for (std::size_t t = 0; t < tracks.size(); ++t) {
            for (std::size_t c = 0; c < tracks[t].chunkCount(); ++c) {
                order.push_back(ChunkRef{t, c});
            }
        }
    }
    const std::size_t groups = options.mediaDataBoxes;
    const auto groupBegin = [&](std::size_t g) { return g * order.size() / groups; };

    // The top-level boxes in order: fixed boxes, mdat groups and moov.
    enum class Kind : std::uint8_t { Fixed, MediaData, Movie };
    struct Item {
        Kind kind;
        Bytes bytes;        // Fixed
        std::size_t group;  // MediaData
    };
    std::vector<Item> items;
    if (!options.majorBrand.empty()) {
        Bytes ftyp;
        putText(ftyp, options.majorBrand);
        putBe32(ftyp, 0x200);
        for (const std::string& brand : options.compatibleBrands) {
            putText(ftyp, brand);
        }
        items.push_back(Item{Kind::Fixed, makeBox("ftyp", ftyp), 0});
    }
    if (options.uuidBox) {
        Bytes uuid(16);
        for (std::size_t i = 0; i < uuid.size(); ++i) {
            uuid[i] = static_cast<std::byte>(0xA0 + i);
        }
        putText(uuid, "extension");
        items.push_back(Item{Kind::Fixed, makeBox("uuid", uuid), 0});
    }
    if (options.freeBox) {
        items.push_back(Item{Kind::Fixed, makeBox("free", Bytes(16)), 0});
    }
    if (options.moov == Mp4MoovPlace::First) {
        items.push_back(Item{Kind::Movie, {}, 0});
    }
    for (std::size_t g = 0; g < groups; ++g) {
        if (g > 0 && !(options.moov == Mp4MoovPlace::Between && g == 1)) {
            items.push_back(Item{Kind::Fixed, makeBox("free", Bytes(8)), 0});
        }
        if (options.wideBox) {
            items.push_back(Item{Kind::Fixed, makeBox("wide", {}), 0});
        }
        items.push_back(Item{Kind::MediaData, {}, g});
        if (options.moov == Mp4MoovPlace::Between && g == 0) {
            items.push_back(Item{Kind::Movie, {}, 0});
        }
    }
    if (options.moov == Mp4MoovPlace::Last) {
        items.push_back(Item{Kind::Movie, {}, 0});
    }

    // Pass 1: sizes (moov's does not depend on the offset values), then where each chunk goes.
    for (TrackPlan& plan : tracks) {
        plan.chunkOffsets.assign(plan.chunkCount(), 0);
    }
    const std::size_t moovSize = serialized(movieBox(tracks, options)).size();
    const std::size_t mdatHeader = options.largeMediaData ? 16 : 8;
    std::uint64_t position = 0;
    for (const Item& item : items) {
        switch (item.kind) {
        case Kind::Fixed:
            position += item.bytes.size();
            break;
        case Kind::Movie:
            position += moovSize;
            break;
        case Kind::MediaData:
            position += mdatHeader;
            for (std::size_t i = groupBegin(item.group); i < groupBegin(item.group + 1); ++i) {
                TrackPlan& plan = tracks[order[i].track];
                plan.chunkOffsets[order[i].chunk] = position;
                position += plan.chunkSize(order[i].chunk);
            }
            break;
        }
    }

    // Pass 2: the file.
    const Node moov = movieBox(tracks, options);
    Mp4Built built;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        switch (item.kind) {
        case Kind::Fixed:
            append(built.bytes, item.bytes);
            break;
        case Kind::Movie: {
            const Bytes bytes = serialized(moov);
            if (bytes.size() != moovSize) {
                throw std::logic_error("MP4 builder: moov changed size");
            }
            append(built.bytes, bytes);
            break;
        }
        case Kind::MediaData: {
            Bytes payload;
            for (std::size_t c = groupBegin(item.group); c < groupBegin(item.group + 1); ++c) {
                const TrackPlan& plan = tracks[order[c].track];
                for (std::size_t s = plan.chunkBegin(order[c].chunk); s < plan.chunkEnd(order[c].chunk); ++s) {
                    append(payload, plan.samples[s]);
                }
            }
            const bool last = i + 1 == items.size();
            if (options.mediaDataToEnd && last) {
                putBe32(built.bytes, 0);
                putText(built.bytes, "mdat");
                if (options.largeMediaData) {
                    throw std::invalid_argument("MP4 builder: an mdat to the end of the file has no 64-bit size");
                }
            } else if (options.largeMediaData) {
                putBe32(built.bytes, 1);
                putText(built.bytes, "mdat");
                putBe64(built.bytes, payload.size() + 16);
            } else {
                putBe32(built.bytes, payload.size() + 8);
                putText(built.bytes, "mdat");
            }
            append(built.bytes, payload);
            break;
        }
        }
    }
    if (options.mediaDataToEnd && items.back().kind != Kind::MediaData) {
        throw std::invalid_argument("MP4 builder: an mdat to the end of the file must be the last box");
    }

    for (const TrackPlan& plan : tracks) {
        std::vector<Mp4Sample> samples;
        std::vector<Mp4ChunkTruth> chunks;
        for (std::size_t c = 0; c < plan.chunkCount(); ++c) {
            std::uint64_t offset = plan.chunkOffsets[c];
            chunks.push_back(Mp4ChunkTruth{offset, plan.chunkSize(c), static_cast<std::uint32_t>(plan.chunkBegin(c)),
                                           static_cast<std::uint32_t>(plan.chunkEnd(c) - plan.chunkBegin(c))});
            for (std::size_t s = plan.chunkBegin(c); s < plan.chunkEnd(c); ++s) {
                samples.push_back(Mp4Sample{offset, static_cast<std::uint32_t>(plan.samples[s].size())});
                offset += plan.samples[s].size();
            }
        }
        built.samples.push_back(std::move(samples));
        built.chunks.push_back(std::move(chunks));
        built.runs.emplace_back();
    }
    return built;
}

SpreadMp4 spreadMp4(const Mp4Built& built, std::uint64_t gap) {
    const std::vector<BoxPosition> boxes = mp4Boxes(built.bytes);
    const BoxPosition mdat = findBox(boxes, "mdat");
    if (mdat.offset + mdat.size != built.bytes.size() || be(built.bytes, mdat.offset, 4) != 1) {
        throw std::invalid_argument("MP4 builder: spreading needs moov first and one mdat with a 64-bit size");
    }
    SpreadMp4 spread;
    spread.gap = gap;
    const std::size_t payload = mdat.offset + 16;
    spread.head.assign(built.bytes.begin(), built.bytes.begin() + static_cast<std::ptrdiff_t>(payload));
    spread.media.assign(built.bytes.begin() + static_cast<std::ptrdiff_t>(payload), built.bytes.end());
    const auto setBe64 = [&](std::size_t offset, std::uint64_t value) {
        for (std::size_t i = 0; i < 8; ++i) {
            spread.head[offset + i] = static_cast<std::byte>((value >> (56 - 8 * i)) & 0xFF);
        }
    };
    setBe64(mdat.offset + 8, mdat.size + gap);
    std::size_t tables = 0;
    for (const BoxPosition& box : boxes) {
        if (box.path == "moov/trak/mdia/minf/stbl/co64") {
            const std::uint64_t count = be(spread.head, box.offset + 12, 4);
            for (std::uint64_t i = 0; i < count; ++i) {
                const std::size_t at = box.offset + 16 + static_cast<std::size_t>(i) * 8;
                setBe64(at, be(spread.head, at, 8) + gap);
            }
            ++tables;
        }
    }
    if (tables != built.samples.size()) {
        throw std::invalid_argument("MP4 builder: spreading needs co64 in every track");
    }
    for (const std::vector<Mp4Sample>& track : built.samples) {
        std::vector<Mp4Sample> moved;
        for (const Mp4Sample& sample : track) {
            moved.push_back(Mp4Sample{sample.offset + gap, sample.size});
        }
        spread.samples.push_back(std::move(moved));
    }
    return spread;
}

std::vector<BoxPosition> mp4Boxes(std::span<const std::byte> file) {
    static const std::array<std::string_view, 11> kContainers = {"moov", "trak", "edts", "mdia", "minf", "dinf",
                                                                 "stbl", "udta", "mvex", "moof", "traf"};
    std::vector<BoxPosition> boxes;
    const auto walk = [&](const auto& self, std::size_t from, std::size_t to, const std::string& parent) -> void {
        std::size_t position = from;
        while (to - position >= 8) {
            std::uint64_t size = be(file, position, 4);
            std::size_t header = 8;
            if (size == 1) {
                if (to - position < 16) {
                    return;
                }
                size = be(file, position + 8, 8);
                header = 16;
            } else if (size == 0) {
                size = to - position;
            }
            if (size < header || size > to - position) {
                return;
            }
            std::string type;
            for (std::size_t i = 4; i < 8; ++i) {
                type += static_cast<char>(file[position + i]);
            }
            const std::string path = parent.empty() ? type : parent + "/" + type;
            const auto end = position + static_cast<std::size_t>(size);
            boxes.push_back(BoxPosition{path, position, static_cast<std::size_t>(size)});
            if (std::find(kContainers.begin(), kContainers.end(), type) != kContainers.end()) {
                self(self, position + header, end, path);
            } else if (type == "stsd" && size >= header + 8) {
                self(self, position + header + 8, end, path);
            }
            position = end;
        }
    };
    walk(walk, 0, file.size(), "");
    return boxes;
}

BoxPosition findBox(const std::vector<BoxPosition>& boxes, std::string_view path, std::size_t index) {
    for (const BoxPosition& box : boxes) {
        if (box.path == path && index-- == 0) {
            return box;
        }
    }
    throw std::invalid_argument("no box " + std::string(path));
}

}  // namespace recovery::test
