// GIF: intact files from the builder (87a and 89a, global and local color
// tables, every color depth, several frames, interlacing, extensions) and
// from giflib and GDI+; truncation at every position; header rejection;
// damaged block introducers, sub-block chains, extensions and code sizes;
// fragmented image data; fuzzing.

#include "formats/gif_format.hpp"

#include "format_test_helpers.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <string>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::ValidationStatus;
using test::GifOptions;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::isIntact;
using testing::isInvalid;
using testing::overwritten;
using testing::verdictOf;

const GifFormat& format() {
    static const GifFormat instance;
    return instance;
}

// Offset of the first block after the header and the global color table.
std::size_t firstBlock(const Bytes& file) {
    const auto packed = static_cast<std::uint8_t>(file[10]);
    return 13 + ((packed & 0x80) != 0 ? 3u << ((packed & 7) + 1) : 0u);
}

// Offset of the first image descriptor (0x2C) of a builder file.
std::size_t firstImage(const Bytes& file) {
    std::size_t position = firstBlock(file);
    while (position < file.size() && static_cast<std::uint8_t>(file[position]) == 0x21) {
        position += 2;
        while (position < file.size()) {
            const auto size = static_cast<std::uint8_t>(file[position]);
            position += 1 + size;
            if (size == 0) {
                break;
            }
        }
    }
    return position;
}

TEST(GifFormatTest, DescriptorIsValid) {
    RECOVERY_EXPECT_OK(carving::validateDescriptor(format().descriptor()));
    EXPECT_EQ(format().descriptor().id, "gif");
    EXPECT_EQ(format().descriptor().signatures.size(), 2u);
    EXPECT_EQ(format().descriptor().endDetection, carving::EndDetectionMethod::StructureWalk);
}

TEST(GifFormatTest, BuilderFilesOfEveryShapeAreIntact) {
    for (const std::uint8_t colorBits : {std::uint8_t{1}, std::uint8_t{4}, std::uint8_t{8}}) {
        for (const bool version89a : {false, true}) {
            for (const bool interlaced : {false, true}) {
                GifOptions options;
                options.colorBits = colorBits;
                options.version89a = version89a;
                options.interlaced = interlaced;
                SCOPED_TRACE(std::to_string(colorBits) + (version89a ? " 89a" : " 87a") +
                             (interlaced ? " interlaced" : ""));
                EXPECT_TRUE(isIntact(format(), test::makeGif(options)));
            }
        }
    }
    // Animations with local color tables, a loop extension and a comment.
    GifOptions animation;
    animation.frames = 4;
    animation.localColorTables = true;
    animation.loop = true;
    animation.comment = "made by the RecoveryEngine tests";
    EXPECT_TRUE(isIntact(format(), test::makeGif(animation)));
    // Without a global color table each frame brings its own.
    GifOptions local;
    local.globalColorTable = false;
    EXPECT_TRUE(isIntact(format(), test::makeGif(local)));
    // The smallest image the format accepts.
    GifOptions tiny;
    tiny.width = 1;
    tiny.height = 1;
    tiny.colorBits = 1;
    const Bytes small = test::makeGif(tiny);
    EXPECT_TRUE(isIntact(format(), small));
    EXPECT_GE(small.size(), format().descriptor().minimumSize);
}

TEST(GifFormatTest, FilesFromRealEncodersAreIntact) {
    std::size_t checked = 0;
    for (const test::samples::Sample& sample : test::samples::all()) {
        if (sample.format != "gif") {
            continue;
        }
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes file = sample.data();
        EXPECT_TRUE(isIntact(format(), file));
        EXPECT_TRUE(testing::prefixesAreTruncated(format(), file));
        ++checked;
    }
    EXPECT_GE(checked, 2u);  // giflib and GDI+
}

TEST(GifFormatTest, EveryPrefixIsTruncated) {
    GifOptions options;
    options.frames = 3;
    options.loop = true;
    options.comment = "comment";
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), test::makeGif(options)));
}

TEST(GifFormatTest, BytesAfterTheTrailerMakeTheContentInvalid) {
    const Bytes file = test::makeGif();
    const Bytes longer = concat({file, testing::noise(50, 6)});
    EXPECT_TRUE(isInvalid(format(), longer, file.size()));
    EXPECT_EQ(endOf(format(), longer).length, file.size());
}

TEST(GifFormatTest, HeaderCheckRejectsFalseSignatures) {
    const Bytes file = test::makeGif();
    EXPECT_TRUE(testing::headerOf(format(), file).plausible);
    // A version the signatures do not cover never reaches the header check,
    // but the first block introducer after the color table must be one of three.
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, firstBlock(file), {0x2B})).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, firstBlock(file), {0x00})).plausible);
    EXPECT_FALSE(testing::headerOf(format(), Bytes{std::byte{'G'}, std::byte{'I'}}).plausible);
    // A file cut off before the introducer is not refused.
    EXPECT_TRUE(testing::headerOf(format(), testing::prefix(file, 13)).plausible);
}

TEST(GifFormatTest, InvalidBlockIntroducersBreakTheStructure) {
    const Bytes file = test::makeGif();
    const std::size_t image = firstImage(file);
    for (const std::uint8_t introducer : {std::uint8_t{0x00}, std::uint8_t{0x2B}, std::uint8_t{0xFF}}) {
        const Bytes damaged = overwritten(file, image, {introducer});
        const carving::EndDetection end = endOf(format(), damaged);
        EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
        EXPECT_EQ(end.length, image);
        EXPECT_TRUE(isInvalid(format(), damaged, image));
    }
    // The trailer replaced by a byte that is not a block introducer.
    const Bytes noTrailer = overwritten(file, file.size() - 1, {0x7F});
    EXPECT_EQ(endOf(format(), noTrailer).status, EndStatus::Broken);
    // The trailer cut off: the data ends before it.
    EXPECT_EQ(endOf(format(), testing::prefix(file, file.size() - 1)).status, EndStatus::Truncated);
}

TEST(GifFormatTest, DamagedSubBlockChainsAndCodeSizesAreCaught) {
    GifOptions options;
    options.width = 40;
    options.height = 40;
    const Bytes file = test::makeGif(options);
    const std::size_t image = firstImage(file);
    const std::size_t codeSize = image + 10;  // no local color table
    // An LZW minimum code size no decoder accepts.
    for (const std::uint8_t size : {std::uint8_t{0}, std::uint8_t{12}, std::uint8_t{0xFF}}) {
        const Bytes damaged = overwritten(file, codeSize, {size});
        EXPECT_EQ(endOf(format(), damaged).status, EndStatus::Found);
        EXPECT_TRUE(isInvalid(format(), damaged, image));
    }
    // A chain whose terminator is replaced by another size runs past the trailer.
    const Bytes swallowed = overwritten(file, file.size() - 2, {0x40});
    const carving::EndDetection end = endOf(format(), swallowed);
    EXPECT_EQ(end.status, EndStatus::Truncated) << testing::describe(end);
    EXPECT_EQ(verdictOf(format(), swallowed).status, ValidationStatus::Truncated);
    // An image whose data chain is empty.
    const Bytes empty = overwritten(file, codeSize + 1, {0x00});
    EXPECT_TRUE(isInvalid(format(), empty, image));
}

TEST(GifFormatTest, ExtensionsAreCheckedButUnknownLabelsAreAccepted) {
    GifOptions options;
    options.graphicControl = true;
    const Bytes file = test::makeGif(options);
    // The graphic control extension is introduced before the first image.
    const std::size_t packed = static_cast<std::size_t>(static_cast<std::uint8_t>(file[10]));
    const std::size_t extension = 13 + ((packed & 0x80) != 0 ? 3u << ((packed & 7) + 1) : 0u);
    ASSERT_EQ(static_cast<std::uint8_t>(file[extension]), 0x21);
    ASSERT_EQ(static_cast<std::uint8_t>(file[extension + 1]), 0xF9);
    // A graphic control block of the wrong size loses the chain: what follows
    // is no longer where the structure expects it.
    EXPECT_NE(verdictOf(format(), overwritten(file, extension + 2, {5})).status, ValidationStatus::Valid);
    // An application extension whose first block is not 11 bytes long.
    EXPECT_TRUE(isInvalid(format(), overwritten(file, extension + 1, {0xFF}), extension));
    // An unknown extension label is skipped like any other.
    EXPECT_TRUE(isIntact(format(), overwritten(file, extension + 1, {0x42})));
}

TEST(GifFormatTest, AFileWithoutAnImageIsInvalid) {
    const Bytes file = test::makeGif();
    const std::size_t image = firstImage(file);
    const Bytes noImage = concat({testing::prefix(file, image), Bytes{std::byte{0x3B}}});
    EXPECT_EQ(endOf(format(), noImage).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), noImage));
}

TEST(GifFormatTest, FragmentedImageDataIsNotValid) {
    GifOptions options;
    options.width = 64;
    options.height = 64;
    const Bytes file = test::makeGif(options);
    const std::size_t image = firstImage(file);
    const std::size_t inside = image + 11 + 200;
    ASSERT_LT(inside + 2048, file.size());
    // Zeros between or over the fragments end the chain of sub-blocks, and
    // what follows is not a block introducer.
    const Bytes zeros(2048, std::byte{0});
    EXPECT_NE(verdictOf(format(), testing::inserted(file, inside, zeros)).status, ValidationStatus::Valid);
    EXPECT_NE(verdictOf(format(), testing::overwritten(file, inside, zeros)).status, ValidationStatus::Valid);
    // Another file's data, though, can chain through as sub-blocks and reach a
    // trailer, so the walk accepts a file whose pixels are somebody else's
    // (L64). Only decoding the LZW data would tell. This pins the limitation:
    // it is the behaviour to change when GIF decoding arrives (P14).
    const Bytes foreign = testing::noise(2048, 31);
    EXPECT_EQ(verdictOf(format(), testing::overwritten(file, inside, foreign)).status, ValidationStatus::Valid);
}

TEST(GifFormatTest, FuzzedFilesStayWithinTheirData) {
    GifOptions options;
    options.frames = 2;
    options.loop = true;
    options.comment = "fuzz";
    testing::fuzz(format(), test::makeGif(options), 2000, 41);
    testing::fuzz(format(), test::samples::named("giflib.gif").data(), 1000, 42);
}

TEST(GifFormatTest, HostileSubBlockChainsEndAtTheData) {
    // 0xFF sub-block sizes chain through everything: erased media reads as one
    // long chain, which ends where the data does (L63).
    const Bytes file = test::makeGif();
    const std::size_t image = firstImage(file);
    const Bytes erased = concat({testing::prefix(file, image + 11), Bytes(512 * 1024, std::byte{0xFF})});
    const carving::EndDetection end = endOf(format(), erased);
    EXPECT_EQ(end.status, EndStatus::Truncated) << testing::describe(end);
    EXPECT_EQ(end.length, erased.size());
}

}  // namespace
}  // namespace recovery::formats
