// The MP4 parser (P11) on files from the MP4 builder: a normal MP4; every box
// order (moov first, last, between mdat boxes; media data in several mdat
// boxes; children in reverse order; no ftyp); 64-bit sizes at every level
// and offsets beyond 4 GiB; corrupt box sizes at the top level and inside
// moov; damaged, missing and repeated boxes and tables; multiple tracks;
// audio-only and video-only files; truncation at every length; files
// scattered over a source; limits; failing reads; fuzzing.

#include "formats/mp4_parser.hpp"

#include "format_test_helpers.hpp"
#include "mp4_test_helpers.hpp"
#include "support/mp4_builders.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <string>

namespace recovery::formats {
namespace {

using mp4::ChunkPlacement;
using mp4::FileStatus;
using mp4::IssueKind;
using mp4::Mp4File;
using mp4::TrackKind;
using test::Mp4Built;
using test::Mp4MoovPlace;
using test::Mp4Options;
using test::Mp4TrackKind;
using test::Mp4TrackOptions;
using testing::Bytes;
using testing::describe;
using testing::hasIssue;
using testing::matchesBuilt;
using testing::overwritten;
using testing::parseBytes;

Mp4TrackOptions video(std::size_t samples = 12, std::size_t perChunk = 4) {
    Mp4TrackOptions track;
    track.kind = Mp4TrackKind::Video;
    track.samples = samples;
    track.samplesPerChunk = perChunk;
    return track;
}

Mp4TrackOptions audio(std::size_t samples = 20, std::size_t perChunk = 6) {
    Mp4TrackOptions track;
    track.kind = Mp4TrackKind::Audio;
    track.samples = samples;
    track.samplesPerChunk = perChunk;
    return track;
}

Mp4TrackOptions text(std::size_t samples = 4) {
    Mp4TrackOptions track;
    track.kind = Mp4TrackKind::Text;
    track.samples = samples;
    track.samplesPerChunk = 1;
    return track;
}

Mp4Options withTracks(std::vector<Mp4TrackOptions> tracks) {
    Mp4Options options;
    options.tracks = std::move(tracks);
    return options;
}

std::uint32_t be32At(const Bytes& file, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<std::uint8_t>(file[offset + i]);
    }
    return value;
}

Bytes be32(std::uint64_t value) {
    return Bytes{static_cast<std::byte>((value >> 24) & 0xFF), static_cast<std::byte>((value >> 16) & 0xFF),
                 static_cast<std::byte>((value >> 8) & 0xFF), static_cast<std::byte>(value & 0xFF)};
}

Bytes be64(std::uint64_t value) {
    return testing::concat({be32(value >> 32), be32(value & 0xFFFFFFFF)});
}

Bytes withBe32(Bytes file, std::size_t offset, std::uint64_t value) {
    return overwritten(std::move(file), offset, be32(value));
}

// Replaces [at, at + erase) with `insert`, and corrects the 32-bit size of
// the box at `container` and of every box around it.
Bytes edited(const Bytes& file, std::size_t container, std::size_t at, std::size_t erase,
             std::span<const std::byte> insert = {}) {
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(file);
    const auto found = std::find_if(boxes.begin(), boxes.end(),
                                    [&](const test::BoxPosition& b) { return b.offset == container; });
    if (found == boxes.end()) {
        throw std::invalid_argument("no box at " + std::to_string(container));
    }
    const test::BoxPosition& inner = *found;
    Bytes out = testing::inserted(testing::erased(file, at, erase), at, insert);
    const auto delta = static_cast<std::int64_t>(insert.size()) - static_cast<std::int64_t>(erase);
    for (const test::BoxPosition& b : boxes) {
        if (b.offset <= inner.offset && inner.offset + inner.size <= b.offset + b.size) {
            const auto size = static_cast<std::uint64_t>(static_cast<std::int64_t>(b.size) + delta);
            out = withBe32(std::move(out), b.offset, size);
        }
    }
    return out;
}

// The innermost box around `box`.
test::BoxPosition parentOf(const std::vector<test::BoxPosition>& boxes, const test::BoxPosition& box) {
    const test::BoxPosition* parent = nullptr;
    for (const test::BoxPosition& candidate : boxes) {
        if (candidate.offset < box.offset && box.offset + box.size <= candidate.offset + candidate.size &&
            (parent == nullptr || candidate.offset > parent->offset)) {
            parent = &candidate;
        }
    }
    if (parent == nullptr) {
        throw std::invalid_argument(box.path + " is a top-level box");
    }
    return *parent;
}

Bytes removedBox(const Bytes& file, std::string_view path, std::size_t index = 0) {
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(file);
    const test::BoxPosition box = test::findBox(boxes, path, index);
    return edited(file, parentOf(boxes, box).offset, box.offset, box.size);
}

Bytes duplicatedBox(const Bytes& file, std::string_view path) {
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(file);
    const test::BoxPosition box = test::findBox(boxes, path);
    const Bytes copy(file.begin() + static_cast<std::ptrdiff_t>(box.offset),
                     file.begin() + static_cast<std::ptrdiff_t>(box.offset + box.size));
    return edited(file, parentOf(boxes, box).offset, box.offset + box.size, 0, copy);
}

// The offset of a table box's first entry (32-bit box header, version and flags, count).
std::size_t firstEntry(const Bytes& file, std::string_view path, std::size_t index = 0) {
    return test::findBox(test::mp4Boxes(file), path, index).offset + 16;
}

constexpr std::string_view kStbl = "moov/trak/mdia/minf/stbl";

std::string stbl(std::string_view child) {
    return std::string(kStbl) + "/" + std::string(child);
}

void expectInvalidWith(const Bytes& file, std::string_view issue) {
    const Mp4File parsed = parseBytes(file);
    EXPECT_EQ(parsed.status, FileStatus::Invalid) << describe(parsed);
    EXPECT_TRUE(hasIssue(parsed, issue)) << "expected \"" << issue << "\"\n" << describe(parsed);
}

// ---------------------------------------------------------------------------
// A normal MP4
// ---------------------------------------------------------------------------

TEST(Mp4ParserTest, NormalMp4WithVideoAndAudio) {
    const Mp4Built built = test::makeMp4();
    const Mp4File file = parseBytes(built.bytes);
    ASSERT_TRUE(matchesBuilt(file, built));

    ASSERT_TRUE(file.fileType.has_value());
    EXPECT_EQ(file.fileType->majorBrand, mp4::FourCc("isom"));
    EXPECT_EQ(file.fileType->minorVersion, 0x200u);
    ASSERT_EQ(file.fileType->compatibleBrands.size(), 4u);
    EXPECT_EQ(file.fileType->compatibleBrands[3], mp4::FourCc("mp41"));
    EXPECT_EQ(file.detail, "brand 'isom', 2 tracks (1 video, 1 audio), moov after the media data (1 mdat box)");

    const mp4::Movie& movie = *file.movie;
    EXPECT_EQ(movie.header.timescale, 1000u);
    EXPECT_EQ(movie.header.nextTrackId, 3u);
    EXPECT_FALSE(movie.fragmented);
    EXPECT_EQ(movie.count(TrackKind::Video), 1u);
    EXPECT_EQ(movie.count(TrackKind::Audio), 1u);

    const mp4::Track& picture = movie.tracks[0];
    EXPECT_EQ(picture.kind, TrackKind::Video);
    EXPECT_EQ(picture.handler, mp4::FourCc("vide"));
    EXPECT_EQ(picture.header.trackId, 1u);
    EXPECT_TRUE(picture.header.enabled());
    EXPECT_EQ(picture.header.width, 64u << 16);
    EXPECT_EQ(picture.header.height, 48u << 16);
    EXPECT_EQ(picture.media.timescale, 30000u);
    EXPECT_EQ(picture.media.duration, 12u * 1001);
    EXPECT_EQ(picture.media.isoLanguage(), "und");
    ASSERT_EQ(picture.samples.descriptions.size(), 1u);
    EXPECT_EQ(picture.samples.descriptions[0].format, mp4::FourCc("avc1"));
    EXPECT_EQ(picture.samples.descriptions[0].dataReferenceIndex, 1u);
    EXPECT_EQ(picture.samples.descriptions[0].width, 64u);
    EXPECT_EQ(picture.samples.descriptions[0].height, 48u);
    ASSERT_EQ(picture.samples.timeToSample.size(), 1u);
    EXPECT_EQ(picture.samples.timeToSample[0].sampleCount, 12u);
    EXPECT_EQ(picture.samples.timeToSample[0].sampleDelta, 1001u);
    EXPECT_EQ(picture.samples.sampleToChunk.size(), 1u);
    EXPECT_EQ(picture.chunks.size(), 3u);

    const mp4::Track& sound = movie.tracks[1];
    EXPECT_EQ(sound.kind, TrackKind::Audio);
    EXPECT_EQ(sound.handler, mp4::FourCc("soun"));
    EXPECT_EQ(sound.header.trackId, 2u);
    EXPECT_EQ(sound.media.timescale, 44100u);
    ASSERT_EQ(sound.samples.descriptions.size(), 1u);
    EXPECT_EQ(sound.samples.descriptions[0].format, mp4::FourCc("mp4a"));
    EXPECT_EQ(sound.samples.descriptions[0].channelCount, 2u);
    EXPECT_EQ(sound.samples.descriptions[0].sampleSize, 16u);
    EXPECT_EQ(sound.samples.descriptions[0].sampleRate, 44100u);
    // 20 samples in chunks of 6: a short last chunk, which needs a second stsc entry.
    const Mp4Built shortLast = test::makeMp4(withTracks({audio(20, 6)}));
    const Mp4File parsed = parseBytes(shortLast.bytes);
    ASSERT_TRUE(matchesBuilt(parsed, shortLast));
    EXPECT_EQ(parsed.movie->tracks[0].samples.sampleToChunk.size(), 2u);
    EXPECT_EQ(parsed.movie->tracks[0].chunks.back().sampleCount, 2u);
}

// ---------------------------------------------------------------------------
// Box order
// ---------------------------------------------------------------------------

TEST(Mp4ParserTest, MoovBeforeAfterOrBetweenTheMediaData) {
    for (const Mp4MoovPlace place : {Mp4MoovPlace::First, Mp4MoovPlace::Last, Mp4MoovPlace::Between}) {
        Mp4Options options;
        options.moov = place;
        options.mediaDataBoxes = 2;
        const Mp4Built built = test::makeMp4(options);
        const Mp4File file = parseBytes(built.bytes);
        EXPECT_TRUE(matchesBuilt(file, built)) << static_cast<int>(place);
        EXPECT_NE(file.detail.find(place == Mp4MoovPlace::First ? "before" : "after"), std::string::npos)
            << file.detail;
    }
}

TEST(Mp4ParserTest, MediaDataInSeveralMdatBoxesWithOtherBoxesBetween) {
    for (const std::size_t boxes : {2U, 3U, 7U}) {
        for (const bool interleave : {true, false}) {
            Mp4Options options = withTracks({video(15, 2), audio(23, 3), video(4, 1)});
            options.mediaDataBoxes = boxes;
            options.interleave = interleave;
            options.moov = boxes % 2 == 0 ? Mp4MoovPlace::Between : Mp4MoovPlace::First;
            options.wideBox = true;
            const Mp4Built built = test::makeMp4(options);
            const Mp4File file = parseBytes(built.bytes);
            SCOPED_TRACE(std::to_string(boxes) + (interleave ? " interleaved" : " one track after another"));
            EXPECT_TRUE(matchesBuilt(file, built));
            EXPECT_EQ(file.layout.all(mp4::box::kMdat).size(), boxes);
        }
    }
}

TEST(Mp4ParserTest, ChildrenInAnyOrder) {
    Mp4Options options;
    options.reverseChildren = true;  // mvhd after the tracks, hdlr after minf, stco before stsd
    options.extraBoxes = true;
    const Mp4Built built = test::makeMp4(options);
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(built.bytes);
    ASSERT_GT(test::findBox(boxes, "moov/mvhd").offset, test::findBox(boxes, "moov/trak").offset);
    ASSERT_GT(test::findBox(boxes, stbl("stsd")).offset, test::findBox(boxes, stbl("stco")).offset);
    const Mp4File file = parseBytes(built.bytes);
    EXPECT_TRUE(matchesBuilt(file, built));
    // The handler is read first whatever the order, so the video size is found.
    EXPECT_EQ(file.movie->tracks[0].samples.descriptions[0].width, 64u);
}

TEST(Mp4ParserTest, QuickTimeLayoutsAndBoxesTheParserSkips) {
    Mp4Options options;
    options.majorBrand = "qt  ";
    options.compatibleBrands = {"qt  "};
    options.wideBox = true;
    options.uuidBox = true;
    options.freeBox = true;
    options.extraBoxes = true;
    Mp4Built built = test::makeMp4(options);
    EXPECT_TRUE(matchesBuilt(parseBytes(built.bytes), built));

    // No ftyp at all, as old QuickTime files.
    options.majorBrand.clear();
    built = test::makeMp4(options);
    const Mp4File file = parseBytes(built.bytes);
    EXPECT_TRUE(matchesBuilt(file, built));
    EXPECT_FALSE(file.fileType.has_value());
    EXPECT_EQ(file.detail.substr(0, 7), "no ftyp");
}

TEST(Mp4ParserTest, AContainerMayEndWithAZeroTerminator) {
    const Mp4Built built = test::makeMp4();
    const test::BoxPosition& trak = test::findBox(test::mp4Boxes(built.bytes), "moov/trak");
    const Bytes terminated = edited(built.bytes, trak.offset, trak.offset + trak.size, 0, Bytes(4));
    EXPECT_EQ(parseBytes(terminated).status, FileStatus::Valid) << describe(parseBytes(terminated));
    // Four bytes that are not zero are not a box.
    const Bytes junk = edited(built.bytes, trak.offset, trak.offset + trak.size, 0, be32(0x01020304));
    expectInvalidWith(junk, "are not a whole box");
}

TEST(Mp4ParserTest, FtypMustComeFirst) {
    const Mp4Built built = test::makeMp4();
    const Bytes moved = testing::concat({test::makeBox("free", Bytes(8)), built.bytes});
    // Everything moves by 16 bytes: the chunk offsets no longer point at the samples, but still into mdat.
    const Mp4File file = parseBytes(moved);
    EXPECT_EQ(file.status, FileStatus::Invalid);
    EXPECT_TRUE(hasIssue(file, "ftyp is not the first box")) << describe(file);
}

// ---------------------------------------------------------------------------
// Extended sizes
// ---------------------------------------------------------------------------

TEST(Mp4ParserTest, SixtyFourBitSizesAtEveryLevel) {
    for (const Mp4MoovPlace place : {Mp4MoovPlace::First, Mp4MoovPlace::Last}) {
        Mp4Options options = withTracks({video(), audio(), text()});
        options.moov = place;
        options.largeMediaData = true;
        options.largeMoov = true;
        options.largeNested = true;
        options.co64 = true;
        const Mp4Built built = test::makeMp4(options);
        const Mp4File file = parseBytes(built.bytes);
        ASSERT_TRUE(matchesBuilt(file, built));
        EXPECT_EQ(file.layout.first(mp4::box::kMoov)->sizeKind, mp4::BoxSize::Large);
        EXPECT_EQ(file.layout.first(mp4::box::kMdat)->sizeKind, mp4::BoxSize::Large);
        EXPECT_EQ(file.layout.first(mp4::box::kMdat)->headerSize, 16u);
        EXPECT_EQ(file.movie->tracks[0].box.sizeKind, mp4::BoxSize::Large);
        EXPECT_TRUE(file.movie->tracks[0].samples.wideChunkOffsets);
    }
}

TEST(Mp4ParserTest, VersionOneHeaders) {
    Mp4TrackOptions picture = video();
    picture.version1 = true;
    Mp4TrackOptions sound = audio();
    sound.version1 = true;
    Mp4Options options = withTracks({picture, sound});
    options.movieVersion1 = true;
    const Mp4Built built = test::makeMp4(options);
    const Mp4File file = parseBytes(built.bytes);
    ASSERT_TRUE(matchesBuilt(file, built));
    EXPECT_EQ(file.movie->header.version, 1u);
    EXPECT_EQ(file.movie->header.timescale, 1000u);
    EXPECT_EQ(file.movie->header.nextTrackId, 3u);
    EXPECT_EQ(file.movie->tracks[0].header.version, 1u);
    EXPECT_EQ(file.movie->tracks[0].header.trackId, 1u);
    EXPECT_EQ(file.movie->tracks[0].header.width, 64u << 16);
    EXPECT_EQ(file.movie->tracks[1].media.version, 1u);
    EXPECT_EQ(file.movie->tracks[1].media.timescale, 44100u);
    EXPECT_EQ(file.movie->tracks[1].media.duration, 20u * 1024);
    EXPECT_EQ(file.movie->tracks[1].media.isoLanguage(), "und");
}

TEST(Mp4ParserTest, MediaDataToTheEndOfTheFile) {
    Mp4Options options;
    options.moov = Mp4MoovPlace::First;
    options.mediaDataToEnd = true;
    const Mp4Built built = test::makeMp4(options);
    const Mp4File file = parseBytes(built.bytes);
    ASSERT_TRUE(matchesBuilt(file, built));
    EXPECT_EQ(file.layout.boxes.back().sizeKind, mp4::BoxSize::ToEnd);
}

TEST(Mp4ParserTest, SampleSizesInEveryForm) {
    // stz2 with 4-bit fields (an odd count: the last byte is half padding), 8-bit and 16-bit fields.
    Mp4TrackOptions nibbles = video(13, 4);
    nibbles.fixedSampleSize = 9;
    nibbles.compactSizes = true;
    Mp4TrackOptions bytes = audio(10, 3);
    bytes.fixedSampleSize = 200;
    bytes.compactSizes = true;
    Mp4TrackOptions words = video(9, 2);
    words.compactSizes = true;
    // One size for all samples.
    Mp4TrackOptions uniform = audio(11, 4);
    uniform.uniformSize = true;
    const Mp4Built built = test::makeMp4(withTracks({nibbles, bytes, words, uniform}));
    const Mp4File file = parseBytes(built.bytes);
    ASSERT_TRUE(matchesBuilt(file, built));
    EXPECT_TRUE(file.movie->tracks[0].samples.compactSampleSizes);
    EXPECT_EQ(file.movie->tracks[0].samples.sampleSizes.size(), 13u);
    EXPECT_NE(file.movie->tracks[3].samples.uniformSampleSize, 0u);
    EXPECT_TRUE(file.movie->tracks[3].samples.sampleSizes.empty());
}

// A moov-first file with co64 and a 64-bit mdat, with `gap` zero bytes
// inserted at the start of the media data: the chunk offsets move by `gap`.
struct Stretched {
    std::vector<std::byte> head;  // everything up to the media data
    std::vector<std::byte> media;
    std::uint64_t mediaOffset = 0;
    std::uint64_t size = 0;
};

Stretched stretched(std::uint64_t gap) {
    Mp4Options options;
    options.moov = Mp4MoovPlace::First;
    options.co64 = true;
    options.largeMediaData = true;
    const Mp4Built built = test::makeMp4(options);
    Bytes file = built.bytes;
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(file);
    for (std::size_t t = 0; t < 2; ++t) {
        const test::BoxPosition& co64 = test::findBox(boxes, stbl("co64"), t);
        const std::size_t count = be32At(file, co64.offset + 12);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t at = co64.offset + 16 + 8 * i;
            const std::uint64_t offset = (std::uint64_t{be32At(file, at)} << 32) | be32At(file, at + 4);
            file = overwritten(std::move(file), at, be64(offset + gap));
        }
    }
    const test::BoxPosition& mdat = test::findBox(boxes, "mdat");
    file = overwritten(std::move(file), mdat.offset + 8, be64(mdat.size + gap));
    Stretched out;
    out.head.assign(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(mdat.offset + 16));
    out.media.assign(file.begin() + static_cast<std::ptrdiff_t>(mdat.offset + 16), file.end());
    out.mediaOffset = mdat.offset + 16 + gap;
    out.size = file.size() + gap;
    return out;
}

TEST(Mp4ParserTest, OffsetsAndSizesBeyondFourGigabytes) {
    const std::uint64_t gap = 5 * kGiB;
    const Stretched file = stretched(gap);
    test::VirtualSource source(file.size);
    source.plant(0, file.head);
    source.plant(file.mediaOffset, file.media);
    RECOVERY_ASSERT_OK(source.open());
    Result<std::unique_ptr<carving::SourceContentReader>> content =
        carving::SourceContentReader::open(source, 0, file.size);
    RECOVERY_ASSERT_OK(content);
    Result<Mp4File> parsed = mp4::parseFile(**content);
    RECOVERY_ASSERT_OK(parsed);
    ASSERT_EQ(parsed->status, FileStatus::Valid) << describe(*parsed);
    EXPECT_EQ(parsed->layout.boxes.back().size, gap + file.media.size() + 16);
    for (const mp4::Track& track : parsed->movie->tracks) {
        for (const mp4::Chunk& chunk : track.chunks) {
            EXPECT_GE(chunk.offset, gap);
            EXPECT_EQ(chunk.placement, ChunkPlacement::MediaData);
        }
    }
    // Nothing near the gap was read: the parser follows the sizes.
    EXPECT_LT(source.stats().bytes, 4 * kMiB);
}

TEST(Mp4ParserTest, AMoovBeyondFourGigabytes) {
    // moov last, after a 5 GiB mdat whose last bytes hold the samples.
    const Mp4Built built = test::makeMp4([] {
        Mp4Options options;
        options.co64 = true;
        options.largeMediaData = true;
        return options;
    }());
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(built.bytes);
    const test::BoxPosition& mdat = test::findBox(boxes, "mdat");
    const test::BoxPosition& moov = test::findBox(boxes, "moov");
    const std::uint64_t gap = 5 * kGiB;
    Bytes head(built.bytes.begin(), built.bytes.begin() + static_cast<std::ptrdiff_t>(mdat.offset + 16));
    head = overwritten(std::move(head), mdat.offset + 8, be64(mdat.size + gap));
    Bytes tail(built.bytes.begin() + static_cast<std::ptrdiff_t>(mdat.offset + 16), built.bytes.end());
    for (std::size_t t = 0; t < 2; ++t) {
        const test::BoxPosition& co64 = test::findBox(boxes, stbl("co64"), t);
        for (std::size_t i = 0; i < be32At(built.bytes, co64.offset + 12); ++i) {
            const std::size_t at = co64.offset + 16 + 8 * i - (mdat.offset + 16);
            const std::uint64_t offset = (std::uint64_t{be32At(tail, at)} << 32) | be32At(tail, at + 4);
            tail = overwritten(std::move(tail), at, be64(offset + gap));
        }
    }
    test::VirtualSource source(built.bytes.size() + gap);
    source.plant(0, head);
    source.plant(mdat.offset + 16 + gap, tail);
    RECOVERY_ASSERT_OK(source.open());
    auto content = carving::SourceContentReader::open(source, 0, source.size());
    RECOVERY_ASSERT_OK(content);
    Result<Mp4File> parsed = mp4::parseFile(**content);
    RECOVERY_ASSERT_OK(parsed);
    ASSERT_EQ(parsed->status, FileStatus::Valid) << describe(*parsed);
    EXPECT_EQ(parsed->movie->box.offset, moov.offset + gap);
    EXPECT_GT(parsed->movie->box.offset, 4 * kGiB);
}

// ---------------------------------------------------------------------------
// Corrupt box sizes
// ---------------------------------------------------------------------------

TEST(Mp4ParserTest, CorruptTopLevelSizesAreNeverValid) {
    Mp4Options options;
    options.freeBox = true;
    options.mediaDataBoxes = 2;
    for (const Mp4MoovPlace place : {Mp4MoovPlace::First, Mp4MoovPlace::Last, Mp4MoovPlace::Between}) {
        options.moov = place;
        const Mp4Built built = test::makeMp4(options);
        std::vector<test::BoxPosition> top;
        for (const test::BoxPosition& box : test::mp4Boxes(built.bytes)) {
            if (box.path.find('/') == std::string::npos) {
                top.push_back(box);
            }
        }
        for (std::size_t b = 0; b < top.size(); ++b) {
            const test::BoxPosition& box = top[b];
            std::vector<std::uint64_t> sizes = {1, 2, 4, 7, box.size - 1, box.size + 1, box.size + 8, 0xFFFFFFFF};
            if (box.size >= 16) {
                sizes.push_back(box.size - 8);
            }
            // Size 0 hides the boxes after this one. With no moov among them
            // that is still a valid file: an mdat to the end holds every chunk.
            const bool moovAfter = std::any_of(top.begin() + static_cast<std::ptrdiff_t>(b) + 1, top.end(),
                                               [](const test::BoxPosition& later) { return later.path == "moov"; });
            if (moovAfter) {
                sizes.push_back(0);
            }
            for (const std::uint64_t size : sizes) {
                const Mp4File file = parseBytes(withBe32(built.bytes, box.offset, size));
                EXPECT_NE(file.status, FileStatus::Valid)
                    << box.path << " at " << box.offset << " with size " << size << ": " << describe(file);
            }
        }
    }
}

TEST(Mp4ParserTest, CorruptNestedSizesAreFound) {
    // Every box of the plan's list inside moov, and the sample descriptions.
    // (A box the parser does not know, such as vmhd or dinf, that grows over
    // the header of another unknown box cannot be told from a valid file.)
    static const std::vector<std::string_view> kKnown = {"mvhd", "trak", "tkhd", "mdia", "mdhd", "hdlr",
                                                         "minf", "stbl", "stsd", "stts", "stsc", "stsz",
                                                         "stco", "avc1", "mp4a", "tx3g"};
    Mp4Options options = withTracks({video(), audio(), text()});
    options.extraBoxes = true;
    const Mp4Built built = test::makeMp4(options);
    std::size_t checked = 0;
    for (const test::BoxPosition& box : test::mp4Boxes(built.bytes)) {
        const std::string type = box.path.substr(box.path.rfind('/') + 1);
        if (box.path.find('/') == std::string::npos ||
            std::find(kKnown.begin(), kKnown.end(), type) == kKnown.end()) {
            continue;
        }
        for (const std::uint64_t size : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{5}, box.size - 1,
                                         box.size + 1, box.size - 8, box.size + 8, std::uint64_t{0xFFFFFFFF}}) {
            const Mp4File file = parseBytes(withBe32(built.bytes, box.offset, size));
            EXPECT_EQ(file.status, FileStatus::Invalid)
                << box.path << " at " << box.offset << " with size " << size << ": " << describe(file);
            ++checked;
        }
    }
    EXPECT_GT(checked, 300u);
    // The unknown ones never crash the parser or make it read outside the file.
    for (const test::BoxPosition& box : test::mp4Boxes(built.bytes)) {
        for (const std::uint64_t size : {std::uint64_t{0}, box.size + 8, std::uint64_t{0xFFFFFFFF}}) {
            (void)parseBytes(withBe32(built.bytes, box.offset, size));
        }
    }
}

TEST(Mp4ParserTest, KnownBoxesInTheWrongContainer) {
    const Bytes file = test::makeMp4().bytes;
    // tkhd grown over the header of edts/mdia leaves mdia's children in trak, and so on.
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(file);
    const test::BoxPosition mvhd = test::findBox(boxes, "moov/mvhd");
    expectInvalidWith(withBe32(file, mvhd.offset, mvhd.size + 8), "box 'tkhd' does not belong in 'moov'");
    expectInvalidWith(withBe32(file, mvhd.offset, mvhd.size + 8), "8 bytes after the fields of version 0");
    const test::BoxPosition tkhd = test::findBox(boxes, "moov/trak/tkhd");
    expectInvalidWith(withBe32(file, tkhd.offset, tkhd.size + 8), "box 'mdhd' does not belong in 'trak'");
    // A table in the wrong place.
    const test::BoxPosition stco = test::findBox(boxes, stbl("stco"));
    const Bytes copy(file.begin() + static_cast<std::ptrdiff_t>(stco.offset),
                     file.begin() + static_cast<std::ptrdiff_t>(stco.offset + stco.size));
    const test::BoxPosition minf = test::findBox(boxes, "moov/trak/mdia/minf");
    expectInvalidWith(edited(file, minf.offset, minf.offset + minf.size, 0, copy),
                      "box 'stco' does not belong in 'minf'");
}

TEST(Mp4ParserTest, ABoxLargerThanItsParent) {
    const Mp4Built built = test::makeMp4();
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(built.bytes);
    const test::BoxPosition& stco = test::findBox(boxes, stbl("stco"));
    expectInvalidWith(withBe32(built.bytes, stco.offset, stco.size + 4), "runs past the end of 'stbl'");
    const test::BoxPosition& tkhd = test::findBox(boxes, "moov/trak/tkhd");
    expectInvalidWith(withBe32(built.bytes, tkhd.offset, 3), "smaller than its header");
    expectInvalidWith(withBe32(built.bytes, tkhd.offset, 0), "has size 0");
}

// ---------------------------------------------------------------------------
// Nested corruption: boxes and tables
// ---------------------------------------------------------------------------

TEST(Mp4ParserTest, MissingBoxes) {
    const Bytes file = test::makeMp4().bytes;
    expectInvalidWith(removedBox(file, "moov/mvhd"), "no 'mvhd' box");
    expectInvalidWith(removedBox(file, "moov/trak/tkhd"), "no 'tkhd' box");
    expectInvalidWith(removedBox(file, "moov/trak/mdia"), "no 'mdia' box");
    expectInvalidWith(removedBox(file, "moov/trak/mdia/mdhd"), "no 'mdhd' box");
    expectInvalidWith(removedBox(file, "moov/trak/mdia/hdlr"), "no 'hdlr' box");
    expectInvalidWith(removedBox(file, "moov/trak/mdia/minf"), "no 'minf' box");
    expectInvalidWith(removedBox(file, kStbl), "no 'stbl' box");
    expectInvalidWith(removedBox(file, stbl("stsd")), "no 'stsd' box");
    expectInvalidWith(removedBox(file, stbl("stts")), "no 'stts' box");
    expectInvalidWith(removedBox(file, stbl("stsc")), "no 'stsc' box");
    expectInvalidWith(removedBox(file, stbl("stsz")), "no 'stsz' or 'stz2' box");
    expectInvalidWith(removedBox(file, stbl("stco")), "no 'stco' or 'co64' box");
    // Without its sample table the track has no chunks.
    const Mp4File parsed = parseBytes(removedBox(file, stbl("stco")));
    EXPECT_FALSE(parsed.movie->tracks[0].sampleTableValid);
    EXPECT_TRUE(parsed.movie->tracks[0].chunks.empty());
    EXPECT_TRUE(parsed.movie->tracks[1].sampleTableValid);
}

TEST(Mp4ParserTest, RepeatedBoxes) {
    const Bytes file = test::makeMp4().bytes;
    expectInvalidWith(duplicatedBox(file, "moov/mvhd"), "a second 'mvhd' box");
    expectInvalidWith(duplicatedBox(file, "moov/trak/tkhd"), "a second 'tkhd' box");
    expectInvalidWith(duplicatedBox(file, "moov/trak/mdia/hdlr"), "a second 'hdlr' box");
    expectInvalidWith(duplicatedBox(file, stbl("stsc")), "a second 'stsc' box");
    expectInvalidWith(duplicatedBox(file, stbl("stco")), "a second chunk offset box");
    // stsz and stz2 together.
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(file);
    const test::BoxPosition& stsz = test::findBox(boxes, stbl("stsz"));
    Bytes both = duplicatedBox(file, stbl("stsz"));
    both = overwritten(std::move(both), stsz.offset + stsz.size + 4, {'s', 't', 'z', '2'});
    expectInvalidWith(both, "a second sample size box");
}

TEST(Mp4ParserTest, TwoTracksWithOneIdOrIdZero) {
    Mp4TrackOptions first = video();
    first.trackId = 7;
    Mp4TrackOptions second = audio();
    second.trackId = 3;
    EXPECT_EQ(parseBytes(test::makeMp4(withTracks({first, second})).bytes).status, FileStatus::Valid);
    second.trackId = 7;
    expectInvalidWith(test::makeMp4(withTracks({first, second})).bytes, "track id 7 is also track 1's");
    const Bytes file = test::makeMp4().bytes;
    const test::BoxPosition& tkhd = test::findBox(test::mp4Boxes(file), "moov/trak/tkhd");
    expectInvalidWith(withBe32(file, tkhd.offset + 20, 0), "track id 0");
}

TEST(Mp4ParserTest, DamagedHeaders) {
    const Bytes file = test::makeMp4().bytes;
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(file);
    const test::BoxPosition& mvhd = test::findBox(boxes, "moov/mvhd");
    expectInvalidWith(overwritten(file, mvhd.offset + 8, {2}), "version 2 is not defined");
    expectInvalidWith(withBe32(file, mvhd.offset + 20, 0), "a time scale of 0");
    // Version 1 needs more bytes than a version 0 box holds.
    expectInvalidWith(overwritten(file, mvhd.offset + 8, {1}), "too short for the 112 it needs");
    const test::BoxPosition& mdhd = test::findBox(boxes, "moov/trak/mdia/mdhd");
    expectInvalidWith(withBe32(file, mdhd.offset + 20, 0), "a time scale of 0");
    const test::BoxPosition& hdlr = test::findBox(boxes, "moov/trak/mdia/hdlr");
    expectInvalidWith(edited(file, hdlr.offset, hdlr.offset + 8 + 16, hdlr.size - 8 - 16), "too short for the 24");
    const test::BoxPosition& tkhd = test::findBox(boxes, "moov/trak/tkhd");
    expectInvalidWith(edited(file, tkhd.offset, tkhd.offset + 40, tkhd.size - 40), "too short for the 84");
}

TEST(Mp4ParserTest, DamagedSampleDescriptions) {
    const Bytes file = test::makeMp4().bytes;
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(file);
    const test::BoxPosition& stsd = test::findBox(boxes, stbl("stsd"));
    expectInvalidWith(withBe32(file, stsd.offset + 12, 0), "no sample descriptions");
    expectInvalidWith(withBe32(file, stsd.offset + 12, 2), "holds 1 sample description, its count says 2");
    // The video entry cut down to 40 bytes of payload.
    const test::BoxPosition& avc1 = test::findBox(boxes, stbl("stsd/avc1"));
    expectInvalidWith(edited(file, avc1.offset, avc1.offset + 48, avc1.size - 48), "of a video track holds 40 bytes");
    // ... and the audio entry to 20.
    const test::BoxPosition& mp4a = test::findBox(boxes, stbl("stsd/mp4a"));
    expectInvalidWith(edited(file, mp4a.offset, mp4a.offset + 28, mp4a.size - 28), "of an audio track holds 20 bytes");
    // Bytes after the entries that are not a box.
    expectInvalidWith(edited(file, stsd.offset, stsd.offset + stsd.size, 0, Bytes(12, std::byte{0x41})),
                      "after the sample descriptions");
    mp4::ParseLimits limits;
    limits.maxSampleDescriptions = 1;
    const Mp4File tooMany = parseBytes(withBe32(file, stsd.offset + 12, 2), limits);
    EXPECT_TRUE(tooMany.issues.contains(IssueKind::LimitExceeded)) << describe(tooMany);
}

TEST(Mp4ParserTest, DamagedTables) {
    const Mp4Built built = test::makeMp4(withTracks({video(12, 4), audio(20, 6)}));
    const Bytes& file = built.bytes;
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(file);
    const auto countAt = [&](std::string_view path) { return test::findBox(boxes, path).offset + 12; };
    // Counts larger than the box.
    expectInvalidWith(withBe32(file, countAt(stbl("stts")), 1000), "do not fit in");
    expectInvalidWith(withBe32(file, countAt(stbl("stsc")), 0xFFFFFFFF), "do not fit in");
    expectInvalidWith(withBe32(file, countAt(stbl("stsz")) + 4, 13), "do not fit in");
    expectInvalidWith(withBe32(file, countAt(stbl("stco")), 4), "do not fit in");
    // stts times too few samples.
    expectInvalidWith(withBe32(file, firstEntry(file, stbl("stts")), 11), "times 11 samples, the track has 12");
    // stsc entries (track 2 has two: chunks of 6, then a last chunk of 2).
    const std::size_t map = firstEntry(file, stbl("stsc"), 1);
    expectInvalidWith(withBe32(file, map, 2), "the first entry starts at chunk 2, not 1");
    expectInvalidWith(withBe32(file, map + 12, 1), "entry 2 starts at chunk 1, not after chunk 1");
    expectInvalidWith(withBe32(file, map + 12, 9), "entry 2 starts at chunk 9 of 4");
    expectInvalidWith(withBe32(file, map + 4, 0), "entry 1 has chunks of no samples");
    expectInvalidWith(withBe32(file, map + 8, 0), "entry 1 uses sample description 0 of 1");
    expectInvalidWith(withBe32(file, map + 20, 2), "entry 2 uses sample description 2 of 1");
    expectInvalidWith(withBe32(file, map + 4, 7), "would hold samples beyond the 20 samples");
    expectInvalidWith(withBe32(file, map + 16, 1), "the chunks hold 19 samples, 'stsz' has 20");
    // stz2 with a field size that does not exist.
    Mp4TrackOptions compact = video();
    compact.compactSizes = true;
    const Bytes stz2File = test::makeMp4(withTracks({compact})).bytes;
    const test::BoxPosition& stz2 = test::findBox(test::mp4Boxes(stz2File), stbl("stz2"));
    expectInvalidWith(overwritten(stz2File, stz2.offset + 15, {5}), "a field size of 5 bits");
    expectInvalidWith(withBe32(stz2File, stz2.offset + 16, 1000), "do not fit in");
}

TEST(Mp4ParserTest, ChunksOutsideTheMediaDataOrOverlapping) {
    // moov first, then two mdat boxes with a free box between them.
    Mp4Options options = withTracks({video(12, 4), audio(20, 6)});
    options.moov = Mp4MoovPlace::First;
    options.mediaDataBoxes = 2;
    const Mp4Built built = test::makeMp4(options);
    const Bytes& file = built.bytes;
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(file);
    const std::size_t videoOffsets = firstEntry(file, stbl("stco"));
    const std::size_t audioOffsets = firstEntry(file, stbl("stco"), 1);
    // Into ftyp, and into moov.
    expectInvalidWith(withBe32(file, videoOffsets, 4), "1 chunk lies outside the media data, the first chunk 1");
    const std::size_t moov = test::findBox(boxes, "moov").offset;
    expectInvalidWith(withBe32(file, videoOffsets + 4, moov + 8), "the first chunk 2");
    // Running past the end of the first mdat, over the free box, into the second.
    const test::BoxPosition first = test::findBox(boxes, "mdat");
    expectInvalidWith(withBe32(file, videoOffsets, first.offset + first.size - 10), "outside the media data");
    // Two tracks' chunks on the same bytes.
    expectInvalidWith(withBe32(file, audioOffsets, built.chunks[0][0].offset + 5), "overlaps chunk 1 of track 1");
    // Beyond the end of the file: the data is missing, which is truncation.
    const Mp4File beyond = parseBytes(withBe32(file, videoOffsets, file.size() + 100));
    EXPECT_EQ(beyond.status, FileStatus::Truncated) << describe(beyond);
    EXPECT_EQ(beyond.movie->tracks[0].chunks[0].placement, ChunkPlacement::BeyondData);
    // A 64-bit offset whose chunk ends beyond the largest offset.
    Mp4Options wideOptions;
    wideOptions.co64 = true;
    const Bytes wide = test::makeMp4(wideOptions).bytes;
    const std::size_t entry = firstEntry(wide, stbl("co64"));
    expectInvalidWith(overwritten(wide, entry, be64(UINT64_MAX - 100)), "ends beyond the largest offset");
}

TEST(Mp4ParserTest, AMovieWithoutTracks) {
    const Bytes file = test::makeMp4().bytes;
    Bytes none = removedBox(file, "moov/trak", 1);
    none = removedBox(none, "moov/trak", 0);
    expectInvalidWith(none, "no tracks");
}

// ---------------------------------------------------------------------------
// Truncation
// ---------------------------------------------------------------------------

TEST(Mp4ParserTest, EveryPrefixIsTruncated) {
    for (const Mp4MoovPlace place : {Mp4MoovPlace::First, Mp4MoovPlace::Last, Mp4MoovPlace::Between}) {
        Mp4Options options;
        options.moov = place;
        options.mediaDataBoxes = 2;
        options.freeBox = true;
        const Bytes file = test::makeMp4(options).bytes;
        for (std::size_t length = 0; length < file.size(); length += length < 2048 ? 1 : 7) {
            const Mp4File parsed = parseBytes(std::span(file).first(length));
            ASSERT_EQ(parsed.status, FileStatus::Truncated)
                << static_cast<int>(place) << " at " << length << ": " << describe(parsed);
        }
        EXPECT_EQ(parseBytes(file).status, FileStatus::Valid);
    }
}

TEST(Mp4ParserTest, TruncationReasons) {
    Mp4Options options;
    options.moov = Mp4MoovPlace::First;
    const Mp4Built first = test::makeMp4(options);
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(first.bytes);
    const test::BoxPosition& mdat = test::findBox(boxes, "mdat");
    // Cut inside mdat: the chunks there are in the media data, the ones after it beyond the data.
    const Mp4File cut = parseBytes(std::span(first.bytes).first(mdat.offset + mdat.size / 2));
    EXPECT_EQ(cut.status, FileStatus::Truncated);
    EXPECT_NE(cut.detail.find("the data ends inside box 'mdat'"), std::string::npos) << cut.detail;
    bool inside = false;
    bool beyond = false;
    for (const mp4::Track& track : cut.movie->tracks) {
        for (const mp4::Chunk& chunk : track.chunks) {
            inside = inside || chunk.placement == ChunkPlacement::MediaData;
            beyond = beyond || chunk.placement == ChunkPlacement::BeyondData;
        }
    }
    EXPECT_TRUE(inside);
    EXPECT_TRUE(beyond);
    // Cut right after moov: every chunk is beyond the data.
    const Mp4File bare = parseBytes(std::span(first.bytes).first(mdat.offset));
    EXPECT_EQ(bare.status, FileStatus::Truncated);
    EXPECT_NE(bare.detail.find("before the media data of track 1"), std::string::npos) << bare.detail;
    // moov last, cut after mdat.
    const Bytes last = test::makeMp4().bytes;
    const test::BoxPosition& moov = test::findBox(test::mp4Boxes(last), "moov");
    const Mp4File noMoov = parseBytes(std::span(last).first(moov.offset));
    EXPECT_EQ(noMoov.status, FileStatus::Truncated);
    EXPECT_EQ(noMoov.detail, "the data ends before a moov box");
    EXPECT_FALSE(noMoov.movie.has_value());
}

// ---------------------------------------------------------------------------
// Multiple tracks, audio only, video only
// ---------------------------------------------------------------------------

TEST(Mp4ParserTest, ManyTracksOfEveryKind) {
    Mp4TrackOptions mono = audio(30, 7);
    mono.channels = 1;
    mono.sampleRate = 8000;
    Mp4TrackOptions large = video(8, 3);
    large.width = 1920;
    large.height = 1080;
    const Mp4Built built =
        test::makeMp4(withTracks({video(), audio(), large, mono, text(), audio(5, 5), video(1, 1)}));
    const Mp4File file = parseBytes(built.bytes);
    ASSERT_TRUE(matchesBuilt(file, built));
    const mp4::Movie& movie = *file.movie;
    EXPECT_EQ(movie.count(TrackKind::Video), 3u);
    EXPECT_EQ(movie.count(TrackKind::Audio), 3u);
    EXPECT_EQ(movie.count(TrackKind::Other), 1u);
    EXPECT_EQ(movie.header.nextTrackId, 8u);
    for (std::size_t t = 0; t < movie.tracks.size(); ++t) {
        EXPECT_EQ(movie.tracks[t].header.trackId, t + 1);
    }
    EXPECT_EQ(movie.tracks[2].samples.descriptions[0].width, 1920u);
    EXPECT_EQ(movie.tracks[2].samples.descriptions[0].height, 1080u);
    EXPECT_EQ(movie.tracks[3].samples.descriptions[0].channelCount, 1u);
    EXPECT_EQ(movie.tracks[3].samples.descriptions[0].sampleRate, 8000u);
    EXPECT_EQ(movie.tracks[4].handler, mp4::FourCc("text"));
    EXPECT_EQ(movie.tracks[4].samples.descriptions[0].format, mp4::FourCc("tx3g"));
    EXPECT_NE(file.detail.find("7 tracks (3 video, 3 audio)"), std::string::npos) << file.detail;
}

TEST(Mp4ParserTest, AudioOnlyAndVideoOnly) {
    for (const bool withVideo : {true, false}) {
        const Mp4Built built = test::makeMp4(withTracks({withVideo ? video(30, 5) : audio(40, 9)}));
        const Mp4File file = parseBytes(built.bytes);
        ASSERT_TRUE(matchesBuilt(file, built));
        EXPECT_EQ(file.movie->count(TrackKind::Video), withVideo ? 1u : 0u);
        EXPECT_EQ(file.movie->count(TrackKind::Audio), withVideo ? 0u : 1u);
    }
}

TEST(Mp4ParserTest, ATrackWithoutSamples) {
    const Mp4Built built = test::makeMp4(withTracks({video(), audio(0, 1)}));
    const Mp4File file = parseBytes(built.bytes);
    ASSERT_TRUE(matchesBuilt(file, built));
    EXPECT_TRUE(file.movie->tracks[1].sampleTableValid);
    EXPECT_TRUE(file.movie->tracks[1].chunks.empty());
}

TEST(Mp4ParserTest, TrackLimit) {
    mp4::ParseLimits limits;
    limits.maxTracks = 2;
    const Mp4File file = parseBytes(test::makeMp4(withTracks({video(), audio(), text()})).bytes, limits);
    EXPECT_EQ(file.status, FileStatus::Invalid);
    EXPECT_TRUE(hasIssue(file, "more than 2 tracks")) << describe(file);
    EXPECT_EQ(file.movie->tracks.size(), 2u);
    EXPECT_TRUE(file.issues.contains(IssueKind::LimitExceeded));
}

// ---------------------------------------------------------------------------
// Files that are not in one piece
// ---------------------------------------------------------------------------

TEST(Mp4ParserTest, AFileScatteredOverTheSourceParsesTheSame) {
    Mp4Options options = withTracks({video(40, 3), audio(60, 5)});
    options.mediaDataBoxes = 3;
    options.moov = Mp4MoovPlace::Between;
    const Mp4Built built = test::makeMp4(options);
    // The file's 512-byte clusters in shuffled places of a larger buffer.
    constexpr std::size_t kCluster = 512;
    const std::size_t clusters = (built.bytes.size() + kCluster - 1) / kCluster;
    std::vector<std::size_t> places(clusters * 2);
    std::iota(places.begin(), places.end(), 0);
    std::shuffle(places.begin(), places.end(), std::mt19937_64(7));
    Bytes disk = testing::noise(places.size() * kCluster, 99);
    std::vector<std::span<const std::byte>> pieces;
    for (std::size_t c = 0; c < clusters; ++c) {
        const std::size_t length = std::min(kCluster, built.bytes.size() - c * kCluster);
        std::copy_n(built.bytes.begin() + static_cast<std::ptrdiff_t>(c * kCluster), length,
                    disk.begin() + static_cast<std::ptrdiff_t>(places[c] * kCluster));
    }
    for (std::size_t c = 0; c < clusters; ++c) {
        const std::size_t length = std::min(kCluster, built.bytes.size() - c * kCluster);
        pieces.emplace_back(disk.data() + places[c] * kCluster, length);
    }
    testing::ScatteredContentReader content(pieces);
    Result<Mp4File> parsed = mp4::parseFile(content);
    RECOVERY_ASSERT_OK(parsed);
    EXPECT_TRUE(matchesBuilt(*parsed, built));
}

TEST(Mp4ParserTest, AMoovFoundOnItsOwn) {
    // A moov box among other data, with no ftyp and no media data around it.
    const Mp4Built built = test::makeMp4(withTracks({video(), audio(), text()}));
    const test::BoxPosition& moov = test::findBox(test::mp4Boxes(built.bytes), "moov");
    const Bytes moovBytes(built.bytes.begin() + static_cast<std::ptrdiff_t>(moov.offset),
                          built.bytes.begin() + static_cast<std::ptrdiff_t>(moov.offset + moov.size));
    const Bytes region = testing::concat({testing::noise(3000, 5), moovBytes, testing::noise(100, 6)});
    carving::MemoryContentReader content(region);
    Result<mp4::BoxRead> header = mp4::readBoxHeader(content, 3000, region.size());
    RECOVERY_ASSERT_OK(header);
    ASSERT_EQ(header->status, mp4::BoxStatus::Valid);
    Result<mp4::Movie> movie = mp4::parseMovie(content, header->header);
    RECOVERY_ASSERT_OK(movie);
    EXPECT_TRUE(movie->issues.empty()) << movie->issues.recorded().front().describe();
    ASSERT_EQ(movie->tracks.size(), 3u);
    for (std::size_t t = 0; t < 3; ++t) {
        const mp4::Track& track = movie->tracks[t];
        ASSERT_TRUE(track.sampleTableValid);
        ASSERT_EQ(track.chunks.size(), built.chunks[t].size());
        for (std::size_t c = 0; c < track.chunks.size(); ++c) {
            // Offsets as the file records them: from the start of the original file.
            EXPECT_EQ(track.chunks[c].offset, built.chunks[t][c].offset);
            EXPECT_EQ(track.chunks[c].size, built.chunks[t][c].size);
            EXPECT_EQ(track.chunks[c].placement, ChunkPlacement::Unchecked);
        }
    }
    // What is not a moov box inside the content is refused.
    mp4::BoxHeader wrong = header->header;
    wrong.type = mp4::box::kTrak;
    RECOVERY_EXPECT_ERROR(mp4::parseMovie(content, wrong), ErrorCode::InvalidInput);
    wrong = header->header;
    wrong.offset = region.size() - 10;
    RECOVERY_EXPECT_ERROR(mp4::parseMovie(content, wrong), ErrorCode::InvalidInput);
}

TEST(Mp4ParserTest, TwoFilesBackToBack) {
    // The next ftyp is not taken as the end: it is reported, and so is the second moov.
    const Mp4Built one = test::makeMp4();
    const Mp4Built two = test::makeMp4(withTracks({audio()}));
    const Bytes both = testing::concat({one.bytes, two.bytes});
    const Mp4File file = parseBytes(both);
    EXPECT_EQ(file.status, FileStatus::Invalid);
    EXPECT_TRUE(hasIssue(file, "a second ftyp box")) << describe(file);
    EXPECT_TRUE(hasIssue(file, "a second moov box")) << describe(file);
    EXPECT_EQ(file.layout.all(mp4::box::kFtyp).size(), 2u);
    ASSERT_TRUE(file.movie.has_value());
    EXPECT_EQ(file.movie->tracks.size(), 2u);  // the first file's movie
}

// ---------------------------------------------------------------------------
// Limits, reads, arguments
// ---------------------------------------------------------------------------

TEST(Mp4ParserTest, TableEntryAndBoxLimits) {
    const Bytes file = test::makeMp4().bytes;
    mp4::ParseLimits limits;
    limits.maxTableEntries = 10;
    Mp4File parsed = parseBytes(file, limits);
    EXPECT_EQ(parsed.status, FileStatus::Invalid);
    EXPECT_TRUE(hasIssue(parsed, "would pass the limit of 10")) << describe(parsed);
    EXPECT_TRUE(parsed.issues.contains(IssueKind::LimitExceeded));

    limits = {};
    limits.maxBoxes = 12;
    parsed = parseBytes(file, limits);
    EXPECT_EQ(parsed.status, FileStatus::Invalid);
    EXPECT_TRUE(hasIssue(parsed, "more than 12 boxes")) << describe(parsed);
    // The count covers the top level too.
    limits.maxBoxes = 2;
    parsed = parseBytes(file, limits);
    EXPECT_TRUE(parsed.issues.contains(IssueKind::LimitExceeded)) << describe(parsed);

    carving::MemoryContentReader content(file);
    limits = {};
    limits.maxTracks = 0;
    RECOVERY_EXPECT_ERROR(mp4::parseFile(content, limits), ErrorCode::InvalidInput);
}

TEST(Mp4ParserTest, HugeCountsAreRefusedBeforeAnythingIsRead) {
    // moov/trak/mdia/minf/stbl/stsz with 64-bit sizes, stsz holding 2^30
    // entries that are really there (zeros in a virtual source of 4 GiB):
    // within the rules, beyond the limits. The count is checked before the
    // table is allocated or read.
    const auto large = [](std::string_view type, std::uint64_t size) {
        Bytes out = be32(1);
        for (const char c : type) {
            out.push_back(static_cast<std::byte>(c));
        }
        return testing::concat({out, be64(size)});
    };
    const std::uint64_t entries = std::uint64_t{1} << 30;
    const std::uint64_t stszSize = 16 + 12 + entries * 4;
    const std::uint64_t stblSize = 16 + stszSize;
    const std::uint64_t minfSize = 16 + stblSize;
    const std::uint64_t mdiaSize = 16 + minfSize;
    const std::uint64_t trakSize = 16 + mdiaSize;
    const std::uint64_t movieSize = 16 + trakSize;
    const Bytes structure = testing::concat({large("moov", movieSize), large("trak", trakSize),
                                             large("mdia", mdiaSize), large("minf", minfSize),
                                             large("stbl", stblSize), large("stsz", stszSize), Bytes(4), be32(0),
                                             be32(entries)});
    test::VirtualSource source(movieSize);
    source.plant(0, structure);
    RECOVERY_ASSERT_OK(source.open());
    auto content = carving::SourceContentReader::open(source, 0, movieSize);
    RECOVERY_ASSERT_OK(content);
    Result<mp4::BoxRead> moov = mp4::readBoxHeader(**content, 0, movieSize);
    RECOVERY_ASSERT_OK(moov);
    ASSERT_EQ(moov->status, mp4::BoxStatus::Valid);
    Result<mp4::Movie> movie = mp4::parseMovie(**content, moov->header);
    RECOVERY_ASSERT_OK(movie);
    EXPECT_TRUE(movie->issues.contains(IssueKind::LimitExceeded));
    ASSERT_EQ(movie->tracks.size(), 1u);
    EXPECT_TRUE(movie->tracks[0].samples.sampleSizes.empty());
    EXPECT_LT(source.stats().bytes, 1 * kMiB);
}

TEST(Mp4ParserTest, FailingReadsFailTheParse) {
    // A read failure that is not a bad sector, inside moov, reaches the caller.
    const Bytes file = test::makeMp4().bytes;
    const std::uint64_t moov = test::findBox(test::mp4Boxes(file), "moov").offset;
    const std::uint64_t sourceSize = (file.size() / 512 + 1) * 512;
    test::VirtualSource source(sourceSize);
    source.plant(0, file);
    source.addFatalSector((moov + 200) / 512);
    RECOVERY_ASSERT_OK(source.open());
    auto content = carving::SourceContentReader::open(source, 0, file.size(), {},
                                                      carving::SourceContentReader::kMinCacheSize);
    RECOVERY_ASSERT_OK(content);
    Result<Mp4File> parsed = mp4::parseFile(**content);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.error().code, ErrorCode::InternalError);

    // Cancellation.
    CancellationSource cancel;
    carving::SourceReadOptions options;
    options.cancellation = cancel.token();
    test::VirtualSource plain(sourceSize);
    plain.plant(0, file);
    RECOVERY_ASSERT_OK(plain.open());
    auto cancellable = carving::SourceContentReader::open(plain, 0, file.size(), options);
    RECOVERY_ASSERT_OK(cancellable);
    cancel.requestCancellation();
    RECOVERY_EXPECT_ERROR(mp4::parseFile(**cancellable), ErrorCode::Cancelled);
}

TEST(Mp4ParserTest, FileTypeBrands) {
    mp4::IssueList issues;
    const Bytes ftyp = test::makeBox("ftyp", Bytes{std::byte{'m'}, std::byte{'p'}, std::byte{'4'}, std::byte{'2'},
                                                   std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
                                                   std::byte{'i'}, std::byte{'s'}, std::byte{'o'}, std::byte{'m'},
                                                   std::byte{'x'}});
    carving::MemoryContentReader content(ftyp);
    Result<mp4::BoxRead> header = mp4::readBoxHeader(content, 0, ftyp.size());
    RECOVERY_ASSERT_OK(header);
    Result<mp4::FileType> type = mp4::parseFileType(content, header->header, issues);
    RECOVERY_ASSERT_OK(type);
    EXPECT_EQ(type->majorBrand, mp4::FourCc("mp42"));
    EXPECT_EQ(type->minorVersion, 1u);
    ASSERT_EQ(type->compatibleBrands.size(), 1u);
    EXPECT_EQ(type->compatibleBrands[0], mp4::FourCc("isom"));
    ASSERT_EQ(issues.count(), 1u);
    EXPECT_NE(issues.recorded()[0].detail.find("not a whole number of brands"), std::string::npos);

    const Bytes tiny = test::makeBox("ftyp", Bytes(6));
    carving::MemoryContentReader small(tiny);
    mp4::IssueList more;
    header = mp4::readBoxHeader(small, 0, tiny.size());
    RECOVERY_ASSERT_OK(mp4::parseFileType(small, header->header, more));
    EXPECT_NE(more.recorded()[0].detail.find("too short for a brand"), std::string::npos);
    header->header.type = mp4::box::kFree;
    RECOVERY_EXPECT_ERROR(mp4::parseFileType(small, header->header, more), ErrorCode::InvalidInput);
}

TEST(Mp4ParserTest, IssueListKeepsTheFirstIssuesAndCountsAll) {
    mp4::IssueList list;
    for (int i = 0; i < 40; ++i) {
        list.add(IssueKind::Malformed, static_cast<std::uint64_t>(i), "moov", "issue " + std::to_string(i));
    }
    EXPECT_EQ(list.count(), 40u);
    EXPECT_EQ(list.recorded().size(), mp4::IssueList::kMaxRecorded);
    EXPECT_EQ(list.recorded().back().detail, "issue 31");
    EXPECT_FALSE(list.contains(IssueKind::LimitExceeded));
    mp4::IssueList other;
    other.add(IssueKind::LimitExceeded, 0, "", "limit");
    other.append(list);
    EXPECT_EQ(other.count(), 41u);
    EXPECT_EQ(other.recorded().size(), mp4::IssueList::kMaxRecorded);
    EXPECT_TRUE(other.contains(IssueKind::LimitExceeded));
    EXPECT_EQ(other.recorded()[0].describe(), "at 0: limit");
    EXPECT_EQ(other.recorded()[1].describe(), "moov at 0: issue 0");
}

TEST(Mp4ParserTest, Languages) {
    mp4::MediaHeader header;
    header.language = 0x55C4;
    EXPECT_EQ(header.isoLanguage(), "und");
    header.language = 0x15C7;
    EXPECT_EQ(header.isoLanguage(), "eng");
    header.language = 0;  // Macintosh English
    EXPECT_EQ(header.isoLanguage(), "");
    header.language = 0x7FFF;
    EXPECT_EQ(header.isoLanguage(), "");
}

// ---------------------------------------------------------------------------
// Fuzzing
// ---------------------------------------------------------------------------

TEST(Mp4ParserTest, Fuzzing) {
    Mp4Options first;
    first.moov = Mp4MoovPlace::First;
    Mp4Options wide = withTracks({video(6, 2), audio(9, 3), text(2)});
    wide.largeMoov = true;
    wide.largeNested = true;
    wide.co64 = true;
    wide.largeMediaData = true;
    Mp4Options spread = withTracks({video(8, 3), audio(8, 3)});
    spread.mediaDataBoxes = 3;
    spread.moov = Mp4MoovPlace::Between;
    spread.reverseChildren = true;
    Mp4TrackOptions compact = video(7, 2);
    compact.compactSizes = true;
    compact.fixedSampleSize = 11;
    const Mp4Options stz2 = withTracks({compact});
    std::uint64_t seed = 1;
    for (const Mp4Options& options : {Mp4Options{}, first, wide, spread, stz2}) {
        const Bytes file = test::makeMp4(options).bytes;
        testing::fuzzParser(file, 1500, seed++);
    }
}

}  // namespace
}  // namespace recovery::formats
