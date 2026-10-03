// PNG media validation: the zlib streams of builder files of every color
// type, depth and interlace method, of APNG animations and of independent
// encoders; zlib streams of every block type and strategy, and DEFLATE bit
// streams that break one rule each (png_media_vectors.cpp); damage the chunk
// structure cannot see (chunk CRCs corrected); truncation, limits, fuzzing.

#include "validation/media_test_helpers.hpp"
#include "validation/png_media_vectors.hpp"

#include "formats/format_test_helpers.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/crc32.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace recovery::validation {
namespace {

using testing::Bytes;
using testing::mediaIs;
using testing::mediaOf;
using testing::passesEveryLevel;
using testing::validated;

const test::PngChunkPosition& chunkOf(const std::vector<test::PngChunkPosition>& chunks, std::string_view type,
                                      std::size_t index = 0) {
    for (const test::PngChunkPosition& chunk : chunks) {
        if (chunk.type == type && index-- == 0) {
            return chunk;
        }
    }
    throw std::runtime_error("no such chunk");
}

// Corrects a chunk's CRC after its data was changed, so the structure stays valid.
void fixCrc(Bytes& file, const test::PngChunkPosition& chunk) {
    const std::uint32_t crc = crc32(std::span(file).subspan(chunk.offset + 4, 4 + chunk.length));
    storeBe32(file, chunk.offset + 8 + chunk.length, crc);
}

// The offset of the first scanline byte in a builder file (one IDAT, stored blocks).
std::size_t firstScanlineByte(const Bytes& file) {
    return chunkOf(test::pngChunks(file), "IDAT").offset + 8 + 2 + 5;
}

TEST(PngMediaTest, BuilderFilesOfEveryColorTypeDepthAndInterlaceMethodPass) {
    const std::vector<std::pair<test::PngColor, std::vector<std::uint8_t>>> kinds = {
        {test::PngColor::Gray, {1, 2, 4, 8, 16}}, {test::PngColor::Rgb, {8, 16}},
        {test::PngColor::Palette, {1, 2, 4, 8}},  {test::PngColor::GrayAlpha, {8, 16}},
        {test::PngColor::Rgba, {8, 16}},
    };
    for (const auto& [color, depths] : kinds) {
        for (const std::uint8_t depth : depths) {
            for (const bool interlaced : {false, true}) {
                test::PngOptions options;
                options.color = color;
                options.bitDepth = depth;
                options.interlaced = interlaced;
                options.width = 37;
                options.height = 13;
                const Bytes file = test::makePng(options);
                EXPECT_TRUE(passesEveryLevel("png", file))
                    << "color " << static_cast<int>(color) << ", depth " << int{depth} << ", interlaced "
                    << interlaced;
            }
        }
    }
}

TEST(PngMediaTest, ImageDataSplitOverManyChunksAndOneByOneImages) {
    for (const std::size_t idatSize : {std::size_t{1}, std::size_t{7}, std::size_t{100}}) {
        test::PngOptions options;
        options.idatSize = idatSize;
        options.textChunks = true;
        EXPECT_TRUE(passesEveryLevel("png", test::makePng(options))) << idatSize;
    }
    for (const bool interlaced : {false, true}) {
        test::PngOptions options;
        options.width = 1;
        options.height = 1;
        options.interlaced = interlaced;
        EXPECT_TRUE(passesEveryLevel("png", test::makePng(options)));
    }
    // A large image: more than one stored block of 65535 bytes.
    test::PngOptions large;
    large.width = 300;
    large.height = 200;
    large.color = test::PngColor::Rgba;
    EXPECT_TRUE(passesEveryLevel("png", test::makePng(large)));
}

TEST(PngMediaTest, IndependentEncodersFilesPass) {
    std::size_t checked = 0;
    for (const test::samples::Sample& sample : test::samples::all()) {
        if (sample.format != "png") {
            continue;
        }
        EXPECT_TRUE(passesEveryLevel("png", sample.data())) << sample.name;
        ++checked;
    }
    EXPECT_GE(checked, 3U);
}

TEST(PngMediaTest, ZlibStreamsOfEveryKindAndRuleBreakers) {
    for (const test::png_vectors::Vector& vector : test::png_vectors::all()) {
        const Bytes data = vector.data();
        const LevelStatus status = vector.expect == "passed"   ? LevelStatus::Passed
                                   : vector.expect == "failed" ? LevelStatus::Failed
                                                               : LevelStatus::Truncated;
        EXPECT_TRUE(mediaIs("png", data, status, vector.detail)) << vector.name;
        if (status == LevelStatus::Failed) {
            // The chunks are intact: only the media level sees the damage,
            // except in the zlib header, which the structure checks itself.
            const bool header =
                vector.name == "preset_dictionary" || vector.name == "header_check" || vector.name == "not_deflate";
            const ValidationState state = validated("png", data);
            EXPECT_EQ(state.structural.status, header ? LevelStatus::Failed : LevelStatus::Passed)
                << vector.name << "\n"
                << testing::describe(state);
            EXPECT_EQ(state.status(), carving::ValidationStatus::Invalid) << vector.name;
            EXPECT_EQ(state.playability.status, LevelStatus::NotRun);
        }
    }
}

TEST(PngMediaTest, DataTheChunkStructureCannotCheckIsChecked) {
    const Bytes original = test::makePng({});
    const std::vector<test::PngChunkPosition> chunks = test::pngChunks(original);
    const test::PngChunkPosition& idat = chunkOf(chunks, "IDAT");

    // A pixel byte changed: the Adler-32 no longer matches.
    Bytes pixel = original;
    pixel[firstScanlineByte(pixel) + 5] ^= std::byte{0x40};
    fixCrc(pixel, idat);
    EXPECT_TRUE(passesEveryLevel("png", original));
    EXPECT_EQ(validated("png", pixel).structural.status, LevelStatus::Passed);
    EXPECT_TRUE(mediaIs("png", pixel, LevelStatus::Failed, "Adler-32"));

    // A filter type that does not exist.
    Bytes filter = original;
    const std::size_t rowBytes = 1 + 32 * 3;
    filter[firstScanlineByte(filter) + 2 * rowBytes] = std::byte{9};
    fixCrc(filter, idat);
    EXPECT_TRUE(mediaIs("png", filter, LevelStatus::Failed, "row 2 has the filter type 9"));

    // IHDR says the image is wider (or taller) than the data holds.
    for (const std::size_t field : {std::size_t{0}, std::size_t{4}}) {
        Bytes resized = original;
        const test::PngChunkPosition& ihdr = chunkOf(chunks, "IHDR");
        storeBe32(resized, ihdr.offset + 8 + field, loadBe32(resized, ihdr.offset + 8 + field) + 1);
        fixCrc(resized, ihdr);
        EXPECT_EQ(validated("png", resized).structural.status, LevelStatus::Passed);
        EXPECT_EQ(mediaOf("png", resized).status, LevelStatus::Failed) << field;
    }
    // ... or narrower: data is left over.
    Bytes narrower = original;
    const test::PngChunkPosition& ihdr = chunkOf(chunks, "IHDR");
    storeBe32(narrower, ihdr.offset + 8, 31);
    fixCrc(narrower, ihdr);
    EXPECT_EQ(mediaOf("png", narrower).status, LevelStatus::Failed);
}

TEST(PngMediaTest, AnimationFramesAreDecodedAndTheirSequenceChecked) {
    test::PngOptions options;
    options.animated = true;
    const Bytes original = test::makePng(options);
    EXPECT_TRUE(passesEveryLevel("png", original));
    const LevelResult passed = mediaOf("png", original);
    EXPECT_NE(passed.detail.find("2 zlib streams"), std::string::npos) << passed.detail;

    const std::vector<test::PngChunkPosition> chunks = test::pngChunks(original);
    // The second frame's data damaged: its own stream fails.
    Bytes frame = original;
    const test::PngChunkPosition& fdat = chunkOf(chunks, "fdAT");
    frame[fdat.offset + 8 + 4 + 2 + 5 + 3] ^= std::byte{0x01};
    fixCrc(frame, fdat);
    EXPECT_TRUE(mediaIs("png", frame, LevelStatus::Failed, "frame 2's zlib stream"));

    // A sequence number out of order.
    Bytes sequence = original;
    storeBe32(sequence, fdat.offset + 8, 7);
    fixCrc(sequence, fdat);
    EXPECT_TRUE(mediaIs("png", sequence, LevelStatus::Failed, "animation chunk 7"));

    // acTL announcing a frame more than there is.
    Bytes count = original;
    const test::PngChunkPosition& actl = chunkOf(chunks, "acTL");
    storeBe32(count, actl.offset + 8, 3);
    fixCrc(count, actl);
    EXPECT_TRUE(mediaIs("png", count, LevelStatus::Failed, "acTL announces 3 frames"));

    // A frame larger than the canvas.
    Bytes larger = original;
    const test::PngChunkPosition& second = chunkOf(chunks, "fcTL", 1);
    storeBe32(larger, second.offset + 8 + 4, 33);
    fixCrc(larger, second);
    EXPECT_TRUE(mediaIs("png", larger, LevelStatus::Failed, "control"));
}

TEST(PngMediaTest, FilesCutShortAreTruncatedNeverPassed) {
    test::PngOptions options;
    options.idatSize = 200;
    const Bytes file = test::makePng(options);
    const std::size_t end = file.size() - 12;  // IEND
    for (std::size_t length = 0; length < end; length += length < 64 ? 1 : 37) {
        const LevelResult result = mediaOf("png", std::span(file).first(length));
        EXPECT_TRUE(result.status == LevelStatus::Truncated || result.status == LevelStatus::Failed)
            << length << ": " << testing::describe(result);
        if (length > 8 + 25 + 8) {
            EXPECT_EQ(result.status, LevelStatus::Truncated) << length << ": " << testing::describe(result);
        }
    }
    // Without IEND but with every scanline: truncated after the image data.
    EXPECT_TRUE(mediaIs("png", std::span(file).first(end), LevelStatus::Truncated, "before IEND"));
}

TEST(PngMediaTest, ImagesLargerThanTheLimitsAreUnsupported) {
    // A small file can claim a huge image: 60000 x 60000 RGBA (14 GB of scanlines).
    Bytes file = test::makePng({});
    const std::vector<test::PngChunkPosition> chunks = test::pngChunks(file);
    const test::PngChunkPosition& ihdr = chunkOf(chunks, "IHDR");
    storeBe32(file, ihdr.offset + 8, 60000);
    storeBe32(file, ihdr.offset + 12, 60000);
    file[ihdr.offset + 8 + 9] = std::byte{6};
    fixCrc(file, ihdr);
    EXPECT_TRUE(mediaIs("png", file, LevelStatus::Unsupported, "decoding limit"));

    MediaLimits limits;
    limits.maxDecodedBytes = 100;
    EXPECT_EQ(mediaOf("png", test::makePng({}), limits).status, LevelStatus::Unsupported);
    limits = {};
    limits.maxMemory = 8;
    EXPECT_EQ(mediaOf("png", test::makePng({}), limits).status, LevelStatus::Unsupported);
}

TEST(PngMediaTest, DamagedFilesNeverBreakTheDecoder) {
    testing::fuzzMedia("png", test::makePng({}), 300, 11);
    test::PngOptions interlaced;
    interlaced.interlaced = true;
    interlaced.idatSize = 50;
    testing::fuzzMedia("png", test::makePng(interlaced), 300, 12);
    for (const test::png_vectors::Vector& vector : test::png_vectors::all()) {
        if (vector.expect == "passed") {
            testing::fuzzMedia("png", vector.data(), 60, vector.bytes.size());
        }
    }
    for (const test::samples::Sample& sample : test::samples::all()) {
        if (sample.format == "png") {
            testing::fuzzMedia("png", sample.data(), 200, sample.bytes.size());
        }
    }
}

}  // namespace
}  // namespace recovery::validation
