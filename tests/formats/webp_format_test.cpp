// WebP: intact files from the builder (simple lossy and lossless, extended
// with alpha and metadata, animations) and from libwebp; truncation at every
// position; header rejection; RIFF sizes and chunk layouts that disagree;
// damaged VP8, VP8L, ALPH, ANIM and ANMF chunks; fragmented data; fuzzing.

#include "formats/webp_format.hpp"

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
using test::WebpKind;
using test::WebpOptions;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::isIntact;
using testing::isInvalid;
using testing::overwritten;
using testing::verdictOf;

const WebpFormat& format() {
    static const WebpFormat instance;
    return instance;
}

// Offset of the chunk with this FourCC.
std::size_t chunkAt(const Bytes& file, std::string_view fourCc) {
    for (std::size_t position = 12; position + 8 <= file.size();) {
        std::string type;
        for (std::size_t i = 0; i < 4; ++i) {
            type += static_cast<char>(file[position + i]);
        }
        const std::uint32_t size = loadLe32(file, position + 4);
        if (type == fourCc) {
            return position;
        }
        position += 8 + size + (size & 1);
    }
    ADD_FAILURE() << "no chunk " << fourCc;
    return 0;
}

WebpOptions kind(WebpKind value) {
    WebpOptions options;
    options.kind = value;
    return options;
}

TEST(WebpFormatTest, DescriptorIsValid) {
    RECOVERY_EXPECT_OK(carving::validateDescriptor(format().descriptor()));
    EXPECT_EQ(format().descriptor().id, "webp");
    EXPECT_EQ(format().descriptor().endDetection, carving::EndDetectionMethod::SizeField);
    // The signature compares "RIFF" and "WEBP" and ignores the size between them.
    const carving::FileSignature& signature = format().descriptor().signatures.front();
    EXPECT_EQ(signature.pattern.size(), 12u);
    EXPECT_EQ(signature.mask.size(), 12u);
    EXPECT_EQ(signature.offset, 0u);
}

TEST(WebpFormatTest, BuilderFilesOfEveryShapeAreIntact) {
    for (const WebpKind value :
         {WebpKind::Lossy, WebpKind::Lossless, WebpKind::LossyWithAlpha, WebpKind::Animated}) {
        SCOPED_TRACE(static_cast<int>(value));
        EXPECT_TRUE(isIntact(format(), test::makeWebp(kind(value))));
    }
    // The extended format for a plain image, metadata chunks, and a chunk of
    // odd size (which needs its padding byte).
    WebpOptions extended = kind(WebpKind::Lossless);
    extended.extended = true;
    extended.iccSize = 20;
    extended.exifSize = 17;
    extended.xmpSize = 63;
    extended.unknownChunk = true;
    EXPECT_TRUE(isIntact(format(), test::makeWebp(extended)));
    WebpOptions animation = kind(WebpKind::Animated);
    animation.frames = 5;
    EXPECT_TRUE(isIntact(format(), test::makeWebp(animation)));
}

TEST(WebpFormatTest, FilesFromLibwebpAreIntact) {
    std::size_t checked = 0;
    for (const test::samples::Sample& sample : test::samples::all()) {
        if (sample.format != "webp") {
            continue;
        }
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes file = sample.data();
        EXPECT_TRUE(isIntact(format(), file));
        EXPECT_TRUE(testing::prefixesAreTruncated(format(), file));
        ++checked;
    }
    // Lossy, lossless, alpha, lossless with alpha, metadata, animation.
    EXPECT_GE(checked, 6u);
}

TEST(WebpFormatTest, EveryPrefixIsTruncated) {
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), test::makeWebp(kind(WebpKind::LossyWithAlpha))));
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), test::makeWebp(kind(WebpKind::Animated))));
}

TEST(WebpFormatTest, BytesAfterTheRiffDataMakeTheContentInvalid) {
    const Bytes file = test::makeWebp();
    const Bytes longer = concat({file, testing::noise(40, 8)});
    EXPECT_TRUE(isInvalid(format(), longer, file.size()));
    EXPECT_EQ(endOf(format(), longer).length, file.size());
}

TEST(WebpFormatTest, HeaderCheckRejectsFalseSignatures) {
    const Bytes lossy = test::makeWebp(kind(WebpKind::Lossy));
    const Bytes lossless = test::makeWebp(kind(WebpKind::Lossless));
    EXPECT_TRUE(testing::headerOf(format(), lossy).plausible);
    EXPECT_TRUE(testing::headerOf(format(), lossless).plausible);
    Bytes smallRiff = lossy;
    storeLe32(smallRiff, 4, 10);
    EXPECT_FALSE(testing::headerOf(format(), smallRiff).plausible);
    Bytes chunkTooBig = lossy;
    storeLe32(chunkTooBig, 16, 0xFFFFFF);
    EXPECT_FALSE(testing::headerOf(format(), chunkTooBig).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(lossy, 12, {'J', 'U', 'N', 'K'})).plausible);
    // VP8 without its start code or key frame bit, VP8L without its signature.
    EXPECT_FALSE(testing::headerOf(format(), overwritten(lossy, 23, {0x00})).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(lossy, 20, {0x01})).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(lossless, 20, {0x30})).plausible);
    // A VP8X that is too short for its own fields.
    Bytes shortVp8x = test::makeWebp(kind(WebpKind::LossyWithAlpha));
    storeLe32(shortVp8x, 16, 6);
    EXPECT_FALSE(testing::headerOf(format(), shortVp8x).plausible);
}

TEST(WebpFormatTest, RiffSizesThatDisagreeWithTheChunksAreCaught) {
    const Bytes file = test::makeWebp(kind(WebpKind::Lossless));
    // A RIFF size beyond the data: the file is cut short.
    Bytes larger = file;
    storeLe32(larger, 4, loadLe32(file, 4) + 1000);
    EXPECT_EQ(endOf(format(), larger).status, EndStatus::Truncated);
    EXPECT_EQ(verdictOf(format(), larger).status, ValidationStatus::Truncated);
    // A RIFF size that ends inside the first chunk.
    Bytes smaller = file;
    storeLe32(smaller, 4, 20);
    carving::EndDetection end = endOf(format(), smaller);
    EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
    EXPECT_TRUE(isInvalid(format(), smaller));
    // A chunk size that runs beyond the RIFF data.
    Bytes chunkBeyond = file;
    storeLe32(chunkBeyond, 16, loadLe32(file, 16) + 8);
    end = endOf(format(), chunkBeyond);
    EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
    EXPECT_EQ(end.length, 12u);
    // An odd chunk size at the end of the file, without its padding byte.
    WebpOptions options = kind(WebpKind::Lossless);
    options.exifSize = 15;
    const Bytes padded = test::makeWebp(options);
    const std::size_t exif = chunkAt(padded, "EXIF");
    Bytes unpadded = testing::erased(padded, padded.size() - 1, 1);
    storeLe32(unpadded, 4, static_cast<std::uint32_t>(unpadded.size() - 8));
    EXPECT_EQ(endOf(format(), unpadded).length, unpadded.size());
    EXPECT_TRUE(isInvalid(format(), unpadded, exif));
}

TEST(WebpFormatTest, DamagedChunkIdsBreakTheStructure) {
    const Bytes file = test::makeWebp(kind(WebpKind::LossyWithAlpha));
    const std::size_t alph = chunkAt(file, "ALPH");
    const Bytes damaged = overwritten(file, alph, {0x01, 0x02});
    const carving::EndDetection end = endOf(format(), damaged);
    EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
    EXPECT_EQ(end.length, alph);
    EXPECT_TRUE(isInvalid(format(), damaged, alph));
}

TEST(WebpFormatTest, DamagedBitstreamHeadersAreInvalid) {
    const Bytes lossy = test::makeWebp(kind(WebpKind::Lossy));
    const std::size_t vp8 = chunkAt(lossy, "VP8 ");
    // Not a key frame, not shown, no start code, a first partition beyond the chunk.
    EXPECT_TRUE(isInvalid(format(), overwritten(lossy, vp8 + 8, {0x01}), vp8));
    EXPECT_TRUE(isInvalid(format(), overwritten(lossy, vp8 + 11, {0x00}), vp8));
    Bytes partition = lossy;
    partition[vp8 + 10] = std::byte{0xFF};
    EXPECT_TRUE(isInvalid(format(), partition, vp8));
    const Bytes lossless = test::makeWebp(kind(WebpKind::Lossless));
    const std::size_t vp8l = chunkAt(lossless, "VP8L");
    EXPECT_TRUE(isInvalid(format(), overwritten(lossless, vp8l + 8, {0x2E}), vp8l));
    EXPECT_TRUE(isInvalid(format(), overwritten(lossless, vp8l + 12, {0xE0}), vp8l));
    // In an extended file the image must have the canvas's size.
    Bytes extended = test::makeWebp(kind(WebpKind::LossyWithAlpha));
    const std::size_t vp8x = chunkAt(extended, "VP8X");
    extended[vp8x + 8 + 4] = std::byte{0x20};  // a wider canvas
    EXPECT_TRUE(isInvalid(format(), extended));
}

TEST(WebpFormatTest, ChunkLayoutRulesAreChecked) {
    const Bytes alpha = test::makeWebp(kind(WebpKind::LossyWithAlpha));
    const std::size_t vp8x = chunkAt(alpha, "VP8X");
    const std::size_t alph = chunkAt(alpha, "ALPH");
    // ALPH without the VP8 chunk it belongs to.
    const std::size_t vp8 = chunkAt(alpha, "VP8 ");
    Bytes withoutImage = testing::erased(alpha, vp8, alpha.size() - vp8);
    storeLe32(withoutImage, 4, static_cast<std::uint32_t>(withoutImage.size() - 8));
    EXPECT_TRUE(isInvalid(format(), withoutImage, alph));
    // ALPH in a simple file, where there is no VP8X to allow it.
    const Bytes lossy = test::makeWebp(kind(WebpKind::Lossy));
    const Bytes alphChunk(alpha.begin() + static_cast<std::ptrdiff_t>(alph),
                          alpha.begin() + static_cast<std::ptrdiff_t>(vp8));
    Bytes simpleWithAlpha = testing::inserted(lossy, 12, alphChunk);
    storeLe32(simpleWithAlpha, 4, static_cast<std::uint32_t>(simpleWithAlpha.size() - 8));
    EXPECT_TRUE(isInvalid(format(), simpleWithAlpha));
    // An ALPH header naming a method that does not exist.
    EXPECT_TRUE(isInvalid(format(), overwritten(alpha, alph + 8, {0xC3}), alph));
    // A second VP8X, and a second image.
    const Bytes vp8xChunk(alpha.begin() + static_cast<std::ptrdiff_t>(vp8x),
                          alpha.begin() + static_cast<std::ptrdiff_t>(alph));
    Bytes twoVp8x = testing::inserted(alpha, alph, vp8xChunk);
    storeLe32(twoVp8x, 4, static_cast<std::uint32_t>(twoVp8x.size() - 8));
    EXPECT_TRUE(isInvalid(format(), twoVp8x));
    const Bytes imageChunk(lossy.begin() + 12, lossy.end());
    Bytes twoImages = concat({lossy, imageChunk});
    storeLe32(twoImages, 4, static_cast<std::uint32_t>(twoImages.size() - 8));
    EXPECT_TRUE(isInvalid(format(), twoImages));
}

TEST(WebpFormatTest, AnimationChunksAreChecked) {
    const Bytes animation = test::makeWebp(kind(WebpKind::Animated));
    const std::size_t anim = chunkAt(animation, "ANIM");
    const std::size_t anmf = chunkAt(animation, "ANMF");
    // An animation whose frames are gone.
    Bytes noFrames = testing::erased(animation, anmf, animation.size() - anmf);
    storeLe32(noFrames, 4, static_cast<std::uint32_t>(noFrames.size() - 8));
    EXPECT_TRUE(isInvalid(format(), noFrames));
    // A frame before its ANIM chunk.
    Bytes animLast = concat({testing::prefix(animation, anim),
                             std::span<const std::byte>(animation).subspan(anmf),
                             std::span<const std::byte>(animation).subspan(anim, anmf - anim)});
    EXPECT_TRUE(isInvalid(format(), animLast));
    // A frame that reaches beyond the canvas, and a frame without an image.
    Bytes wideFrame = animation;
    wideFrame[anmf + 8 + 6] = std::byte{0x40};
    EXPECT_TRUE(isInvalid(format(), wideFrame));
    Bytes frameImage = animation;
    frameImage[anmf + 8 + 16] = std::byte{'X'};
    EXPECT_TRUE(isInvalid(format(), frameImage));
    // Frames in a still image, and image data in an animation.
    const Bytes still = test::makeWebp(kind(WebpKind::Lossy));
    Bytes stillWithFrame = testing::inserted(still, still.size(),
                                             std::span<const std::byte>(animation).subspan(anmf));
    storeLe32(stillWithFrame, 4, static_cast<std::uint32_t>(stillWithFrame.size() - 8));
    EXPECT_NE(verdictOf(format(), stillWithFrame).status, ValidationStatus::Valid);
}

TEST(WebpFormatTest, FragmentedFilesAreNotAlwaysCaught) {
    // A simple lossy file is one chunk: nothing inside it is checked, so data
    // from another file in the middle of it is not noticed (L65).
    const Bytes lossy = test::makeWebp(kind(WebpKind::Lossy));
    const Bytes overwrittenImage = testing::overwritten(lossy, 40, testing::noise(32, 12));
    EXPECT_EQ(verdictOf(format(), overwrittenImage).status, ValidationStatus::Valid);
    // Data inserted between fragments moves the end of the RIFF data.
    const Bytes fragmented = testing::inserted(lossy, 40, testing::noise(512, 13));
    EXPECT_EQ(endOf(format(), fragmented).length, lossy.size());
    EXPECT_TRUE(isInvalid(format(), fragmented, lossy.size()));
    // In an extended file the chunks after the image data are checked, so the
    // shift is caught.
    WebpOptions options = kind(WebpKind::Lossy);
    options.exifSize = 40;
    options.xmpSize = 40;
    const Bytes extended = test::makeWebp(options);
    const Bytes brokenChunks = testing::inserted(extended, chunkAt(extended, "VP8 ") + 20,
                                                 testing::noise(64, 14));
    EXPECT_NE(verdictOf(format(), brokenChunks).status, ValidationStatus::Valid);
}

TEST(WebpFormatTest, FuzzedFilesStayWithinTheirData) {
    testing::fuzz(format(), test::makeWebp(kind(WebpKind::LossyWithAlpha)), 1500, 51);
    testing::fuzz(format(), test::makeWebp(kind(WebpKind::Animated)), 1500, 52);
    testing::fuzz(format(), test::samples::named("webpmux_metadata.webp").data(), 1000, 53);
}

}  // namespace
}  // namespace recovery::formats
