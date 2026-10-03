// Movie fragments in the MP4 parser (P12): moov with mvex/trex, then moof
// and mdat pairs, their base data offsets given in each of the three ways
// ISO/IEC 14496-12 allows, samples in moov's tables and in fragments
// together; damaged, missing and inconsistent fragment boxes; truncation at
// every length; limits; fuzzing. And the NAL unit length size read from the
// AVC and HEVC configurations. (FFmpeg's and GPAC's fragmented files are in
// mp4_reference_test.cpp.)

#include "formats/mp4_parser.hpp"

#include "format_test_helpers.hpp"
#include "mp4_test_helpers.hpp"
#include "support/mp4_builders.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace recovery::formats {
namespace {

using mp4::FileStatus;
using mp4::Mp4File;
using test::Mp4Built;
using test::Mp4FragmentBase;
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

Mp4TrackOptions track(Mp4TrackKind kind, std::size_t samples, std::size_t perChunk = 3) {
    Mp4TrackOptions options;
    options.kind = kind;
    options.samples = samples;
    options.samplesPerChunk = perChunk;
    return options;
}

Mp4Options fragmented(Mp4FragmentBase base = Mp4FragmentBase::Moof, std::size_t inMoov = 0) {
    Mp4Options options;
    options.moov = Mp4MoovPlace::First;
    options.tracks = {track(Mp4TrackKind::Video, 12), track(Mp4TrackKind::Audio, 20)};
    options.fragmentSamples = 4;
    options.samplesInMoov = inMoov;
    options.fragmentBase = base;
    return options;
}

Bytes be32(std::uint64_t value) {
    return Bytes{static_cast<std::byte>((value >> 24) & 0xFF), static_cast<std::byte>((value >> 16) & 0xFF),
                 static_cast<std::byte>((value >> 8) & 0xFF), static_cast<std::byte>(value & 0xFF)};
}

Bytes text(std::string_view value) {
    Bytes out;
    for (const char c : value) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

test::BoxPosition box(const Bytes& file, std::string_view path, std::size_t index = 0) {
    return test::findBox(test::mp4Boxes(file), path, index);
}

// The same file with the field `at` bytes into the `index`th box at `path` replaced.
Bytes withField(const Bytes& file, std::string_view path, std::size_t index, std::size_t at, std::uint64_t value) {
    return overwritten(file, box(file, path, index).offset + at, be32(value));
}

Bytes renamed(const Bytes& file, std::string_view path, std::string_view type, std::size_t index = 0) {
    return overwritten(file, box(file, path, index).offset + 4, text(type));
}

TEST(Mp4FragmentTest, FragmentsOfEveryKindParseAsBuilt) {
    for (const Mp4FragmentBase base : {Mp4FragmentBase::Moof, Mp4FragmentBase::Explicit, Mp4FragmentBase::Implicit}) {
        for (const std::size_t inMoov : {std::size_t{0}, std::size_t{5}}) {
            SCOPED_TRACE("base " + std::to_string(static_cast<int>(base)) + ", " + std::to_string(inMoov) +
                         " samples in moov");
            const Mp4Built built = test::makeMp4(fragmented(base, inMoov));
            const Mp4File file = parseBytes(built.bytes);
            ASSERT_TRUE(matchesBuilt(file, built)) << describe(file);
            ASSERT_TRUE(file.movie.has_value());
            EXPECT_TRUE(file.movie->fragmented);
            EXPECT_EQ(file.movie->trackExtends.size(), 2u);
            // Video: 12 samples, audio: 20, four of each per fragment.
            const std::size_t expected = inMoov == 0 ? 5 : 4;
            ASSERT_EQ(file.fragments.size(), expected);
            for (std::size_t i = 0; i < file.fragments.size(); ++i) {
                EXPECT_EQ(file.fragments[i].sequenceNumber, i + 1);
                EXPECT_GE(file.fragments[i].trackFragments, 1u);
                EXPECT_EQ(file.fragments[i].runs, file.fragments[i].trackFragments);
            }
            EXPECT_NE(file.detail.find("movie fragments"), std::string::npos) << file.detail;
        }
    }
}

TEST(Mp4FragmentTest, EveryKindOfTrackAndRun) {
    // Text runs with composition offsets, a uniform size from tfhd (runs
    // without sizes), several track fragments in one moof.
    Mp4Options options = fragmented(Mp4FragmentBase::Implicit);
    Mp4TrackOptions uniform = track(Mp4TrackKind::Video, 9);
    uniform.uniformSize = true;
    options.tracks = {uniform, track(Mp4TrackKind::Audio, 11), track(Mp4TrackKind::Text, 5)};
    options.fragmentSamples = 3;
    const Mp4Built built = test::makeMp4(options);
    const Mp4File file = parseBytes(built.bytes);
    EXPECT_TRUE(matchesBuilt(file, built)) << describe(file);
    ASSERT_EQ(file.fragments.size(), 4u);
    EXPECT_EQ(file.fragments[0].trackFragments, 3u);
}

TEST(Mp4FragmentTest, SamplesOfTablesAndFragmentsComeInOrder) {
    const Mp4Built built = test::makeMp4(fragmented(Mp4FragmentBase::Moof, 5));
    const Mp4File file = parseBytes(built.bytes);
    ASSERT_TRUE(file.movie.has_value());
    for (std::size_t t = 0; t < file.movie->tracks.size(); ++t) {
        const mp4::Track& track = file.movie->tracks[t];
        EXPECT_EQ(track.samples.sampleCount, 5u);
        EXPECT_EQ(track.totalSamples(), built.samples[t].size());
        std::uint32_t expected = 0;
        mp4::forEachSample(track, [&](std::uint32_t index, std::uint64_t offset, std::uint32_t size) {
            EXPECT_EQ(index, expected);
            EXPECT_EQ(offset, built.samples[t][index].offset);
            EXPECT_EQ(size, built.samples[t][index].size);
            ++expected;
        });
        EXPECT_EQ(expected, built.samples[t].size());
    }
}

TEST(Mp4FragmentTest, FragmentsNeedMvexAndTrex) {
    const Bytes file = test::makeMp4(fragmented()).bytes;
    const Mp4File withoutMvex = parseBytes(renamed(file, "moov/mvex", "free"));
    EXPECT_EQ(withoutMvex.status, FileStatus::Invalid);
    EXPECT_FALSE(withoutMvex.movie->fragmented);
    EXPECT_TRUE(hasIssue(withoutMvex, "moov has no 'mvex' box")) << describe(withoutMvex);
    EXPECT_TRUE(hasIssue(withoutMvex, "has no 'trex' box")) << describe(withoutMvex);

    const Mp4File withoutTrex = parseBytes(renamed(file, "moov/mvex/trex", "free", 1));
    EXPECT_TRUE(hasIssue(withoutTrex, "track 2 has no 'trex' box")) << describe(withoutTrex);

    // A second trex for the same track, a trex for a track moov does not have, bytes after its fields.
    const std::uint64_t firstTrack = 1;
    const Mp4File twice = parseBytes(withField(file, "moov/mvex/trex", 1, 12, firstTrack));
    EXPECT_TRUE(hasIssue(twice, "a second 'trex' box for track 1")) << describe(twice);
    const Mp4File stray = parseBytes(withField(file, "moov/mvex/trex", 1, 12, 77));
    EXPECT_TRUE(hasIssue(stray, "defaults for track 77, which no track of moov has")) << describe(stray);
    const test::BoxPosition trex = box(file, "moov/mvex/trex");
    EXPECT_EQ(trex.size, 32u);
    // A default sample description the track does not have.
    const Mp4File description = parseBytes(withField(file, "moov/mvex/trex", 0, 16, 5));
    EXPECT_TRUE(hasIssue(description, "uses sample description 5 of 1")) << describe(description);
}

TEST(Mp4FragmentTest, DamagedFragmentBoxesAreCaught) {
    const Bytes file = test::makeMp4(fragmented()).bytes;
    const auto expectIssue = [&](const Bytes& damaged, std::string_view issue) {
        const Mp4File parsed = parseBytes(damaged);
        EXPECT_EQ(parsed.status, FileStatus::Invalid) << issue;
        EXPECT_TRUE(hasIssue(parsed, issue)) << describe(parsed);
    };
    expectIssue(renamed(file, "moof/mfhd", "xxxx"), "no 'mfhd' box");
    expectIssue(renamed(file, "moof/traf/tfhd", "xxxx"), "no 'tfhd' box");
    expectIssue(withField(file, "moof/traf/tfhd", 0, 12, 99), "track id 99, which no track of moov has");
    expectIssue(withField(file, "moof/mfhd", 1, 12, 1), "sequence number 1 does not follow 1");
    // A run whose data offset points into its own moof box.
    expectIssue(withField(file, "moof/traf/trun", 0, 16, 0), "outside the media data");
    // A negative data offset reaching before the start of the file.
    expectIssue(withField(file, "moof/traf/trun", 0, 16, 0x80000000U), "points before the start of the file");
    // More samples than the box holds.
    expectIssue(withField(file, "moof/traf/trun", 0, 12, 0x10000000U), "do not fit");
    // tfhd flags that call for fields the box does not hold.
    expectIssue(withField(file, "moof/traf/tfhd", 0, 8, 0x020001), "too short");
    // Two runs over the same bytes: the second fragment's first run moved onto the first fragment's.
    const std::uint64_t firstRun = box(file, "mdat").offset + 8;
    const Bytes overlap = withField(file, "moof/traf/trun", 2, 16, firstRun - box(file, "moof", 1).offset);
    expectIssue(overlap, "overlaps");
}

TEST(Mp4FragmentTest, PrefixesAreTruncatedOrCompleteAtABoxBoundary) {
    for (const Mp4FragmentBase base : {Mp4FragmentBase::Moof, Mp4FragmentBase::Implicit}) {
        const Bytes file = test::makeMp4(fragmented(base, 2)).bytes;
        std::set<std::size_t> boundaries;
        for (const test::BoxPosition& top : test::mp4Boxes(file)) {
            if (top.path.find('/') == std::string::npos) {
                boundaries.insert(top.offset + top.size);
            }
        }
        for (std::size_t length = 0; length < file.size(); length += length < 2048 ? 1 : 7) {
            const Mp4File parsed = parseBytes(std::span(file).first(length));
            if (parsed.status == FileStatus::Valid) {
                EXPECT_TRUE(boundaries.contains(length)) << length << ": " << describe(parsed);
            } else {
                ASSERT_EQ(parsed.status, FileStatus::Truncated) << length << ": " << describe(parsed);
            }
        }
    }
}

TEST(Mp4FragmentTest, LimitsBoundTheFragments) {
    const Bytes file = test::makeMp4(fragmented()).bytes;
    mp4::ParseLimits limits;
    limits.maxTableEntries = 24;
    const Mp4File limited = parseBytes(file, limits);
    EXPECT_EQ(limited.status, FileStatus::Invalid);
    EXPECT_TRUE(limited.issues.contains(mp4::IssueKind::LimitExceeded)) << describe(limited);
    limits.maxBoxes = 30;
    limits.maxTableEntries = std::uint64_t{1} << 22;
    const Mp4File boxes = parseBytes(file, limits);
    EXPECT_TRUE(boxes.issues.contains(mp4::IssueKind::LimitExceeded)) << describe(boxes);
}

TEST(Mp4FragmentTest, Fuzzing) {
    std::uint64_t seed = 900;
    for (const Mp4FragmentBase base : {Mp4FragmentBase::Moof, Mp4FragmentBase::Explicit, Mp4FragmentBase::Implicit}) {
        testing::fuzzParser(test::makeMp4(fragmented(base, 3)).bytes, 1500, seed++);
    }
}

TEST(Mp4FragmentTest, NalUnitLengthSizeComesFromTheCodecConfiguration) {
    for (const bool hevc : {false, true}) {
        for (const std::uint8_t size : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{4}}) {
            Mp4TrackOptions options = track(Mp4TrackKind::Video, 6);
            options.hevc = hevc;
            options.nalLengthSize = size;
            Mp4Options file;
            file.tracks = {options, track(Mp4TrackKind::Audio, 6)};
            const Mp4Built built = test::makeMp4(file);
            const Mp4File parsed = parseBytes(built.bytes);
            ASSERT_TRUE(matchesBuilt(parsed, built)) << describe(parsed);
            const mp4::SampleDescription& description = parsed.movie->tracks[0].samples.descriptions[0];
            EXPECT_EQ(description.format.text(), hevc ? "hvc1" : "avc1");
            EXPECT_EQ(description.nalLengthSize, size);
            // Audio has none.
            EXPECT_EQ(parsed.movie->tracks[1].samples.descriptions[0].nalLengthSize, 0u);
        }
    }
    // A configuration of another version, or a length size of 3 bytes (not allowed): none.
    const Bytes file = test::makeMp4().bytes;
    const std::size_t config = box(file, "moov/trak/mdia/minf/stbl/stsd/avc1").offset + 8 + 78 + 8;
    const Mp4File version = parseBytes(overwritten(file, config, {0x02}));
    EXPECT_EQ(version.status, FileStatus::Valid);
    EXPECT_EQ(version.movie->tracks[0].samples.descriptions[0].nalLengthSize, 0u);
    const Mp4File three = parseBytes(overwritten(file, config + 4, {0xFE}));
    EXPECT_EQ(three.movie->tracks[0].samples.descriptions[0].nalLengthSize, 0u);
    // Without a configuration box.
    const Mp4File none = parseBytes(overwritten(file, config - 4, text("xxxx")));
    EXPECT_EQ(none.status, FileStatus::Valid);
    EXPECT_EQ(none.movie->tracks[0].samples.descriptions[0].nalLengthSize, 0u);
}

}  // namespace
}  // namespace recovery::formats
