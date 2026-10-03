// BMP media validation: uncompressed bitmaps have nothing coded; RLE8 and
// RLE4 data of builder files and RLE data written here code by code, with
// runs that leave their row, deltas that leave the bitmap, rows after the
// last one, a missing end-of-bitmap code; truncation and fuzzing.

#include "validation/media_test_helpers.hpp"

#include "recovery/byte_order.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <vector>

namespace recovery::validation {
namespace {

using testing::Bytes;
using testing::mediaIs;
using testing::mediaOf;
using testing::validated;

// A BITMAPINFOHEADER bitmap of width x height whose pixel data is `rle` (RLE8 or RLE4).
Bytes bmpWithRle(std::int32_t width, std::int32_t height, bool rle4, std::initializer_list<std::uint8_t> rle) {
    const std::uint32_t colors = rle4 ? 16 : 256;
    const std::uint32_t pixelOffset = 14 + 40 + colors * 4;
    Bytes file(pixelOffset + rle.size());
    file[0] = std::byte{'B'};
    file[1] = std::byte{'M'};
    storeLe32(file, 2, static_cast<std::uint32_t>(file.size()));
    storeLe32(file, 10, pixelOffset);
    storeLe32(file, 14, 40);
    storeLe32(file, 18, static_cast<std::uint32_t>(width));
    storeLe32(file, 22, static_cast<std::uint32_t>(height));
    storeLe16(file, 26, 1);
    storeLe16(file, 28, rle4 ? 4 : 8);
    storeLe32(file, 30, rle4 ? 2 : 1);
    storeLe32(file, 34, static_cast<std::uint32_t>(rle.size()));
    std::size_t at = pixelOffset;
    for (const std::uint8_t value : rle) {
        file[at++] = static_cast<std::byte>(value);
    }
    return file;
}

TEST(BmpMediaTest, UncompressedBitmapsHaveNothingCoded) {
    for (const test::BmpHeader header : {test::BmpHeader::Core, test::BmpHeader::Info, test::BmpHeader::V4,
                                         test::BmpHeader::V5}) {
        for (const int bits : {1, 4, 8, 24}) {
            test::BmpOptions options;
            options.header = header;
            options.bitsPerPixel = static_cast<std::uint16_t>(bits);
            const Bytes file = test::makeBmp(options);
            const ValidationState state = validated("bmp", file);
            EXPECT_EQ(state.structural.status, LevelStatus::Passed) << testing::describe(state);
            EXPECT_EQ(state.media.status, LevelStatus::NotApplicable) << testing::describe(state);
            EXPECT_EQ(state.status(), carving::ValidationStatus::Valid);
        }
    }
    test::BmpOptions bitfields;
    bitfields.compression = test::BmpCompression::Bitfields;
    bitfields.bitsPerPixel = 32;
    EXPECT_EQ(mediaOf("bmp", test::makeBmp(bitfields)).status, LevelStatus::NotApplicable);
    for (const test::samples::Sample& sample : test::samples::all()) {
        if (sample.format == "bmp") {
            EXPECT_EQ(mediaOf("bmp", sample.data()).status, LevelStatus::NotApplicable) << sample.name;
        }
    }
}

TEST(BmpMediaTest, BuilderRleBitmapsPass) {
    for (const bool rle4 : {false, true}) {
        for (const std::int32_t width : {1, 7, 17, 64}) {
            test::BmpOptions options;
            options.compression = rle4 ? test::BmpCompression::Rle4 : test::BmpCompression::Rle8;
            options.bitsPerPixel = rle4 ? 4 : 8;
            options.width = width;
            options.height = 9;
            EXPECT_TRUE(testing::passesEveryLevel("bmp", test::makeBmp(options))) << rle4 << " " << width;
        }
    }
}

TEST(BmpMediaTest, RleCodesWrittenOneByOne) {
    // Rows of 4 pixels: an encoded run, an absolute run, a delta, ends of line.
    EXPECT_TRUE(mediaIs("bmp", bmpWithRle(4, 3, false, {4, 7, 0, 0, 0, 4, 1, 2, 3, 4, 0, 0, 0, 2, 2, 0, 2, 9, 0, 1}),
                        LevelStatus::Passed, "10 pixels"));
    // RLE4: an absolute run of 3 nibbles takes 2 bytes, padded to 2.
    EXPECT_TRUE(mediaIs("bmp", bmpWithRle(5, 1, true, {2, 0x12, 0, 3, 0x34, 0x50, 0, 1}), LevelStatus::Passed));
    // A bitmap left empty: only the end code.
    EXPECT_TRUE(mediaIs("bmp", bmpWithRle(4, 2, false, {0, 1}), LevelStatus::Passed, "0 pixels"));

    // A run longer than what is left of its row (the structure walk does not see columns).
    const Bytes wide = bmpWithRle(4, 2, false, {3, 1, 2, 1, 0, 1});
    EXPECT_EQ(validated("bmp", wide).structural.status, LevelStatus::Passed);
    EXPECT_TRUE(mediaIs("bmp", wide, LevelStatus::Failed, "leaves the 4x2 bitmap"));
    EXPECT_TRUE(mediaIs("bmp", bmpWithRle(4, 2, false, {0, 5, 1, 2, 3, 4, 5, 0, 0, 1}), LevelStatus::Failed,
                        "absolute run of 5 pixels"));
    // A delta out of the bitmap; a run after the last row.
    EXPECT_TRUE(mediaIs("bmp", bmpWithRle(4, 2, false, {0, 2, 5, 0, 0, 1}), LevelStatus::Failed, "delta"));
    EXPECT_TRUE(mediaIs("bmp", bmpWithRle(4, 1, false, {0, 2, 0, 1, 1, 7, 0, 1}), LevelStatus::Failed, "run of 1"));
    EXPECT_TRUE(mediaIs("bmp", bmpWithRle(4, 1, false, {0, 0, 0, 0, 0, 1}), LevelStatus::Failed, "after the last row"));
    // No end-of-bitmap code inside the image data.
    EXPECT_TRUE(mediaIs("bmp", bmpWithRle(4, 1, false, {4, 1, 0, 0}), LevelStatus::Failed, "end-of-bitmap"));
    // RLE bitmaps are bottom-up.
    EXPECT_TRUE(mediaIs("bmp", bmpWithRle(4, -1, false, {0, 1}), LevelStatus::Failed, "bottom-up"));
}

TEST(BmpMediaTest, FilesCutInsideTheRleDataAreTruncated) {
    test::BmpOptions options;
    options.compression = test::BmpCompression::Rle8;
    options.bitsPerPixel = 8;
    const Bytes file = test::makeBmp(options);
    const std::size_t pixels = loadLe32(file, 10);
    for (std::size_t length = pixels; length + 1 < file.size(); ++length) {
        EXPECT_TRUE(mediaIs("bmp", std::span(file).first(length), LevelStatus::Truncated)) << length;
    }
}

TEST(BmpMediaTest, DamagedFilesNeverBreakTheDecoder) {
    test::BmpOptions rle8;
    rle8.compression = test::BmpCompression::Rle8;
    rle8.bitsPerPixel = 8;
    testing::fuzzMedia("bmp", test::makeBmp(rle8), 400, 31);
    test::BmpOptions rle4;
    rle4.compression = test::BmpCompression::Rle4;
    rle4.bitsPerPixel = 4;
    rle4.width = 33;
    testing::fuzzMedia("bmp", test::makeBmp(rle4), 400, 32);
    testing::fuzzMedia("bmp", test::makeBmp({}), 100, 33);
}

}  // namespace
}  // namespace recovery::validation
