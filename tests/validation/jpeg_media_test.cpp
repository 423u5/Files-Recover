// JPEG media validation: the entropy-coded data of builder files (baseline
// and progressive, every sampling, restart intervals, odd sizes, Exif
// thumbnails, fill bytes) and of independent encoders (libjpeg-turbo's
// successive approximation, restart markers, optimized tables, GDI+); the
// standard tables of Annex K.3 for files without DHT; damage the marker walk
// cannot see (L62): foreign data inside a scan, with and without restart
// markers; broken tables and progressions; truncation, limits, fuzzing.

#include "validation/media_test_helpers.hpp"

#include "formats/format_test_helpers.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

namespace recovery::validation {
namespace {

using testing::Bytes;
using testing::mediaIs;
using testing::mediaOf;
using testing::passesEveryLevel;
using testing::validated;

struct ImageSize {
    std::uint16_t width;
    std::uint16_t height;
};

// The first marker with this code.
test::JpegMarker markerOf(const Bytes& file, std::uint8_t code, std::size_t index = 0) {
    for (const test::JpegMarker& marker : test::jpegMarkers(file)) {
        if (marker.code == code && index-- == 0) {
            return marker;
        }
    }
    throw std::runtime_error("no such marker");
}

// Where the first scan's entropy-coded data starts.
std::size_t scanData(const Bytes& file) {
    const test::JpegMarker sos = markerOf(file, 0xDA);
    return sos.offset + 2 + sos.length;
}

Bytes photo(std::uint16_t width, std::uint16_t height, bool progressive = false, std::uint16_t restart = 0) {
    test::JpegOptions options;
    options.width = width;
    options.height = height;
    options.progressive = progressive;
    options.restartInterval = restart;
    return test::makeJpeg(options);
}

TEST(JpegMediaTest, BuilderFilesOfEverySamplingAndModePass) {
    for (const test::JpegSampling sampling : {test::JpegSampling::Gray, test::JpegSampling::Yuv444,
                                              test::JpegSampling::Yuv422, test::JpegSampling::Yuv420}) {
        for (const bool progressive : {false, true}) {
            for (const std::uint16_t restart : std::array<std::uint16_t, 3>{0, 1, 3}) {
                for (const ImageSize size : std::array<ImageSize, 3>{{{37, 29}, {1, 1}, {64, 48}}}) {
                    test::JpegOptions options;
                    options.sampling = sampling;
                    options.progressive = progressive;
                    options.restartInterval = restart;
                    options.width = size.width;
                    options.height = size.height;
                    EXPECT_TRUE(passesEveryLevel("jpeg", test::makeJpeg(options)))
                        << static_cast<int>(sampling) << " progressive " << progressive << " restart " << restart
                        << " " << size.width << "x" << size.height;
                }
            }
        }
    }
    test::JpegOptions extras;
    extras.exifThumbnail = true;
    extras.comment = "a comment";
    extras.fillBytes = 3;
    extras.jfif = false;
    EXPECT_TRUE(passesEveryLevel("jpeg", test::makeJpeg(extras)));
    const LevelResult result = mediaOf("jpeg", photo(256, 192, false, 4));
    EXPECT_EQ(result.status, LevelStatus::Passed);
    EXPECT_NE(result.detail.find("baseline, 8-bit, 256x192, 3 components: 1 scan, 192 MCUs decoded"),
              std::string::npos)
        << result.detail;
    EXPECT_NE(result.detail.find("47 restart markers"), std::string::npos) << result.detail;
}

TEST(JpegMediaTest, IndependentEncodersFilesPass) {
    for (const char* name : {"cjpeg_baseline.jpg", "cjpeg_444.jpg", "cjpeg_gray_optimized.jpg",
                             "cjpeg_progressive.jpg", "cjpeg_restart.jpg", "jpegtran_progressive_restart.jpg",
                             "gdiplus.jpg"}) {
        EXPECT_TRUE(passesEveryLevel("jpeg", test::samples::named(name).data())) << name;
    }
    // libjpeg's progression: DC first and refinement, AC first and refinement scans.
    const LevelResult progressive = mediaOf("jpeg", test::samples::named("cjpeg_progressive.jpg").data());
    EXPECT_NE(progressive.detail.find("progressive"), std::string::npos) << progressive.detail;
    EXPECT_NE(progressive.detail.find("10 scans"), std::string::npos) << progressive.detail;
}

TEST(JpegMediaTest, ArithmeticCodingIsUnsupported) {
    const ValidationState state = validated("jpeg", test::samples::named("cjpeg_arithmetic.jpg").data());
    EXPECT_EQ(state.structural.status, LevelStatus::Passed) << testing::describe(state);
    EXPECT_EQ(state.media.status, LevelStatus::Unsupported) << testing::describe(state);
    EXPECT_NE(state.media.detail.find("arithmetic"), std::string::npos);
    EXPECT_EQ(state.status(), carving::ValidationStatus::Valid);
}

TEST(JpegMediaTest, FilesWithoutHuffmanTablesUseTheStandardOnes) {
    // cjpeg writes the tables of Annex K.3; Motion JPEG frames leave them out.
    const Bytes file = test::samples::named("cjpeg_baseline.jpg").data();
    Bytes stripped;
    std::size_t copied = 0;
    for (const test::JpegMarker& marker : test::jpegMarkers(file)) {
        if (marker.code == 0xC4) {
            stripped.insert(stripped.end(), file.begin() + static_cast<std::ptrdiff_t>(copied),
                            file.begin() + static_cast<std::ptrdiff_t>(marker.offset));
            copied = marker.offset + 2 + marker.length;
        }
    }
    stripped.insert(stripped.end(), file.begin() + static_cast<std::ptrdiff_t>(copied), file.end());
    ASSERT_LT(stripped.size(), file.size());
    EXPECT_TRUE(mediaIs("jpeg", stripped, LevelStatus::Passed, "the standard Huffman tables"));
}

TEST(JpegMediaTest, ForeignDataInsideAScanIsCaught) {
    // L62: data without 0xFF bytes inside the entropy-coded data, which the
    // marker walk reads as more entropy-coded data.
    const Bytes file = photo(256, 192);
    const std::size_t inside = scanData(file) + 1000;
    for (const Bytes& foreign : {Bytes(2048, std::byte{0}), formats::testing::quietNoise(2048, 5)}) {
        const Bytes inserted = formats::testing::inserted(file, inside, foreign);
        const Bytes overwritten = formats::testing::overwritten(file, inside, foreign);
        for (const Bytes* damaged : {&inserted, &overwritten}) {
            const ValidationState state = validated("jpeg", *damaged);
            EXPECT_NE(state.media.status, LevelStatus::Passed) << testing::describe(state);
            EXPECT_EQ(state.status(), carving::ValidationStatus::Invalid) << testing::describe(state);
        }
    }
    // With restart markers too: the interval holding the foreign data no longer decodes.
    const Bytes restarted = photo(256, 192, false, 4);
    const std::size_t data = scanData(restarted) + 700;
    const Bytes zeros = formats::testing::inserted(restarted, data, Bytes(1024, std::byte{0}));
    EXPECT_EQ(validated("jpeg", zeros).status(), carving::ValidationStatus::Invalid);
}

TEST(JpegMediaTest, BrokenTablesAndScanHeadersFail) {
    const Bytes file = test::samples::named("cjpeg_baseline.jpg").data();
    // A DHT whose code counts over-subscribe their lengths: two codes of
    // length 1 (the counts 0, 1, 5 become 2, 0, 4: still 12 codes).
    Bytes table = file;
    const test::JpegMarker dht = markerOf(file, 0xC4);
    table[dht.offset + 5] = std::byte{2};
    table[dht.offset + 6] = std::byte{0};
    table[dht.offset + 7] = std::byte{4};
    EXPECT_TRUE(mediaIs("jpeg", table, LevelStatus::Failed, "prefix code"));

    // The scan names a Huffman table nobody defined.
    Bytes undefined = file;
    const test::JpegMarker sos = markerOf(file, 0xDA);
    undefined[sos.offset + 6] = std::byte{0x33};
    EXPECT_TRUE(mediaIs("jpeg", undefined, LevelStatus::Failed, "not defined"));

    // Progressive: a refinement scan before the scan it refines.
    const Bytes progressive = test::samples::named("cjpeg_progressive.jpg").data();
    Bytes order = progressive;
    const test::JpegMarker second = markerOf(progressive, 0xDA, 1);
    const std::size_t approximation = second.offset + 2 + second.length - 1;
    order[approximation] = static_cast<std::byte>(0x32);
    const LevelResult refined = mediaOf("jpeg", order);
    EXPECT_EQ(refined.status, LevelStatus::Failed) << testing::describe(refined);
}

TEST(JpegMediaTest, RestartMarkersMustComeInOrderAfterEachInterval) {
    const Bytes file = photo(64, 48, false, 2);
    // RST1 replaced by RST3: the marker walk notices the order too, but the
    // decoder must say where.
    Bytes order = file;
    const test::JpegMarker rst = markerOf(file, 0xD1);
    order[rst.offset + 1] = std::byte{0xD3};
    EXPECT_TRUE(mediaIs("jpeg", order, LevelStatus::Failed, "where RST1 is due"));
}

TEST(JpegMediaTest, FilesCutShortAreTruncated) {
    for (const bool progressive : {false, true}) {
        const Bytes file = photo(64, 48, progressive, 2);
        for (std::size_t length = 2; length + 2 < file.size(); length += length < 700 ? 7 : 61) {
            const LevelResult result = mediaOf("jpeg", std::span(file).first(length));
            EXPECT_EQ(result.status, LevelStatus::Truncated) << length << ": " << testing::describe(result);
        }
    }
}

TEST(JpegMediaTest, LimitsMakeLargeImagesUnsupported) {
    MediaLimits work;
    work.maxDecodedBytes = 1000;
    EXPECT_EQ(mediaOf("jpeg", photo(64, 48), work).status, LevelStatus::Unsupported);
    MediaLimits memory;
    memory.maxMemory = 64;
    EXPECT_EQ(mediaOf("jpeg", test::samples::named("cjpeg_progressive.jpg").data(), memory).status,
              LevelStatus::Unsupported);
}

TEST(JpegMediaTest, DamagedFilesNeverBreakTheDecoder) {
    testing::fuzzMedia("jpeg", photo(64, 48, false, 3), 400, 41);
    testing::fuzzMedia("jpeg", photo(64, 48, true), 400, 42);
    for (const char* name : {"cjpeg_progressive.jpg", "jpegtran_progressive_restart.jpg", "gdiplus.jpg",
                             "cjpeg_arithmetic.jpg"}) {
        testing::fuzzMedia("jpeg", test::samples::named(name).data(), 300, 43);
    }
}

}  // namespace
}  // namespace recovery::validation
