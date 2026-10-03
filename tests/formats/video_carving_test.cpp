// Carving whole sources with the MP4 format (P12) registered next to the
// image and audio formats: MP4 files of every layout and from every writer
// planted in noise and carved back byte for byte, each by exactly one of MP4
// and M4A; a file larger than 4 GiB beyond 4 GiB; files cut off by the end of
// the source; an MP4 whose moov was overwritten; two MP4s back to back; a
// motion photo (a JPEG with an MP4 after it); MP4s in a FAT32 volume, deleted
// and fragmented; a disk image file.

#include "formats/video_formats.hpp"

#include "carving/format_registry.hpp"
#include "format_test_helpers.hpp"
#include "formats/audio_formats.hpp"
#include "formats/image_formats.hpp"
#include "storage/disk_image_source.hpp"
#include "support/audio_builders.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/memory_source.hpp"
#include "support/mp4_builders.hpp"
#include "support/mp4_samples.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace recovery::formats {
namespace {

using carving::CarveWarning;
using carving::EndStatus;
using carving::FileCandidate;
using carving::ValidationStatus;
using test::Mp4FragmentBase;
using test::Mp4MoovPlace;
using test::Mp4Options;
using testing::Bytes;
using testing::carve;
using testing::carvedBytes;
using testing::Carved;
using testing::Formats;

struct Planted {
    std::string formatId;
    std::string what;
    Bytes bytes;
};

std::vector<Planted> isoFiles() {
    std::vector<Planted> files;
    files.push_back({"mp4", "an MP4 with moov last", test::makeMp4().bytes});
    Mp4Options options;
    options.moov = Mp4MoovPlace::First;
    options.largeMediaData = true;
    options.co64 = true;
    files.push_back({"mp4", "an MP4 with moov first and 64-bit sizes", test::makeMp4(options).bytes});
    options = {};
    options.moov = Mp4MoovPlace::First;
    options.fragmentSamples = 5;
    options.fragmentBase = Mp4FragmentBase::Implicit;
    files.push_back({"mp4", "a fragmented MP4", test::makeMp4(options).bytes});
    options = {};
    options.majorBrand = "qt  ";
    options.compatibleBrands = {"qt  "};
    files.push_back({"mp4", "a QuickTime movie", test::makeMp4(options).bytes});
    files.push_back({"m4a", "an M4A", test::makeM4a()});
    for (const test::mp4_samples::Sample& sample : test::mp4_samples::all()) {
        const bool video = std::any_of(sample.tracks.begin(), sample.tracks.end(),
                                       [](const test::mp4_samples::Track& t) { return t.kind == "video"; });
        files.push_back({video ? "mp4" : "m4a", std::string(sample.name), sample.data()});
    }
    return files;
}

const FileCandidate* at(const Carved& carved, std::uint64_t offset, std::string_view format) {
    const auto found = std::find_if(carved.candidates.begin(), carved.candidates.end(), [&](const FileCandidate& c) {
        return c.sourceOffset == offset && c.formatId == format;
    });
    return found == carved.candidates.end() ? nullptr : &*found;
}

TEST(VideoCarvingTest, RegistersTheVideoFormat) {
    carving::FormatRegistry registry;
    RECOVERY_ASSERT_OK(registerVideoFormats(registry));
    ASSERT_EQ(registry.size(), 1u);
    EXPECT_EQ(registry.formats()[0]->descriptor().id, "mp4");
    RECOVERY_EXPECT_ERROR(registerVideoFormats(registry), ErrorCode::InvalidInput);
    // Images, audio and video side by side: no id is taken twice.
    RECOVERY_ASSERT_OK(registerImageFormats(registry));
    RECOVERY_ASSERT_OK(registerAudioFormats(registry));
    EXPECT_EQ(registry.size(), 10u);
}

TEST(VideoCarvingTest, CarvesEveryMp4AndM4aExactlyOnce) {
    const std::vector<Planted> files = isoFiles();
    test::VirtualSource source(16 * kMiB);
    source.setNoise(0x51C0FFEE);
    std::vector<std::uint64_t> offsets;
    std::uint64_t offset = 4096;
    for (const Planted& file : files) {
        offsets.push_back(offsets.size() % 2 == 0 ? offset : offset + 7);
        source.plant(offsets.back(), file.bytes);
        offset += 256 * 1024;
    }
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carve(source, Formats::Everything);
    for (std::size_t i = 0; i < files.size(); ++i) {
        SCOPED_TRACE(files[i].what);
        const FileCandidate* found = at(carved, offsets[i], files[i].formatId);
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->validation.status, ValidationStatus::Valid) << testing::describe(*found);
        EXPECT_EQ(found->end.status, EndStatus::Found);
        EXPECT_EQ(found->length, files[i].bytes.size());
        EXPECT_TRUE(found->warnings.empty());
        EXPECT_TRUE(carvedBytes(source, *found) == files[i].bytes);
        // The other ISO format does not claim it.
        EXPECT_EQ(at(carved, offsets[i], files[i].formatId == "mp4" ? "m4a" : "mp4"), nullptr);
    }
    EXPECT_EQ(carved.report.count(ValidationStatus::Valid), files.size());
    EXPECT_EQ(carved.report.scan.hits,
              carved.report.candidates + carved.report.rejectedTotal() + carved.report.skippedInsideCandidates);
}

TEST(VideoCarvingTest, AFileLargerThan4GiBBeyond4GiB) {
    Mp4Options options;
    options.moov = Mp4MoovPlace::First;
    options.largeMediaData = true;
    options.co64 = true;
    options.largeMoov = true;
    const test::Mp4Built built = test::makeMp4(options);
    const test::SpreadMp4 spread = test::spreadMp4(built, 5 * kGiB);
    const std::uint64_t base = 4 * kGiB + 12345 * 512;
    test::VirtualSource source(base + spread.size() + 64 * kKiB);
    source.setNoise(99);
    source.plant(base, spread.head);
    source.plant(base + spread.mediaOffset(), spread.media);
    RECOVERY_ASSERT_OK(source.open());

    carving::CarveOptions carveOptions;
    carveOptions.scan.startOffset = base - 64 * kKiB;
    carveOptions.scan.endOffset = base + 64 * kKiB;
    const Carved carved = carve(source, Formats::Video, carveOptions);
    const FileCandidate* found = at(carved, base, "mp4");
    ASSERT_NE(found, nullptr);
    SCOPED_TRACE(testing::describe(*found));
    EXPECT_EQ(found->length, spread.size());
    EXPECT_GT(found->length, 4 * kGiB);
    EXPECT_EQ(found->end.status, EndStatus::Found);
    EXPECT_EQ(found->validation.status, ValidationStatus::Valid);
    // Every sample is where the sample tables say, beyond 9 GiB on the source.
    for (std::size_t t = 0; t < spread.samples.size(); ++t) {
        for (std::size_t s = 0; s < spread.samples[t].size(); s += 5) {
            const test::Mp4Sample& sample = spread.samples[t][s];
            const test::Mp4Sample& original = built.samples[t][s];
            const Bytes expected(built.bytes.begin() + static_cast<std::ptrdiff_t>(original.offset),
                                 built.bytes.begin() + static_cast<std::ptrdiff_t>(original.offset + original.size));
            EXPECT_EQ(source.contentAt(base + sample.offset, sample.size), expected);
        }
    }
    // The carve read the boxes and the samples, not the gap.
    EXPECT_LT(carved.report.carveBytesRead, 64 * kMiB);
}

TEST(VideoCarvingTest, FilesCutOffByTheEndOfTheSourceAreTruncated) {
    Mp4Options first;
    first.moov = Mp4MoovPlace::First;
    Mp4Options fragments;
    fragments.moov = Mp4MoovPlace::First;
    fragments.fragmentSamples = 3;
    const std::vector<Planted> files = {{"mp4", "moov last", test::makeMp4().bytes},
                                        {"mp4", "moov first", test::makeMp4(first).bytes},
                                        {"mp4", "fragments", test::makeMp4(fragments).bytes}};
    for (const Planted& file : files) {
        SCOPED_TRACE(file.what);
        const std::uint64_t size = 64 * kKiB;
        const std::uint64_t kept = file.bytes.size() * 2 / 3;
        test::VirtualSource source(size);
        source.plant(size - kept, file.bytes);
        RECOVERY_ASSERT_OK(source.open());
        const Carved carved = carve(source, Formats::Video);
        const FileCandidate* candidate = at(carved, size - kept, "mp4");
        ASSERT_NE(candidate, nullptr);
        SCOPED_TRACE(testing::describe(*candidate));
        EXPECT_EQ(candidate->end.status, EndStatus::Truncated);
        EXPECT_EQ(candidate->length, kept);
        EXPECT_TRUE(candidate->hasWarning(CarveWarning::TruncatedBySourceEnd));
        EXPECT_EQ(candidate->validation.status, ValidationStatus::Truncated);
    }
}

TEST(VideoCarvingTest, AnMp4WhoseMoovWasOverwrittenIsKeptUpToTheBreak) {
    const test::Mp4Built built = test::makeMp4();  // moov after mdat
    const std::size_t moov = test::findBox(test::mp4Boxes(built.bytes), "moov").offset;
    const Bytes wav = test::makeWav();
    test::VirtualSource source(1 * kMiB);
    source.plant(4096, built.bytes);
    source.plant(4096 + moov, wav);  // a new file took the clusters that held moov
    RECOVERY_ASSERT_OK(source.open());
    const Carved carved = carve(source, Formats::Everything);
    const FileCandidate* video = at(carved, 4096, "mp4");
    ASSERT_NE(video, nullptr);
    SCOPED_TRACE(testing::describe(*video));
    EXPECT_EQ(video->end.status, EndStatus::Broken);
    EXPECT_EQ(video->length, moov);
    EXPECT_NE(video->validation.status, ValidationStatus::Valid);
    const FileCandidate* audio = at(carved, 4096 + moov, "wav");
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->validation.status, ValidationStatus::Valid);
}

TEST(VideoCarvingTest, TwoFilesBackToBackAreTwoCandidates) {
    const Bytes first = test::makeMp4().bytes;
    Mp4Options options;
    options.moov = Mp4MoovPlace::First;
    options.seed = 5;
    const Bytes second = test::makeMp4(options).bytes;
    test::VirtualSource source(512 * kKiB);
    source.plant(4096, testing::concat({first, second}));
    RECOVERY_ASSERT_OK(source.open());
    const Carved carved = carve(source, Formats::Video);
    ASSERT_EQ(carved.candidates.size(), 2u);
    EXPECT_EQ(carved.candidates[0].length, first.size());
    EXPECT_EQ(carved.candidates[1].sourceOffset, 4096 + first.size());
    EXPECT_EQ(carved.candidates[1].length, second.size());
    for (const FileCandidate& candidate : carved.candidates) {
        EXPECT_EQ(candidate.validation.status, ValidationStatus::Valid) << testing::describe(candidate);
    }
}

TEST(VideoCarvingTest, AMotionPhotoHoldsAPictureAndAVideo) {
    // Phones append the video of a motion photo to the JPEG, after its EOI.
    const Bytes jpeg = test::makeJpeg();
    const Bytes video = test::makeMp4().bytes;
    test::VirtualSource source(512 * kKiB);
    source.plant(8192, testing::concat({jpeg, video}));
    RECOVERY_ASSERT_OK(source.open());
    const Carved carved = carve(source, Formats::Everything);
    const FileCandidate* picture = at(carved, 8192, "jpeg");
    ASSERT_NE(picture, nullptr);
    EXPECT_EQ(picture->length, jpeg.size());
    const FileCandidate* movie = at(carved, 8192 + jpeg.size(), "mp4");
    ASSERT_NE(movie, nullptr);
    EXPECT_EQ(movie->validation.status, ValidationStatus::Valid);
    EXPECT_TRUE(carvedBytes(source, *movie) == video);
}

TEST(VideoCarvingTest, Mp4InAFat32VolumeDeletedAndFragmented) {
    test::Fat32BuilderOptions volumeOptions;
    volumeOptions.sectorsPerCluster = 1;
    volumeOptions.clusterCount = 8192;
    test::Fat32ImageBuilder builder(volumeOptions);
    const std::uint32_t clusterSize = builder.clusterSize();
    const Bytes whole = test::makeMp4().bytes;
    Mp4Options first;
    first.moov = Mp4MoovPlace::First;
    first.seed = 8;
    const Bytes split = test::makeMp4(first).bytes;
    const auto deleted = builder.addFile(test::Fat32ImageBuilder::root(), "CLIP0001.MP4", whole);
    builder.deleteEntry(deleted);
    // The second file's clusters are not in one piece: another file sits between them.
    const auto needed = static_cast<std::uint32_t>((split.size() + clusterSize - 1) / clusterSize);
    std::vector<std::uint32_t> clusters;
    for (std::uint32_t i = 0; i < needed; ++i) {
        clusters.push_back(i < 4 ? 3000 + i : 3100 + i);
    }
    const auto fragmented = builder.addFileInClusters(test::Fat32ImageBuilder::root(), "CLIP0002.MP4", split, clusters);
    builder.addFileInClusters(test::Fat32ImageBuilder::root(), "GAP.BIN", testing::noise(3 * clusterSize, 5),
                              {3004, 3005, 3006});
    builder.deleteEntry(fragmented);
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carve(source, Formats::Video);
    const auto found = std::find_if(carved.candidates.begin(), carved.candidates.end(),
                                    [&](const FileCandidate& c) { return carvedBytes(source, c) == whole; });
    ASSERT_NE(found, carved.candidates.end());
    EXPECT_EQ(found->validation.status, ValidationStatus::Valid);
    // The fragmented one is found, and its structure says it is not whole (P13 reassembles it).
    const std::uint64_t start = builder.clusterOffset(3000);
    const FileCandidate* piece = at(carved, start, "mp4");
    ASSERT_NE(piece, nullptr);
    EXPECT_NE(piece->validation.status, ValidationStatus::Valid) << testing::describe(*piece);
}

TEST(VideoCarvingTest, CarvesFromADiskImageFile) {
    const test::TempDir directory;
    const std::filesystem::path path = directory / "video.img";
    const std::vector<Planted> files = isoFiles();
    Bytes image(8 * kMiB, std::byte{0});
    std::vector<std::uint64_t> offsets;
    std::uint64_t offset = 512;
    for (const Planted& file : files) {
        offsets.push_back(offset);
        std::copy(file.bytes.begin(), file.bytes.end(), image.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += file.bytes.size() + 512 - (file.bytes.size() % 512);
    }
    ASSERT_LE(offset, image.size());
    test::writeFile(path, image);

    storage::DiskImageSource source(path);
    RECOVERY_ASSERT_OK(source.open());
    const Carved carved = carve(source, Formats::Everything);
    for (std::size_t i = 0; i < files.size(); ++i) {
        SCOPED_TRACE(files[i].what);
        const FileCandidate* found = at(carved, offsets[i], files[i].formatId);
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->validation.status, ValidationStatus::Valid);
        EXPECT_TRUE(carvedBytes(source, *found) == files[i].bytes);
    }
}

}  // namespace
}  // namespace recovery::formats
