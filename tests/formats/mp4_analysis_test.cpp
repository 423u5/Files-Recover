// The MP4 analysis of P12 (formats/mp4_analysis.hpp): which brands and
// tracks make a file audio (M4A) or video (MP4); the media extent of the
// sample tables and fragments; moov discovery by searching the data; and the
// framing of AVC and HEVC samples (NAL units filling each sample), on builder
// files, damaged samples, truncated files and fragments.

#include "formats/mp4_analysis.hpp"

#include "format_test_helpers.hpp"
#include "mp4_test_helpers.hpp"
#include "support/mp4_builders.hpp"
#include "support/mp4_samples.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <string>

namespace recovery::formats {
namespace {

using mp4::BrandClass;
using mp4::FourCc;
using mp4::MediaKind;
using mp4::TrackKind;
using test::Mp4Built;
using test::Mp4MoovPlace;
using test::Mp4Options;
using test::Mp4TrackKind;
using test::Mp4TrackOptions;
using testing::Bytes;
using testing::overwritten;
using testing::parseBytes;

Mp4TrackOptions track(Mp4TrackKind kind, std::size_t samples = 12, std::size_t perChunk = 4) {
    Mp4TrackOptions options;
    options.kind = kind;
    options.samples = samples;
    options.samplesPerChunk = perChunk;
    return options;
}

mp4::Movie movieOf(std::initializer_list<TrackKind> kinds) {
    mp4::Movie movie;
    for (const TrackKind kind : kinds) {
        mp4::Track track;
        track.kind = kind;
        movie.tracks.push_back(track);
    }
    return movie;
}

std::optional<mp4::FileType> brand(std::uint32_t value) {
    return mp4::FileType{FourCc(value), 0, {}};
}

std::uint32_t code(std::string_view text) {
    std::uint32_t value = 0;
    for (const char c : text) {
        value = (value << 8) | static_cast<std::uint8_t>(c);
    }
    return value;
}

Bytes be32(std::uint64_t value) {
    return Bytes{static_cast<std::byte>((value >> 24) & 0xFF), static_cast<std::byte>((value >> 16) & 0xFF),
                 static_cast<std::byte>((value >> 8) & 0xFF), static_cast<std::byte>(value & 0xFF)};
}

mp4::FramingCheck framingOf(const Bytes& file) {
    const mp4::Mp4File parsed = parseBytes(file);
    EXPECT_TRUE(parsed.movie.has_value()) << testing::describe(parsed);
    if (!parsed.movie.has_value()) {
        return {};
    }
    carving::MemoryContentReader content(file);
    Result<mp4::FramingCheck> check = mp4::checkSampleFraming(content, *parsed.movie);
    EXPECT_TRUE(check.ok());
    return check.ok() ? *check : mp4::FramingCheck{};
}

TEST(Mp4AnalysisTest, BrandsAreClassified) {
    for (const std::string_view audio : {"M4A ", "M4B ", "M4P ", "F4A ", "F4B "}) {
        EXPECT_EQ(mp4::classifyBrand(FourCc(code(audio))), BrandClass::Audio) << audio;
    }
    for (const std::string_view video : {"M4V ", "M4VH", "M4VP", "F4V ", "F4P ", "qt  "}) {
        EXPECT_EQ(mp4::classifyBrand(FourCc(code(video))), BrandClass::Video) << video;
    }
    for (const std::string_view image :
         {"heic", "heix", "heim", "heis", "hevc", "hevx", "mif1", "msf1", "avif", "avis", "crx ", "jp2 ", "mjp2"}) {
        EXPECT_EQ(mp4::classifyBrand(FourCc(code(image))), BrandClass::Image) << image;
    }
    for (const std::string_view generic : {"isom", "iso2", "iso5", "mp41", "mp42", "avc1", "3gp4", "3gp6", "3g2a",
                                           "dash", "MSNV", "XAVC"}) {
        EXPECT_EQ(mp4::classifyBrand(FourCc(code(generic))), BrandClass::Generic) << generic;
    }
}

TEST(Mp4AnalysisTest, TheBrandOrTheTracksTellAudioFromVideo) {
    const mp4::Movie soundOnly = movieOf({TrackKind::Audio, TrackKind::Other});
    const mp4::Movie both = movieOf({TrackKind::Video, TrackKind::Audio});
    const mp4::Movie videoOnly = movieOf({TrackKind::Video});
    const mp4::Movie textOnly = movieOf({TrackKind::Other});
    // The brand decides when it is an audio, video or image brand.
    EXPECT_EQ(mp4::classify(brand(code("M4A ")), &both).kind, MediaKind::Audio);
    EXPECT_EQ(mp4::classify(brand(code("M4B ")), nullptr).kind, MediaKind::Audio);
    EXPECT_EQ(mp4::classify(brand(code("qt  ")), &soundOnly).kind, MediaKind::Video);
    EXPECT_EQ(mp4::classify(brand(code("M4V ")), nullptr).kind, MediaKind::Video);
    EXPECT_EQ(mp4::classify(brand(code("heic")), &videoOnly).kind, MediaKind::Neither);
    EXPECT_EQ(mp4::classify(brand(0x00010203), &videoOnly).kind, MediaKind::Neither);
    // A generic brand (or none): the tracks.
    for (const std::optional<mp4::FileType>& generic : {brand(code("isom")), brand(code("3gp4")),
                                                        std::optional<mp4::FileType>{}}) {
        EXPECT_EQ(mp4::classify(generic, &soundOnly).kind, MediaKind::Audio);
        EXPECT_EQ(mp4::classify(generic, &both).kind, MediaKind::Video);
        EXPECT_EQ(mp4::classify(generic, &videoOnly).kind, MediaKind::Video);
        EXPECT_EQ(mp4::classify(generic, &textOnly).kind, MediaKind::Video);
        const mp4::Classification lost = mp4::classify(generic, nullptr);
        EXPECT_EQ(lost.kind, MediaKind::Video);
        EXPECT_NE(lost.reason.find("without a moov box"), std::string::npos) << lost.reason;
    }
}

TEST(Mp4AnalysisTest, EveryEmbeddedSampleIsVideoOrAudio) {
    for (const test::mp4_samples::Sample& sample : test::mp4_samples::all()) {
        const mp4::Mp4File file = parseBytes(sample.data());
        const mp4::Classification kind = mp4::classify(file.fileType, file.movie.has_value() ? &*file.movie : nullptr);
        bool video = false;
        for (const test::mp4_samples::Track& t : sample.tracks) {
            video = video || t.kind == "video";
        }
        EXPECT_EQ(kind.kind, video ? MediaKind::Video : MediaKind::Audio) << sample.name << ": " << kind.reason;
    }
}

TEST(Mp4AnalysisTest, MediaExtentSpansEveryChunkAndRun) {
    Mp4Options options;
    options.moov = Mp4MoovPlace::Last;
    const Mp4Built built = test::makeMp4(options);
    const mp4::Mp4File file = parseBytes(built.bytes);
    const std::optional<mp4::MediaExtent> extent = mp4::mediaExtent(*file.movie);
    ASSERT_TRUE(extent.has_value());
    const test::BoxPosition mdat = test::findBox(test::mp4Boxes(built.bytes), "mdat");
    EXPECT_EQ(extent->begin, mdat.offset + 8);
    EXPECT_EQ(extent->end, mdat.offset + mdat.size);
    EXPECT_EQ(extent->bytes, mdat.size - 8);
    EXPECT_EQ(extent->samples, built.samples[0].size() + built.samples[1].size());

    Mp4Options fragments;
    fragments.moov = Mp4MoovPlace::First;
    fragments.fragmentSamples = 5;
    const Mp4Built split = test::makeMp4(fragments);
    const mp4::Mp4File parsed = parseBytes(split.bytes);
    const std::optional<mp4::MediaExtent> runs = mp4::mediaExtent(*parsed.movie);
    ASSERT_TRUE(runs.has_value());
    EXPECT_EQ(runs->end, split.bytes.size());  // the last run ends the file
    EXPECT_EQ(runs->samples, split.samples[0].size() + split.samples[1].size());

    // A movie without samples has no media extent.
    EXPECT_FALSE(mp4::mediaExtent(movieOf({TrackKind::Video})).has_value());
}

TEST(Mp4AnalysisTest, FindMovieSearchesTheData) {
    Mp4Options options;
    options.moov = Mp4MoovPlace::Last;
    const Mp4Built built = test::makeMp4(options);
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(built.bytes);
    const test::BoxPosition moov = test::findBox(boxes, "moov");
    // The first bytes overwritten: the top-level boxes no longer lead to moov.
    const Bytes damaged = overwritten(built.bytes, 0, testing::noise(64, 3));
    EXPECT_FALSE(parseBytes(damaged).movie.has_value());
    carving::MemoryContentReader content(damaged);
    Result<std::optional<mp4::FoundMovie>> found = mp4::findMovie(content, 0, damaged.size());
    RECOVERY_ASSERT_OK(found);
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->box.offset, moov.offset);
    EXPECT_EQ((*found)->box.size, moov.size);
    ASSERT_EQ((*found)->movie.tracks.size(), 2u);
    EXPECT_TRUE((*found)->movie.tracks[0].sampleTableValid);

    // Something that says "moov" in the media data before it is not taken.
    const std::size_t sample = built.samples[0][1].offset;
    Bytes decoy = overwritten(damaged, sample, be32(64));
    decoy = overwritten(decoy, sample + 4, {'m', 'o', 'o', 'v', 0, 0, 0, 8, 'f', 'r', 'e', 'e'});
    carving::MemoryContentReader withDecoy(decoy);
    Result<std::optional<mp4::FoundMovie>> real = mp4::findMovie(withDecoy, 0, decoy.size());
    RECOVERY_ASSERT_OK(real);
    ASSERT_TRUE(real->has_value());
    EXPECT_EQ((*real)->box.offset, moov.offset);
    // ...unless the attempts run out on it first.
    Result<std::optional<mp4::FoundMovie>> once = mp4::findMovie(withDecoy, 0, decoy.size(), {}, 1);
    RECOVERY_ASSERT_OK(once);
    EXPECT_FALSE(once->has_value());

    // A range that does not hold all of moov, and invalid limits.
    Result<std::optional<mp4::FoundMovie>> cut = mp4::findMovie(content, 0, moov.offset + moov.size - 1);
    RECOVERY_ASSERT_OK(cut);
    EXPECT_FALSE(cut->has_value());
    Result<std::optional<mp4::FoundMovie>> after = mp4::findMovie(content, moov.offset + 1, damaged.size());
    RECOVERY_ASSERT_OK(after);
    EXPECT_FALSE(after->has_value());
    mp4::ParseLimits zero;
    zero.maxBoxes = 0;
    RECOVERY_EXPECT_ERROR(mp4::findMovie(content, 0, damaged.size(), zero), ErrorCode::InvalidInput);
}

TEST(Mp4AnalysisTest, BuilderSamplesAreFramed) {
    for (const bool hevc : {false, true}) {
        for (const std::uint8_t size : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{4}}) {
            Mp4TrackOptions video = track(Mp4TrackKind::Video, 15);
            video.hevc = hevc;
            video.nalLengthSize = size;
            Mp4Options options;
            options.tracks = {video, track(Mp4TrackKind::Audio, 9), track(Mp4TrackKind::Text, 3)};
            const mp4::FramingCheck check = framingOf(test::makeMp4(options).bytes);
            ASSERT_EQ(check.tracks.size(), 1u);  // audio and text have no NAL units
            EXPECT_EQ(check.tracks[0].track, 1u);
            EXPECT_EQ(check.checked(), 15u);
            EXPECT_EQ(check.bad(), 0u) << check.describeFirstBad();
            EXPECT_TRUE(check.describeFirstBad().empty());
        }
    }
    // Movie fragments: every sample of the runs.
    Mp4Options fragments;
    fragments.moov = Mp4MoovPlace::First;
    fragments.fragmentSamples = 4;
    fragments.samplesInMoov = 2;
    const mp4::FramingCheck check = framingOf(test::makeMp4(fragments).bytes);
    EXPECT_EQ(check.checked(), 12u);
    EXPECT_EQ(check.bad(), 0u) << check.describeFirstBad();
}

TEST(Mp4AnalysisTest, FramingCatchesSamplesThatAreNotTheirOwn) {
    const Mp4Built built = test::makeMp4();
    const test::Mp4Sample& sample = built.samples[0][5];
    const auto expectBad = [&](const Bytes& damaged, std::string_view why) {
        const mp4::FramingCheck check = framingOf(damaged);
        ASSERT_EQ(check.tracks.size(), 1u);
        EXPECT_EQ(check.bad(), 1u) << why;
        EXPECT_EQ(check.tracks[0].firstBadSample, 5u) << why;
        EXPECT_EQ(check.tracks[0].firstBadOffset, sample.offset) << why;
        EXPECT_NE(check.describeFirstBad().find(why), std::string::npos) << check.describeFirstBad();
        EXPECT_NE(check.describeFirstBad().find("sample 6 of track 1"), std::string::npos);
    };
    expectBad(overwritten(built.bytes, sample.offset, be32(0)), "length 0");
    expectBad(overwritten(built.bytes, sample.offset, be32(0x7FFFFFFF)), "runs past");
    expectBad(overwritten(built.bytes, sample.offset + 4, {0x80}), "forbidden zero bit");
    expectBad(overwritten(built.bytes, sample.offset, be32(sample.size - 4 - 3)), "too few for a NAL unit");
    // Another file's data over the sample (noise without a plausible length field).
    Bytes foreign = testing::noise(sample.size, 5);
    foreign[0] = std::byte{0x7F};
    expectBad(overwritten(built.bytes, sample.offset, foreign), "NAL unit");
    // Samples the data does not hold are not checked.
    Mp4Options first;
    first.moov = Mp4MoovPlace::First;
    const Mp4Built fast = test::makeMp4(first);
    const Bytes cut = testing::prefix(fast.bytes, fast.samples[0][7].offset + 10);
    const mp4::FramingCheck partial = framingOf(cut);
    EXPECT_LT(partial.checked(), 12u);
    EXPECT_GT(partial.checked(), 0u);
    EXPECT_EQ(partial.bad(), 0u) << partial.describeFirstBad();
}

}  // namespace
}  // namespace recovery::formats
