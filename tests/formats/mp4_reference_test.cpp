// Cross-checks of the MP4 parser (P11) against files this project did not
// write, and of the MP4 builder against demuxers this project did not write.
//
// Mp4SamplesTest runs always: every embedded sample (FFmpeg, GPAC's MP4Box,
// Media Foundation) must parse as Valid, with the tracks, codecs, sizes,
// sample counts and sample locations that FFmpeg's demuxer finds; its
// prefixes are never Invalid; fuzzing it never crashes the parser.
//
// Mp4ReferenceTest reads a directory of files made elsewhere (phones,
// cameras, screen recorders, video editors). It is skipped unless
// RECOVERY_MP4_REFERENCE_DIR names a directory of .mp4/.mov/.m4v/.m4a/.3gp
// files; see docs/testing/testing.md.
//
// Mp4BuilderExport writes the builder's files, with where each sample is, to
// RECOVERY_MP4_EXPORT_DIR (skipped otherwise), so that FFmpeg and GPAC can
// check them: tests/reference/check_mp4_builder_files.sh.

#include "formats/mp4_parser.hpp"

#include "format_test_helpers.hpp"
#include "formats/m4a_format.hpp"
#include "formats/mp4_analysis.hpp"
#include "formats/mp4_format.hpp"
#include "mp4_test_helpers.hpp"
#include "storage/disk_image_source.hpp"
#include "support/mp4_builders.hpp"
#include "support/mp4_samples.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

namespace recovery::formats {
namespace {

using mp4::FileStatus;
using mp4::Mp4File;
using testing::Bytes;
using testing::describe;
using testing::parseBytes;

std::uint64_t fnv1a(std::uint64_t hash, std::uint64_t value, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i) {
        hash ^= (value >> (8 * i)) & 0xFF;
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

// The hash embed_mp4_samples.py computes from ffprobe's packets.
std::uint64_t sampleLocations(const mp4::Track& track) {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    mp4::forEachSample(track, [&](std::uint32_t, std::uint64_t offset, std::uint32_t size) {
        hash = fnv1a(hash, offset, 8);
        hash = fnv1a(hash, size, 4);
    });
    return hash;
}

std::string_view kindName(mp4::TrackKind kind) {
    switch (kind) {
    case mp4::TrackKind::Video:
        return "video";
    case mp4::TrackKind::Audio:
        return "audio";
    case mp4::TrackKind::Other:
        break;
    }
    return "other";
}

// The lengths at which a prefix of the file ends at a top-level box boundary.
std::set<std::size_t> boxBoundaries(const Bytes& file) {
    std::set<std::size_t> ends;
    for (const test::BoxPosition& box : test::mp4Boxes(file)) {
        if (box.path.find('/') == std::string::npos) {
            ends.insert(box.offset + box.size);
        }
    }
    return ends;
}

TEST(Mp4SamplesTest, EverySampleParsesAsFfmpegReadsIt) {
    ASSERT_GE(test::mp4_samples::all().size(), 20u);
    for (const test::mp4_samples::Sample& sample : test::mp4_samples::all()) {
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Mp4File file = parseBytes(sample.data());
        ASSERT_EQ(file.status, FileStatus::Valid) << describe(file);
        ASSERT_TRUE(file.movie.has_value());
        EXPECT_EQ(file.movie->fragmented, sample.fragmented);
        ASSERT_EQ(file.movie->tracks.size(), sample.tracks.size());
        for (std::size_t t = 0; t < sample.tracks.size(); ++t) {
            SCOPED_TRACE("track " + std::to_string(t + 1));
            const mp4::Track& track = file.movie->tracks[t];
            const test::mp4_samples::Track& expected = sample.tracks[t];
            EXPECT_EQ(kindName(track.kind), expected.kind);
            ASSERT_TRUE(track.sampleTableValid);
            ASSERT_FALSE(track.samples.descriptions.empty());
            const mp4::SampleDescription& description = track.samples.descriptions[0];
            EXPECT_EQ(description.format.text(), expected.codec);
            EXPECT_EQ(description.width, expected.width);
            EXPECT_EQ(description.height, expected.height);
            EXPECT_EQ(description.channelCount, expected.channels);
            EXPECT_EQ(description.sampleRate, expected.sampleRate);
            // The samples of the sample tables and, since P12, of the movie fragments.
            EXPECT_EQ(track.totalSamples(), static_cast<std::uint64_t>(expected.samples));
            EXPECT_EQ(sampleLocations(track), expected.locations);
            EXPECT_EQ(!track.fragmentRuns.empty(), sample.fragmented && expected.samples > track.samples.sampleCount);
            for (const bool run : {false, true}) {
                for (const mp4::Chunk& chunk : run ? track.fragmentRuns : track.chunks) {
                    EXPECT_EQ(chunk.placement, mp4::ChunkPlacement::MediaData);
                }
            }
        }
    }
}

TEST(Mp4SamplesTest, PrefixesAreTruncatedOrCompleteAtABoxBoundary) {
    for (const test::mp4_samples::Sample& sample : test::mp4_samples::all()) {
        SCOPED_TRACE(sample.name);
        const Bytes file = sample.data();
        const std::set<std::size_t> boundaries = boxBoundaries(file);
        for (std::size_t length = 0; length < file.size(); length += length < 1024 ? 1 : 5) {
            const Mp4File parsed = parseBytes(std::span(file).first(length));
            if (parsed.status == FileStatus::Valid) {
                // A file cut after a whole box that nothing needs (GPAC's trailing free box, say) is complete.
                EXPECT_TRUE(boundaries.contains(length)) << length << ": " << describe(parsed);
            } else {
                ASSERT_EQ(parsed.status, FileStatus::Truncated) << length << ": " << describe(parsed);
            }
        }
    }
}

TEST(Mp4SamplesTest, Fuzzing) {
    std::uint64_t seed = 100;
    for (const test::mp4_samples::Sample& sample : test::mp4_samples::all()) {
        SCOPED_TRACE(sample.name);
        testing::fuzzParser(sample.data(), 300, seed++);
    }
}

TEST(Mp4ReferenceTest, ExternalFilesAreValid) {
    const std::optional<std::filesystem::path> directory =
        testing::directoryFromEnvironment(L"RECOVERY_MP4_REFERENCE_DIR");
    if (!directory) {
        GTEST_SKIP() << "set RECOVERY_MP4_REFERENCE_DIR to run against MP4 files made elsewhere";
    }
    static const std::set<std::string> kExtensions = {".mp4", ".mov", ".m4v", ".m4a", ".3gp"};
    std::size_t checked = 0;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(*directory, ec)) {
        std::string extension = item.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!item.is_regular_file() || !kExtensions.contains(extension)) {
            continue;
        }
        SCOPED_TRACE(item.path().filename().string());
        storage::DiskImageSource source(item.path());
        RECOVERY_ASSERT_OK(source.open());
        auto content = carving::SourceContentReader::open(source, 0, source.size());
        RECOVERY_ASSERT_OK(content);
        Result<Mp4File> parsed = mp4::parseFile(**content);
        RECOVERY_ASSERT_OK(parsed);
        EXPECT_EQ(parsed->status, FileStatus::Valid) << describe(*parsed);
        std::cout << item.path().filename().string() << ": " << parsed->detail << "\n";
        // P12: the format the file belongs to (MP4 or M4A) carves all of it and validates it.
        const mp4::Classification kind =
            mp4::classify(parsed->fileType, parsed->movie.has_value() ? &*parsed->movie : nullptr);
        ASSERT_NE(kind.kind, mp4::MediaKind::Neither) << kind.reason;
        const Mp4Format video;
        const M4aFormat audio;
        const carving::IFileFormat& format =
            kind.kind == mp4::MediaKind::Video ? static_cast<const carving::IFileFormat&>(video) : audio;
        if (parsed->fileType.has_value()) {
            auto whole = carving::SourceContentReader::open(source, 0, source.size());
            RECOVERY_ASSERT_OK(whole);
            Result<carving::EndDetection> end = format.findEnd(**whole);
            RECOVERY_ASSERT_OK(end);
            EXPECT_EQ(end->status, carving::EndStatus::Found) << testing::describe(*end);
            EXPECT_EQ(end->length, source.size()) << testing::describe(*end);
        }
        auto again = carving::SourceContentReader::open(source, 0, source.size());
        RECOVERY_ASSERT_OK(again);
        Result<carving::ValidationResult> verdict = format.validator().validate(**again);
        RECOVERY_ASSERT_OK(verdict);
        EXPECT_EQ(verdict->status, carving::ValidationStatus::Valid) << testing::describe(*verdict);
        std::cout << "  " << format.descriptor().id << ": " << verdict->detail << "\n";
        ++checked;
    }
    ASSERT_GT(checked, 0u) << "no MP4 files in " << directory->string();
    std::cout << "checked " << checked << " reference MP4 files\n";
}

TEST(Mp4BuilderExport, WritesFiles) {
    const std::optional<std::filesystem::path> directory =
        testing::directoryFromEnvironment(L"RECOVERY_MP4_EXPORT_DIR", false);
    if (!directory) {
        GTEST_SKIP() << "set RECOVERY_MP4_EXPORT_DIR to export the builder's files for other demuxers";
    }
    std::filesystem::create_directories(*directory);
    using test::Mp4MoovPlace;
    using test::Mp4Options;
    using test::Mp4TrackKind;
    using test::Mp4TrackOptions;
    const auto track = [](Mp4TrackKind kind, std::size_t samples, std::size_t perChunk) {
        Mp4TrackOptions options;
        options.kind = kind;
        options.samples = samples;
        options.samplesPerChunk = perChunk;
        return options;
    };
    std::vector<std::pair<std::string, Mp4Options>> files;
    files.emplace_back("builder_default", Mp4Options{});
    Mp4Options options;
    options.moov = Mp4MoovPlace::First;
    files.emplace_back("builder_moov_first", options);
    options = {};
    options.moov = Mp4MoovPlace::Between;
    options.mediaDataBoxes = 3;
    options.wideBox = true;
    options.tracks = {track(Mp4TrackKind::Video, 20, 3), track(Mp4TrackKind::Audio, 30, 4),
                      track(Mp4TrackKind::Text, 4, 1)};
    files.emplace_back("builder_three_mdat", options);
    options = {};
    options.largeMediaData = true;
    options.largeMoov = true;
    options.largeNested = true;
    options.co64 = true;
    files.emplace_back("builder_64bit", options);
    options = {};
    options.reverseChildren = true;
    options.extraBoxes = true;
    options.interleave = false;
    files.emplace_back("builder_reversed", options);
    options = {};
    Mp4TrackOptions compact = track(Mp4TrackKind::Audio, 25, 5);
    compact.compactSizes = true;
    Mp4TrackOptions uniform = track(Mp4TrackKind::Video, 9, 2);
    uniform.uniformSize = true;
    options.tracks = {compact, uniform};
    options.movieVersion1 = true;
    files.emplace_back("builder_compact_sizes", options);
    options = {};
    options.majorBrand.clear();
    options.moov = Mp4MoovPlace::First;
    options.mediaDataToEnd = true;
    files.emplace_back("builder_no_ftyp", options);

    for (const auto& [name, fileOptions] : files) {
        const test::Mp4Built built = test::makeMp4(fileOptions);
        ASSERT_TRUE(testing::matchesBuilt(parseBytes(built.bytes), built)) << name;
        test::writeFile(*directory / (name + ".mp4"), built.bytes);
        // One line per sample: track index (from 0), offset, size.
        std::ofstream samples(*directory / (name + ".samples"), std::ios::binary | std::ios::trunc);
        for (std::size_t t = 0; t < built.samples.size(); ++t) {
            for (const test::Mp4Sample& sample : built.samples[t]) {
                samples << t << ' ' << sample.offset << ' ' << sample.size << '\n';
            }
        }
    }
    std::cout << "wrote " << files.size() << " files to " << directory->string() << "\n";
}

}  // namespace
}  // namespace recovery::formats
