// Cross-checks against files this project did not write.
//
// ImageSamplesTest runs always: every embedded sample (libjpeg-turbo, libwebp,
// giflib, GDI+) must be recognised, validated and carved back byte for byte.
//
// ImageReferenceTest reads a directory of images made elsewhere, for a corpus
// larger than the embedded samples (photos from a camera, files from other
// tools). It is skipped unless RECOVERY_IMAGE_REFERENCE_DIR names a directory
// of .jpg/.jpeg/.png/.webp/.gif/.bmp files; see docs/testing/testing.md.
//
// ImageBuilderExport writes the test builders' own files to
// RECOVERY_IMAGE_EXPORT_DIR (skipped otherwise), so that independent decoders
// can check them: tests/reference/check_image_builder_files.sh.

#include "formats/image_formats.hpp"

#include "format_test_helpers.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::FileCandidate;
using carving::IFileFormat;
using carving::ValidationStatus;
using testing::Bytes;
using testing::carveImages;
using testing::carvedBytes;
using testing::Carved;

const IFileFormat& formatWithId(std::string_view id) {
    static const std::vector<std::shared_ptr<const IFileFormat>> formats = imageFormats();
    for (const auto& format : formats) {
        if (format->descriptor().id == id) {
            return *format;
        }
    }
    std::abort();
}

// "jpg" and "jpeg" both belong to the jpeg format.
std::string formatOfExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".jpg" || extension == ".jpeg") {
        return "jpeg";
    }
    if (extension == ".png" || extension == ".webp" || extension == ".gif" || extension == ".bmp") {
        return extension.substr(1);
    }
    return {};
}

// Every file must be recognised where it lies in a noisy image, carved with
// exactly its bytes, and validated.
void expectCarvedExactly(const std::vector<std::pair<std::string, Bytes>>& files) {
    std::uint64_t size = 1 * kMiB;
    for (const auto& [name, bytes] : files) {
        size += bytes.size() + 4096;
    }
    test::VirtualSource source(size);
    source.setNoise(0x1234ABCD);
    std::vector<std::uint64_t> offsets;
    std::uint64_t offset = 4096;
    for (const auto& [name, bytes] : files) {
        offsets.push_back(offset);
        source.plant(offset, bytes);
        offset += bytes.size() + 4096 - (bytes.size() % 4096);
    }
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carveImages(source);
    for (std::size_t i = 0; i < files.size(); ++i) {
        SCOPED_TRACE(files[i].first);
        const auto found = std::find_if(carved.candidates.begin(), carved.candidates.end(),
                                        [&](const FileCandidate& c) { return c.sourceOffset == offsets[i]; });
        ASSERT_NE(found, carved.candidates.end());
        EXPECT_EQ(found->validation.status, ValidationStatus::Valid) << testing::describe(*found);
        EXPECT_EQ(found->length, files[i].second.size()) << testing::describe(*found);
        EXPECT_TRUE(carvedBytes(source, *found) == files[i].second);
    }
}

TEST(ImageSamplesTest, EverySampleIsRecognisedValidatedAndCarvedBack) {
    ASSERT_GE(test::samples::all().size(), 20u);
    std::vector<std::pair<std::string, Bytes>> files;
    for (const test::samples::Sample& sample : test::samples::all()) {
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes bytes = sample.data();
        const IFileFormat& format = formatWithId(sample.format);
        EXPECT_TRUE(testing::isIntact(format, bytes));
        // No other format claims the file.
        for (const auto& other : imageFormats()) {
            if (other->descriptor().id != sample.format) {
                EXPECT_FALSE(testing::headerOf(*other, bytes).plausible) << other->descriptor().id;
            }
        }
        files.emplace_back(sample.name, bytes);
    }
    expectCarvedExactly(files);
}

TEST(ImageReferenceTest, ExternalFilesAreValidAndCarvedExactly) {
    const std::optional<std::filesystem::path> directory =
        testing::directoryFromEnvironment(L"RECOVERY_IMAGE_REFERENCE_DIR");
    if (!directory) {
        GTEST_SKIP() << "set RECOVERY_IMAGE_REFERENCE_DIR to run against images made elsewhere";
    }
    std::vector<std::pair<std::string, Bytes>> files;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(*directory, ec)) {
        const std::string id = formatOfExtension(item.path());
        if (id.empty() || !item.is_regular_file()) {
            continue;
        }
        const Bytes bytes = test::readFile(item.path());
        if (bytes.size() > 64 * kMiB) {
            continue;  // the corpus is meant for ordinary photos
        }
        SCOPED_TRACE(item.path().filename().string());
        const IFileFormat& format = formatWithId(id);
        EXPECT_TRUE(testing::isIntact(format, bytes));
        files.emplace_back(item.path().filename().string(), bytes);
    }
    ASSERT_FALSE(files.empty()) << "no images in " << directory->string();
    expectCarvedExactly(files);
    std::cout << "checked " << files.size() << " reference images\n";
}

TEST(ImageBuilderExport, WritesFiles) {
    const std::optional<std::filesystem::path> directory = testing::directoryFromEnvironment(L"RECOVERY_IMAGE_EXPORT_DIR", false);
    if (!directory) {
        GTEST_SKIP() << "set RECOVERY_IMAGE_EXPORT_DIR to export the builders' files for other decoders";
    }
    std::vector<std::pair<std::string, Bytes>> files;

    test::JpegOptions jpeg;
    jpeg.width = 96;
    jpeg.height = 64;
    for (const auto& [name, sampling] : std::vector<std::pair<std::string, test::JpegSampling>>{
             {"gray", test::JpegSampling::Gray},
             {"444", test::JpegSampling::Yuv444},
             {"422", test::JpegSampling::Yuv422},
             {"420", test::JpegSampling::Yuv420}}) {
        jpeg.sampling = sampling;
        files.emplace_back("builder_" + name + ".jpg", test::makeJpeg(jpeg));
    }
    jpeg.sampling = test::JpegSampling::Yuv420;
    jpeg.progressive = true;
    files.emplace_back("builder_progressive.jpg", test::makeJpeg(jpeg));
    jpeg.progressive = false;
    jpeg.restartInterval = 2;
    files.emplace_back("builder_restart.jpg", test::makeJpeg(jpeg));
    jpeg.restartInterval = 0;
    jpeg.exifThumbnail = true;
    jpeg.comment = "made by the RecoveryEngine tests";
    files.emplace_back("builder_exif.jpg", test::makeJpeg(jpeg));

    test::PngOptions png;
    png.width = 48;
    png.height = 32;
    for (const auto& [name, color, depth] : std::vector<std::tuple<std::string, test::PngColor, int>>{
             {"gray1", test::PngColor::Gray, 1},
             {"gray8", test::PngColor::Gray, 8},
             {"gray16", test::PngColor::Gray, 16},
             {"rgb8", test::PngColor::Rgb, 8},
             {"rgba8", test::PngColor::Rgba, 8},
             {"palette4", test::PngColor::Palette, 4}}) {
        png.color = color;
        png.bitDepth = static_cast<std::uint8_t>(depth);
        files.emplace_back("builder_" + name + ".png", test::makePng(png));
    }
    png.color = test::PngColor::Rgb;
    png.bitDepth = 8;
    png.interlaced = true;
    files.emplace_back("builder_interlaced.png", test::makePng(png));
    png.interlaced = false;
    png.idatSize = 300;
    png.textChunks = true;
    files.emplace_back("builder_chunks.png", test::makePng(png));

    test::GifOptions gif;
    gif.width = 48;
    gif.height = 32;
    for (const std::uint8_t bits : {std::uint8_t{1}, std::uint8_t{4}, std::uint8_t{8}}) {
        gif.colorBits = bits;
        files.emplace_back("builder_" + std::to_string(bits) + "bit.gif", test::makeGif(gif));
    }
    gif.colorBits = 4;
    gif.frames = 3;
    gif.loop = true;
    gif.localColorTables = true;
    files.emplace_back("builder_animation.gif", test::makeGif(gif));
    gif.frames = 1;
    gif.interlaced = true;
    files.emplace_back("builder_interlaced.gif", test::makeGif(gif));

    test::BmpOptions bmp;
    bmp.width = 37;
    bmp.height = 21;
    for (const int bits : {1, 4, 8, 16, 24, 32}) {
        bmp.bitsPerPixel = static_cast<std::uint16_t>(bits);
        files.emplace_back("builder_" + std::to_string(bits) + "bit.bmp", test::makeBmp(bmp));
    }
    bmp.bitsPerPixel = 8;
    bmp.compression = test::BmpCompression::Rle8;
    files.emplace_back("builder_rle8.bmp", test::makeBmp(bmp));
    bmp.bitsPerPixel = 4;
    bmp.compression = test::BmpCompression::Rle4;
    files.emplace_back("builder_rle4.bmp", test::makeBmp(bmp));
    bmp.compression = test::BmpCompression::Rgb;
    bmp.bitsPerPixel = 24;
    bmp.header = test::BmpHeader::Core;
    files.emplace_back("builder_core.bmp", test::makeBmp(bmp));
    bmp.header = test::BmpHeader::V5;
    bmp.bitsPerPixel = 32;
    bmp.profileSize = 128;
    files.emplace_back("builder_v5.bmp", test::makeBmp(bmp));

    for (const auto& [name, kind] : std::vector<std::pair<std::string, test::WebpKind>>{
             {"lossy", test::WebpKind::Lossy},
             {"lossless", test::WebpKind::Lossless},
             {"alpha", test::WebpKind::LossyWithAlpha},
             {"animation", test::WebpKind::Animated}}) {
        test::WebpOptions webp;
        webp.kind = kind;
        files.emplace_back("builder_" + name + ".webp", test::makeWebp(webp));
    }
    test::WebpOptions metadata;
    metadata.kind = test::WebpKind::Lossless;
    metadata.iccSize = 24;
    metadata.exifSize = 18;
    metadata.xmpSize = 80;
    files.emplace_back("builder_metadata.webp", test::makeWebp(metadata));

    std::filesystem::create_directories(*directory);
    for (const auto& [name, bytes] : files) {
        test::writeFile(*directory / name, bytes);
    }
    // What is written must also pass the engine's own checks.
    for (const auto& [name, bytes] : files) {
        SCOPED_TRACE(name);
        const std::string id = formatOfExtension(name);
        ASSERT_FALSE(id.empty());
        EXPECT_TRUE(testing::isIntact(formatWithId(id), bytes));
    }
    std::cout << "wrote " << files.size() << " files to " << directory->string() << "\n";
}

}  // namespace
}  // namespace recovery::formats
