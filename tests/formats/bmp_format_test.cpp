// BMP: intact files from the builder (OS/2 core, BITMAPINFOHEADER, V4 and V5
// headers, 1 to 32 bits per pixel, bit fields, RLE8 and RLE4, top-down, an
// embedded color profile) and from libjpeg-turbo, libwebp and GDI+;
// truncation; header rejection ("BM" is only two bytes, so the header check
// carries the weight); size fields that disagree with the headers; damaged
// RLE data; fuzzing.

#include "formats/bmp_format.hpp"

#include "format_test_helpers.hpp"
#include "recovery/byte_order.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <string>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::ValidationStatus;
using test::BmpCompression;
using test::BmpHeader;
using test::BmpOptions;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::isIntact;
using testing::isInvalid;
using testing::overwritten;
using testing::verdictOf;

const BmpFormat& format() {
    static const BmpFormat instance;
    return instance;
}

BmpOptions bmpOptions(int width, int height, int bits, BmpHeader header = BmpHeader::Info,
                      BmpCompression compression = BmpCompression::Rgb) {
    BmpOptions options;
    options.width = width;
    options.height = height;
    options.bitsPerPixel = static_cast<std::uint16_t>(bits);
    options.header = header;
    options.compression = compression;
    return options;
}

TEST(BmpFormatTest, DescriptorIsValid) {
    RECOVERY_EXPECT_OK(carving::validateDescriptor(format().descriptor()));
    EXPECT_EQ(format().descriptor().id, "bmp");
    EXPECT_EQ(format().descriptor().endDetection, carving::EndDetectionMethod::SizeField);
}

TEST(BmpFormatTest, EveryHeaderAndPixelFormatIsIntact) {
    for (const int bits : {1, 4, 8, 24}) {
        SCOPED_TRACE("core " + std::to_string(bits));
        EXPECT_TRUE(isIntact(format(), test::makeBmp(bmpOptions(17, 11, bits, BmpHeader::Core))));
    }
    for (const int bits : {1, 2, 4, 8, 16, 24, 32}) {
        for (const BmpHeader header : {BmpHeader::Info, BmpHeader::V4, BmpHeader::V5}) {
            SCOPED_TRACE(std::to_string(static_cast<int>(header)) + " " + std::to_string(bits));
            EXPECT_TRUE(isIntact(format(), test::makeBmp(bmpOptions(13, 7, bits, header))));
        }
    }
    // Bit fields (16 and 32 bits), top-down rows, a file size field of 0, and
    // a V5 color profile after the pixel data.
    for (const int bits : {16, 32}) {
        EXPECT_TRUE(isIntact(format(), test::makeBmp(bmpOptions(9, 5, bits, BmpHeader::Info,
                                                                BmpCompression::Bitfields))));
        EXPECT_TRUE(isIntact(format(), test::makeBmp(bmpOptions(9, 5, bits, BmpHeader::V4,
                                                                BmpCompression::Bitfields))));
    }
    BmpOptions topDown = bmpOptions(20, 12, 24);
    topDown.topDown = true;
    EXPECT_TRUE(isIntact(format(), test::makeBmp(topDown)));
    BmpOptions zeroSize = bmpOptions(20, 12, 24);
    zeroSize.zeroFileSize = true;
    EXPECT_TRUE(isIntact(format(), test::makeBmp(zeroSize)));
    BmpOptions profile = bmpOptions(8, 8, 32, BmpHeader::V5);
    profile.profileSize = 128;
    EXPECT_TRUE(isIntact(format(), test::makeBmp(profile)));
    // The smallest file the format accepts.
    const Bytes tiny = test::makeBmp(bmpOptions(1, 1, 24, BmpHeader::Core));
    EXPECT_TRUE(isIntact(format(), tiny));
    EXPECT_GE(tiny.size(), format().descriptor().minimumSize);
}

TEST(BmpFormatTest, RunLengthEncodedBitmapsAreIntact) {
    for (const auto& [bits, compression] :
         std::vector<std::pair<int, BmpCompression>>{{8, BmpCompression::Rle8}, {4, BmpCompression::Rle4}}) {
        for (const auto& [width, height] : std::vector<std::pair<int, int>>{{1, 1}, {19, 7}, {64, 33}}) {
            SCOPED_TRACE(std::to_string(bits) + " bits, " + std::to_string(width) + "x" + std::to_string(height));
            const Bytes file = test::makeBmp(bmpOptions(width, height, bits, BmpHeader::Info, compression));
            EXPECT_TRUE(isIntact(format(), file));
        }
    }
}

TEST(BmpFormatTest, FilesFromRealEncodersAreIntact) {
    std::size_t checked = 0;
    for (const test::samples::Sample& sample : test::samples::all()) {
        if (sample.format != "bmp") {
            continue;
        }
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes file = sample.data();
        EXPECT_TRUE(isIntact(format(), file));
        EXPECT_TRUE(testing::prefixesAreTruncated(format(), file));
        ++checked;
    }
    // libjpeg-turbo (Windows and OS/2 headers, 24 and 8 bits), libwebp (V3
    // with bit fields) and GDI+ (24 and 32 bits).
    EXPECT_GE(checked, 5u);
}

TEST(BmpFormatTest, EveryPrefixIsTruncated) {
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), test::makeBmp(bmpOptions(23, 9, 24))));
    EXPECT_TRUE(testing::prefixesAreTruncated(
        format(), test::makeBmp(bmpOptions(23, 9, 8, BmpHeader::Info, BmpCompression::Rle8))));
    BmpOptions profile = bmpOptions(8, 8, 32, BmpHeader::V5);
    profile.profileSize = 64;
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), test::makeBmp(profile)));
}

TEST(BmpFormatTest, BytesAfterTheFileMakeTheContentInvalid) {
    const Bytes file = test::makeBmp();
    const Bytes longer = concat({file, testing::noise(30, 2)});
    EXPECT_TRUE(isInvalid(format(), longer, file.size()));
    EXPECT_EQ(endOf(format(), longer).length, file.size());
}

TEST(BmpFormatTest, HeaderCheckRejectsFalseSignatures) {
    const Bytes file = test::makeBmp(bmpOptions(16, 8, 24));
    EXPECT_TRUE(testing::headerOf(format(), file).plausible);
    const std::vector<std::pair<Bytes, std::string>> cases = {
        {overwritten(file, 14, {41}), "unknown DIB header size"},
        {overwritten(file, 26, {2}), "two planes"},
        {overwritten(file, 28, {3}), "3 bits per pixel"},
        {overwritten(file, 30, {7}), "unknown compression"},
        {overwritten(file, 30, {1}), "RLE8 with 24 bits per pixel"},
        {overwritten(file, 30, {3}), "bit fields with 24 bits per pixel"},
        {overwritten(file, 18, {0, 0, 0, 0}), "width 0"},
        {overwritten(file, 22, {0, 0, 0, 0}), "height 0"},
        {overwritten(file, 18, {0xFF, 0xFF, 0xFF, 0xFF}), "negative width"},
        {overwritten(file, 10, {20, 0, 0, 0}), "pixel data inside the headers"},
        {overwritten(file, 46, {0, 0, 0, 2}), "more palette colors than any bitmap has"},
        {Bytes{std::byte{'B'}, std::byte{'M'}}, "no DIB header"},
    };
    for (const auto& [bytes, what] : cases) {
        EXPECT_FALSE(testing::headerOf(format(), bytes).plausible) << what;
    }
    // A compressed bitmap must say how large its data is.
    const Bytes rle = test::makeBmp(bmpOptions(16, 8, 8, BmpHeader::Info, BmpCompression::Rle8));
    EXPECT_FALSE(testing::headerOf(format(), overwritten(rle, 34, {0, 0, 0, 0})).plausible);
    // A top-down bitmap cannot be run-length encoded.
    EXPECT_FALSE(testing::headerOf(format(), overwritten(rle, 22, {0xF8, 0xFF, 0xFF, 0xFF})).plausible);
}

TEST(BmpFormatTest, TheSizeComesFromTheFileSizeFieldOrTheHeaders) {
    const Bytes file = test::makeBmp(bmpOptions(16, 8, 24));
    // A file size field that is too small for the headers' own pixel data.
    Bytes small = file;
    storeLe32(small, 2, static_cast<std::uint32_t>(file.size() - 10));
    carving::EndDetection end = endOf(format(), small);
    EXPECT_EQ(end.status, EndStatus::Found) << testing::describe(end);
    EXPECT_EQ(end.length, file.size());
    EXPECT_TRUE(isInvalid(format(), small));
    // A field of 0: the size comes from the headers.
    Bytes zero = file;
    storeLe32(zero, 2, 0);
    EXPECT_TRUE(isIntact(format(), zero));
    // A larger field: the file is what it says it is, with padding at its end.
    const Bytes padded = concat({file, Bytes(6, std::byte{0})});
    Bytes larger = padded;
    storeLe32(larger, 2, static_cast<std::uint32_t>(padded.size()));
    EXPECT_TRUE(isIntact(format(), larger));
    // A field beyond the data: the file is cut short.
    Bytes beyond = file;
    storeLe32(beyond, 2, static_cast<std::uint32_t>(file.size() + 4096));
    end = endOf(format(), beyond);
    EXPECT_EQ(end.status, EndStatus::Truncated);
    EXPECT_EQ(verdictOf(format(), beyond).status, ValidationStatus::Truncated);
}

TEST(BmpFormatTest, ImpossibleHeadersBreakTheStructure) {
    const Bytes file = test::makeBmp(bmpOptions(16, 8, 24));
    for (const auto& [damaged, what] : std::vector<std::pair<Bytes, std::string>>{
             {overwritten(file, 14, {41}), "unknown DIB header size"},
             {overwritten(file, 28, {3}), "3 bits per pixel"},
             {overwritten(file, 22, {0, 0, 0, 0}), "height 0"},
             {overwritten(file, 10, {20, 0, 0, 0}), "pixel data inside the headers"},
         }) {
        SCOPED_TRACE(what);
        const carving::EndDetection end = endOf(format(), damaged);
        EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
        EXPECT_EQ(end.length, 0u);
        EXPECT_TRUE(isInvalid(format(), damaged, 0));
    }
    // Dimensions whose pixel data would overflow 64 bits.
    Bytes huge = test::makeBmp(bmpOptions(16, 8, 32));
    storeLe32(huge, 18, 0x7FFFFFFF);
    storeLe32(huge, 22, 0x7FFFFFFF);
    const carving::EndDetection end = endOf(format(), huge);
    EXPECT_NE(end.status, EndStatus::Found);
    EXPECT_LE(end.length, huge.size());
}

TEST(BmpFormatTest, HeaderInconsistenciesThatEncodersDoNotWriteAreInvalid) {
    const Bytes file = test::makeBmp(bmpOptions(16, 8, 8));
    // A pixel data offset inside the palette.
    Bytes overlapping = file;
    storeLe32(overlapping, 10, 14 + 40 + 4);
    EXPECT_EQ(endOf(format(), overlapping).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), overlapping));
    // Two planes: possible to parse, but no writer produces it.
    EXPECT_TRUE(isInvalid(format(), overwritten(file, 26, {2})));
    // A V5 profile that overlaps the header.
    BmpOptions options = bmpOptions(8, 8, 24, BmpHeader::V5);
    options.profileSize = 32;
    Bytes profile = test::makeBmp(options);
    ASSERT_TRUE(isIntact(format(), profile));
    storeLe32(profile, 14 + 112, 4);
    EXPECT_TRUE(isInvalid(format(), profile));
}

TEST(BmpFormatTest, DamagedRunLengthDataIsInvalid) {
    const Bytes file = test::makeBmp(bmpOptions(32, 10, 8, BmpHeader::Info, BmpCompression::Rle8));
    const std::uint32_t pixels = loadLe32(file, 10);
    // The end-of-bitmap code replaced by an encoded run.
    Bytes noEnd = file;
    noEnd[file.size() - 1] = std::byte{0x03};
    EXPECT_TRUE(isInvalid(format(), noEnd, pixels));
    // More end-of-line codes than the bitmap has rows.
    Bytes manyLines = file;
    for (std::size_t i = pixels; i + 2 < manyLines.size(); i += 2) {
        manyLines[i] = std::byte{0};
        manyLines[i + 1] = std::byte{0};
    }
    EXPECT_TRUE(isInvalid(format(), manyLines, pixels));
    // A delta that jumps past the last row.
    Bytes delta = file;
    delta[pixels] = std::byte{0};
    delta[pixels + 1] = std::byte{2};
    delta[pixels + 2] = std::byte{0};
    delta[pixels + 3] = std::byte{0xFF};
    EXPECT_TRUE(isInvalid(format(), delta, pixels));
    // An absolute run that reaches beyond the pixel data (the first command of
    // a bitmap whose data is shorter than the run).
    const Bytes small = test::makeBmp(bmpOptions(8, 4, 8, BmpHeader::Info, BmpCompression::Rle8));
    const std::uint32_t smallPixels = loadLe32(small, 10);
    ASSERT_LT(small.size() - smallPixels, 256u);
    const Bytes absolute = overwritten(small, smallPixels, {0x00, 0xFF});
    EXPECT_TRUE(isInvalid(format(), absolute, smallPixels));
    // End detection does not read the pixel data: the file still ends where
    // its headers say (only validation reads it).
    EXPECT_EQ(endOf(format(), noEnd).status, EndStatus::Found);
}

TEST(BmpFormatTest, FragmentationInsideUncompressedPixelsIsNotDetectable) {
    // Uncompressed pixel data has no structure, so another file's data in the
    // middle of it changes nothing that can be checked (L65).
    const Bytes file = test::makeBmp(bmpOptions(32, 32, 24));
    const std::uint32_t pixels = loadLe32(file, 10);
    const Bytes damaged = testing::overwritten(file, pixels + 100, testing::noise(512, 15));
    EXPECT_EQ(verdictOf(format(), damaged).status, ValidationStatus::Valid);
    // Data inserted between two fragments does show up, because the file is
    // then longer than its headers say.
    const Bytes fragmented = testing::inserted(file, pixels + 100, testing::noise(512, 16));
    EXPECT_EQ(endOf(format(), fragmented).length, file.size());
    EXPECT_TRUE(isInvalid(format(), fragmented, file.size()));
}

TEST(BmpFormatTest, FuzzedFilesStayWithinTheirData) {
    testing::fuzz(format(), test::makeBmp(bmpOptions(16, 9, 8)), 1500, 61);
    testing::fuzz(format(), test::makeBmp(bmpOptions(16, 9, 8, BmpHeader::Info, BmpCompression::Rle8)), 1500, 62);
    BmpOptions profile = bmpOptions(8, 8, 32, BmpHeader::V5);
    profile.profileSize = 64;
    testing::fuzz(format(), test::makeBmp(profile), 1000, 63);
}

}  // namespace
}  // namespace recovery::formats
