// Carving whole images with every image format registered: files of all five
// formats planted in noise and carved back byte for byte, embedded
// thumbnails, files cut off by the end of the source, overwritten files,
// floods of false signatures, images in a FAT32 volume (active, deleted and
// fragmented), a disk image file, and a file whose structure never ends.

#include "formats/image_formats.hpp"

#include "formats/jpeg_format.hpp"

#include "carving/file_carver.hpp"
#include "carving/format_registry.hpp"
#include "carving/signature_scanner.hpp"
#include "format_test_helpers.hpp"
#include "storage/disk_image_source.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>

namespace recovery::formats {
namespace {

using carving::CarveOptions;
using carving::CarveWarning;
using carving::EndStatus;
using carving::FileCandidate;
using carving::ValidationStatus;
using testing::Bytes;
using testing::carveImages;
using testing::carvedBytes;
using testing::Carved;

// One file of every format, from the builders and from real encoders.
struct Planted {
    std::string formatId;
    std::string what;
    Bytes bytes;
};

std::vector<Planted> everyFormat() {
    test::JpegOptions jpeg;
    jpeg.width = 80;
    jpeg.height = 60;
    jpeg.restartInterval = 3;
    test::PngOptions png;
    png.width = 40;
    png.height = 30;
    png.idatSize = 300;
    test::GifOptions gif;
    gif.frames = 2;
    test::BmpOptions bmp;
    bmp.width = 33;
    bmp.height = 17;
    test::WebpOptions webp;
    webp.kind = test::WebpKind::LossyWithAlpha;
    return {
        {"jpeg", "a JPEG with restart markers", test::makeJpeg(jpeg)},
        {"png", "a PNG with several IDAT chunks", test::makePng(png)},
        {"webp", "an extended WebP with alpha", test::makeWebp(webp)},
        {"gif", "an animated GIF", test::makeGif(gif)},
        {"bmp", "a BMP", test::makeBmp(bmp)},
        {"jpeg", "cjpeg_progressive.jpg", test::samples::named("cjpeg_progressive.jpg").data()},
        {"png", "gdiplus_alpha.png", test::samples::named("gdiplus_alpha.png").data()},
        {"webp", "img2webp_animated.webp", test::samples::named("img2webp_animated.webp").data()},
        {"gif", "giflib.gif", test::samples::named("giflib.gif").data()},
        {"bmp", "djpeg_os2.bmp", test::samples::named("djpeg_os2.bmp").data()},
    };
}

TEST(ImageCarvingTest, RegistersEveryImageFormatInPriorityOrder) {
    carving::FormatRegistry registry;
    RECOVERY_ASSERT_OK(registerImageFormats(registry));
    ASSERT_EQ(registry.size(), 5u);
    const std::vector<std::string> expected = {"jpeg", "png", "webp", "gif", "bmp"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(registry.formats()[i]->descriptor().id, expected[i]);
        RECOVERY_EXPECT_OK(carving::validateDescriptor(registry.formats()[i]->descriptor()));
    }
    // Registering them twice fails and changes nothing.
    RECOVERY_EXPECT_ERROR(registerImageFormats(registry), ErrorCode::InvalidInput);
    EXPECT_EQ(registry.size(), 5u);
    // The formats are the same objects every call: they hold no state.
    EXPECT_EQ(imageFormats().size(), 5u);
}

TEST(ImageCarvingTest, CarvesEveryFormatFromANoisyDiskExactly) {
    const std::vector<Planted> files = everyFormat();
    test::VirtualSource source(4 * kMiB);
    source.setNoise(0x9E3779B9);
    std::vector<std::uint64_t> offsets;
    std::uint64_t offset = 4096;
    for (const Planted& file : files) {
        // Sector-aligned and unaligned starts alternate.
        offsets.push_back(offsets.size() % 2 == 0 ? offset : offset + 13);
        source.plant(offsets.back(), file.bytes);
        offset += 300 * 1024;
    }
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carveImages(source);
    std::size_t matched = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        SCOPED_TRACE(files[i].what);
        const auto found = std::find_if(carved.candidates.begin(), carved.candidates.end(),
                                        [&](const FileCandidate& c) { return c.sourceOffset == offsets[i]; });
        ASSERT_NE(found, carved.candidates.end());
        EXPECT_EQ(found->formatId, files[i].formatId) << testing::describe(*found);
        EXPECT_EQ(found->validation.status, ValidationStatus::Valid) << testing::describe(*found);
        EXPECT_EQ(found->end.status, EndStatus::Found);
        EXPECT_EQ(found->length, files[i].bytes.size());
        EXPECT_TRUE(found->warnings.empty());
        EXPECT_TRUE(carvedBytes(source, *found) == files[i].bytes);
        ++matched;
    }
    EXPECT_EQ(matched, files.size());
    // Nothing else validated: the noise holds no images.
    EXPECT_EQ(carved.report.count(ValidationStatus::Valid), files.size());
    EXPECT_EQ(carved.report.scan.hits,
              carved.report.candidates + carved.report.rejectedTotal() + carved.report.skippedInsideCandidates);
}

TEST(ImageCarvingTest, EmbeddedThumbnailsBelongToTheirFile) {
    test::JpegOptions options;
    options.exifThumbnail = true;
    const Bytes file = test::makeJpeg(options);
    test::VirtualSource source(1 * kMiB);
    source.plant(8192, file);
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carveImages(source);
    ASSERT_EQ(carved.candidates.size(), 1u);
    EXPECT_EQ(carved.candidates[0].sourceOffset, 8192u);
    EXPECT_EQ(carved.candidates[0].length, file.size());
    EXPECT_EQ(carved.candidates[0].validation.status, ValidationStatus::Valid);
    // The thumbnail's own SOI was found, and skipped as part of the file.
    EXPECT_GE(carved.report.scan.hits, 2u);
    EXPECT_GE(carved.report.skippedInsideCandidates, 1u);
}

TEST(ImageCarvingTest, FilesCutOffByTheEndOfTheSourceAreTruncated) {
    for (const Planted& file : everyFormat()) {
        SCOPED_TRACE(file.what);
        const std::uint64_t size = 64 * kKiB;
        const std::uint64_t start = size - file.bytes.size() / 2;
        test::VirtualSource source(size);
        source.plant(start, file.bytes);
        RECOVERY_ASSERT_OK(source.open());
        const Carved carved = carveImages(source);
        ASSERT_EQ(carved.candidates.size(), 1u) << carved.report.rejectedTotal();
        const FileCandidate& candidate = carved.candidates[0];
        SCOPED_TRACE(testing::describe(candidate));
        EXPECT_EQ(candidate.sourceOffset, start);
        EXPECT_EQ(candidate.end.status, EndStatus::Truncated);
        EXPECT_EQ(candidate.length, size - start);
        EXPECT_TRUE(candidate.hasWarning(CarveWarning::TruncatedBySourceEnd));
        EXPECT_EQ(candidate.validation.status, ValidationStatus::Truncated);
    }
}

TEST(ImageCarvingTest, AFileOverwrittenByAnotherIsKeptUpToTheBreak) {
    test::JpegOptions options;
    options.width = 200;
    options.height = 150;
    const Bytes jpeg = test::makeJpeg(options);
    const Bytes png = test::makePng();
    ASSERT_GT(jpeg.size(), 8192u);
    test::VirtualSource source(1 * kMiB);
    source.plant(4096, jpeg);
    // A new file takes the clusters of the second half of the old one.
    source.plant(4096 + 8192, png);
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carveImages(source);
    ASSERT_GE(carved.candidates.size(), 2u);
    const FileCandidate& first = carved.candidates[0];
    SCOPED_TRACE(testing::describe(first));
    EXPECT_EQ(first.formatId, "jpeg");
    EXPECT_NE(first.validation.status, ValidationStatus::Valid);
    EXPECT_LE(first.length, 8192u + png.size());
    const auto second = std::find_if(carved.candidates.begin(), carved.candidates.end(),
                                     [](const FileCandidate& c) { return c.formatId == "png"; });
    ASSERT_NE(second, carved.candidates.end());
    EXPECT_EQ(second->sourceOffset, 4096u + 8192u);
    EXPECT_EQ(second->validation.status, ValidationStatus::Valid);
    EXPECT_TRUE(carvedBytes(source, *second) == png);
}

TEST(ImageCarvingTest, AFloodOfFalseSignaturesGivesNoValidCandidates) {
    test::VirtualSource source(2 * kMiB);
    source.setNoise(12345);
    // Every format's signature, planted every few kilobytes with nothing behind it.
    const std::vector<Bytes> signatures = {
        Bytes{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0xE0}},
        Bytes{std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'}, std::byte{'\r'}, std::byte{'\n'},
              std::byte{0x1A}, std::byte{'\n'}},
        Bytes{std::byte{'R'}, std::byte{'I'}, std::byte{'F'}, std::byte{'F'}, std::byte{0x40}, std::byte{0},
              std::byte{0}, std::byte{0}, std::byte{'W'}, std::byte{'E'}, std::byte{'B'}, std::byte{'P'}},
        Bytes{std::byte{'G'}, std::byte{'I'}, std::byte{'F'}, std::byte{'8'}, std::byte{'9'}, std::byte{'a'}},
        Bytes{std::byte{'B'}, std::byte{'M'}},
    };
    std::uint64_t planted = 0;
    for (std::uint64_t offset = 1024; offset + 64 < 2 * kMiB; offset += 1024) {
        source.plant(offset, signatures[static_cast<std::size_t>(planted % signatures.size())]);
        ++planted;
    }
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carveImages(source);
    EXPECT_GE(carved.report.scan.hits, planted);
    EXPECT_EQ(carved.report.count(ValidationStatus::Valid), 0u);
    for (const FileCandidate& candidate : carved.candidates) {
        EXPECT_NE(candidate.validation.status, ValidationStatus::Valid) << testing::describe(candidate);
    }
    // Reading stays modest: a rejected hit costs one small read.
    EXPECT_LE(carved.report.carveBytesRead, planted * 64 * kKiB);
}

TEST(ImageCarvingTest, CarvesImagesFromAFat32VolumeIncludingDeletedOnes) {
    test::Fat32BuilderOptions volumeOptions;
    volumeOptions.sectorsPerCluster = 4;
    volumeOptions.clusterCount = 4096;
    test::Fat32ImageBuilder builder(volumeOptions);
    const auto dcim = builder.addDirectory(test::Fat32ImageBuilder::root(), "DCIM");
    const std::vector<Planted> files = everyFormat();
    std::vector<std::string> names;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string name = "IMG_" + std::to_string(1000 + i) + "." + files[i].formatId;
        const auto entry = builder.addFile(dcim.clusters.front(), name, files[i].bytes);
        names.push_back(name);
        if (i % 2 == 0) {
            builder.deleteEntry(entry);  // half of them are deleted, clusters freed
        }
    }
    const Bytes volume = builder.build();
    test::MemoryStorageSource source(volume);
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carveImages(source);
    // Every file, deleted or not, is found by its content alone.
    for (std::size_t i = 0; i < files.size(); ++i) {
        SCOPED_TRACE(names[i] + ": " + files[i].what);
        const auto found = std::find_if(carved.candidates.begin(), carved.candidates.end(),
                                        [&](const FileCandidate& c) {
                                            return c.formatId == files[i].formatId &&
                                                   c.length == files[i].bytes.size() &&
                                                   carvedBytes(source, c) == files[i].bytes;
                                        });
        EXPECT_NE(found, carved.candidates.end());
        if (found != carved.candidates.end()) {
            EXPECT_EQ(found->validation.status, ValidationStatus::Valid) << testing::describe(*found);
        }
    }
}

TEST(ImageCarvingTest, FragmentedFilesInAVolumeAreNotValid) {
    test::Fat32BuilderOptions volumeOptions;
    volumeOptions.sectorsPerCluster = 1;
    volumeOptions.clusterCount = 8192;
    test::Fat32ImageBuilder builder(volumeOptions);
    test::PngOptions options;
    options.width = 64;
    options.height = 64;
    options.idatSize = 400;
    const Bytes png = test::makePng(options);
    // The file's clusters are not in one piece: another file sits between them.
    const std::uint32_t clusterSize = builder.clusterSize();
    const auto needed = static_cast<std::uint32_t>((png.size() + clusterSize - 1) / clusterSize);
    ASSERT_GT(needed, 6u);
    std::vector<std::uint32_t> clusters;
    for (std::uint32_t i = 0; i < 3; ++i) {
        clusters.push_back(100 + i);
    }
    for (std::uint32_t i = 3; i < needed; ++i) {
        clusters.push_back(400 + i);
    }
    const auto fragmented = builder.addFileInClusters(test::Fat32ImageBuilder::root(), "frag.png", png, clusters);
    builder.addFileInClusters(test::Fat32ImageBuilder::root(), "other.bin",
                              test::makePattern(3 * clusterSize, 7), {103, 104, 105});
    builder.deleteEntry(fragmented);
    const Bytes volume = builder.build();
    test::MemoryStorageSource source(volume);
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carveImages(source);
    // The PNG is found, but carving it in one piece never gives the file back.
    const auto found = std::find_if(carved.candidates.begin(), carved.candidates.end(),
                                    [](const FileCandidate& c) { return c.formatId == "png"; });
    ASSERT_NE(found, carved.candidates.end());
    SCOPED_TRACE(testing::describe(*found));
    EXPECT_NE(found->validation.status, ValidationStatus::Valid);
    EXPECT_NE(carvedBytes(source, *found), png);
}

TEST(ImageCarvingTest, CarvesFromADiskImageFile) {
    const test::TempDir directory;
    const std::filesystem::path path = directory / "images.img";
    const std::vector<Planted> files = everyFormat();
    Bytes image(256 * kKiB, std::byte{0});
    std::vector<std::uint64_t> offsets;
    std::uint64_t offset = 512;
    for (const Planted& file : files) {
        offsets.push_back(offset);
        std::copy(file.bytes.begin(), file.bytes.end(), image.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += file.bytes.size() + 512 - (file.bytes.size() % 512);
    }
    test::writeFile(path, image);

    storage::DiskImageSource source(path);
    RECOVERY_ASSERT_OK(source.open());
    const Carved carved = carveImages(source);
    EXPECT_EQ(carved.report.count(ValidationStatus::Valid), files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        SCOPED_TRACE(files[i].what);
        const auto found = std::find_if(carved.candidates.begin(), carved.candidates.end(),
                                        [&](const FileCandidate& c) { return c.sourceOffset == offsets[i]; });
        ASSERT_NE(found, carved.candidates.end());
        EXPECT_EQ(found->length, files[i].bytes.size());
        EXPECT_TRUE(carvedBytes(source, *found) == files[i].bytes);
    }
}

TEST(ImageCarvingTest, AJpegWhoseScanNeverEndsStopsAtTheMaximumSize) {
    // Zeros are valid entropy-coded data, so a JPEG header followed by erased
    // space is carved up to the format's maximum size and no further (L61).
    test::JpegOptions options;
    options.width = 32;
    options.height = 32;
    const Bytes jpeg = test::makeJpeg(options);
    const auto markers = test::jpegMarkers(jpeg);
    const auto scan = std::find_if(markers.begin(), markers.end(),
                                   [](const test::JpegMarker& m) { return m.code == 0xDA; });
    ASSERT_NE(scan, markers.end());
    const std::size_t headerLength = scan->offset + 2 + scan->length;

    test::VirtualSource source(JpegFormat::kMaximumSize + 8 * kMiB);
    source.plant(4096, Bytes(jpeg.begin(), jpeg.begin() + static_cast<std::ptrdiff_t>(headerLength)));
    RECOVERY_ASSERT_OK(source.open());

    carving::FormatRegistry registry;
    RECOVERY_ASSERT_OK(registerImageFormats(registry));
    carving::SignatureHit hit;
    hit.format = registry.find("jpeg");
    hit.formatIndex = 0;
    hit.signatureIndex = 0;
    hit.fileOffset = 4096;
    carving::FileCarver carver(source);
    Result<carving::CarveOutcome> outcome = carver.carve(hit);
    RECOVERY_ASSERT_OK(outcome);
    const FileCandidate* candidate = std::get_if<FileCandidate>(&*outcome);
    ASSERT_NE(candidate, nullptr);
    SCOPED_TRACE(testing::describe(*candidate));
    EXPECT_EQ(candidate->end.status, EndStatus::Truncated);
    EXPECT_EQ(candidate->length, JpegFormat::kMaximumSize);
    EXPECT_TRUE(candidate->hasWarning(CarveWarning::TruncatedByMaximumSize));
    EXPECT_EQ(candidate->validation.status, ValidationStatus::Truncated);
    // Reading stopped at the maximum size, whatever the source still holds.
    EXPECT_LE(source.stats().highestEnd, 4096 + JpegFormat::kMaximumSize + 1);
}

}  // namespace
}  // namespace recovery::formats
