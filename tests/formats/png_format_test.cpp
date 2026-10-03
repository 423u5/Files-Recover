// PNG: intact files from the builder (every color type and bit depth,
// interlaced, split image data, ancillary chunks, APNG) and from real
// encoders; truncation at every position; header rejection; damaged chunk
// lengths, types, CRCs and order; overwritten and fragmented image data;
// fuzzing.

#include "formats/png_format.hpp"

#include "format_test_helpers.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/crc32.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::ValidationStatus;
using test::PngColor;
using test::PngOptions;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::isIntact;
using testing::isInvalid;
using testing::overwritten;
using testing::verdictOf;

const PngFormat& format() {
    static const PngFormat instance;
    return instance;
}

// The `skip`-th chunk of this type.
test::PngChunkPosition chunkOf(const Bytes& file, std::string_view type, std::size_t skip = 0) {
    for (const test::PngChunkPosition& chunk : test::pngChunks(file)) {
        if (chunk.type == type && skip-- == 0) {
            return chunk;
        }
    }
    ADD_FAILURE() << "no chunk " << type;
    return {};
}

TEST(PngFormatTest, DescriptorIsValid) {
    RECOVERY_EXPECT_OK(carving::validateDescriptor(format().descriptor()));
    EXPECT_EQ(format().descriptor().id, "png");
    EXPECT_EQ(format().descriptor().extension, "png");
    EXPECT_EQ(format().descriptor().endDetection, carving::EndDetectionMethod::StructureWalk);
}

TEST(PngFormatTest, EveryColorTypeAndBitDepthIsIntact) {
    const std::vector<std::pair<PngColor, std::vector<int>>> combinations = {
        {PngColor::Gray, {1, 2, 4, 8, 16}},
        {PngColor::Rgb, {8, 16}},
        {PngColor::Palette, {1, 2, 4, 8}},
        {PngColor::GrayAlpha, {8, 16}},
        {PngColor::Rgba, {8, 16}},
    };
    for (const auto& [color, depths] : combinations) {
        for (const int depth : depths) {
            for (const bool interlaced : {false, true}) {
                PngOptions options;
                options.color = color;
                options.bitDepth = static_cast<std::uint8_t>(depth);
                options.interlaced = interlaced;
                options.width = 13;
                options.height = 9;
                SCOPED_TRACE("color type " + std::to_string(static_cast<int>(color)) + ", depth " +
                             std::to_string(depth) + (interlaced ? ", interlaced" : ""));
                EXPECT_TRUE(isIntact(format(), test::makePng(options)));
            }
        }
    }
}

TEST(PngFormatTest, SplitImageDataAncillaryChunksAndApngAreIntact) {
    PngOptions options;
    options.idatSize = 100;  // many IDAT chunks
    options.textChunks = true;
    const Bytes split = test::makePng(options);
    EXPECT_TRUE(isIntact(format(), split));
    const std::vector<test::PngChunkPosition> chunks = test::pngChunks(split);
    EXPECT_GT(std::count_if(chunks.begin(), chunks.end(),
                            [](const test::PngChunkPosition& c) { return c.type == "IDAT"; }),
              3);
    options = PngOptions{};
    options.animated = true;
    EXPECT_TRUE(isIntact(format(), test::makePng(options)));
    // A 1x1 image is the smallest file the format accepts.
    options = PngOptions{};
    options.width = 1;
    options.height = 1;
    const Bytes tiny = test::makePng(options);
    EXPECT_TRUE(isIntact(format(), tiny));
    EXPECT_GE(tiny.size(), format().descriptor().minimumSize);
}

TEST(PngFormatTest, FilesFromRealEncodersAreIntact) {
    std::size_t checked = 0;
    for (const test::samples::Sample& sample : test::samples::all()) {
        if (sample.format != "png") {
            continue;
        }
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes file = sample.data();
        EXPECT_TRUE(isIntact(format(), file));
        EXPECT_TRUE(testing::prefixesAreTruncated(format(), file));
        ++checked;
    }
    EXPECT_GE(checked, 3u);  // libpng (dwebp) and GDI+, with and without alpha
}

TEST(PngFormatTest, EveryPrefixIsTruncated) {
    PngOptions options;
    options.idatSize = 64;
    options.textChunks = true;
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), test::makePng(options)));
}

TEST(PngFormatTest, BytesAfterIendMakeTheContentInvalid) {
    const Bytes file = test::makePng();
    const Bytes longer = concat({file, testing::noise(64, 3)});
    EXPECT_TRUE(isInvalid(format(), longer, file.size()));
    EXPECT_EQ(endOf(format(), longer).length, file.size());
}

TEST(PngFormatTest, HeaderCheckRejectsFalseSignatures) {
    const Bytes file = test::makePng();
    const std::vector<std::pair<Bytes, std::string>> cases = {
        {overwritten(file, 1, {'X'}), "damaged signature"},
        {overwritten(file, 11, {12}), "IHDR length is not 13"},
        {overwritten(file, 13, {'h'}), "first chunk is not IHDR"},
        {overwritten(file, 24, {3}), "bit depth 3"},
        {overwritten(file, 25, {1}), "color type 1"},
        {overwritten(file, 26, {1}), "compression method 1"},
        {overwritten(file, 27, {1}), "filter method 1"},
        {overwritten(file, 28, {2}), "interlace method 2"},
        {overwritten(file, 16, {0, 0, 0, 0}), "width 0"},
        {overwritten(file, 32, {0xFF}), "IHDR CRC mismatch"},
        {testing::prefix(file, 20), "header cut short"},
    };
    for (const auto& [bytes, what] : cases) {
        const carving::HeaderCheck check = testing::headerOf(format(), bytes);
        EXPECT_FALSE(check.plausible) << what;
    }
    EXPECT_TRUE(testing::headerOf(format(), file).plausible);
}

TEST(PngFormatTest, DamagedChunkHeadersBreakTheStructure) {
    const Bytes file = test::makePng();
    const test::PngChunkPosition idat = chunkOf(file, "IDAT");
    // A length beyond 2^31-1, and a type that is not four letters.
    Bytes huge = file;
    storeBe32(huge, idat.offset, 0x80000000);
    carving::EndDetection end = endOf(format(), huge);
    EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
    EXPECT_EQ(end.length, idat.offset);
    EXPECT_TRUE(isInvalid(format(), huge, idat.offset));

    const Bytes badType = overwritten(file, idat.offset + 4, {0x01});
    end = endOf(format(), badType);
    EXPECT_EQ(end.status, EndStatus::Broken);
    EXPECT_EQ(end.length, idat.offset);

    // A length that points past the end of the data: the file is cut short.
    Bytes beyond = file;
    storeBe32(beyond, idat.offset, 0x7FFFFFFF);
    end = endOf(format(), beyond);
    EXPECT_EQ(end.status, EndStatus::Truncated);
    EXPECT_EQ(verdictOf(format(), beyond).status, ValidationStatus::Truncated);
}

TEST(PngFormatTest, ChunkCrcMismatchesAreInvalidButTheFileStillEnds) {
    PngOptions options;
    options.idatSize = 200;
    const Bytes file = test::makePng(options);
    const test::PngChunkPosition idat = chunkOf(file, "IDAT", 1);
    // One damaged byte inside the chunk's data: the lengths still lead to IEND.
    const Bytes damaged = overwritten(file, idat.offset + 20, {0x5A});
    const carving::EndDetection end = endOf(format(), damaged);
    EXPECT_EQ(end.status, EndStatus::Found) << testing::describe(end);
    EXPECT_EQ(end.length, file.size());
    const carving::ValidationResult verdict = verdictOf(format(), damaged);
    EXPECT_EQ(verdict.status, ValidationStatus::Invalid);
    EXPECT_EQ(verdict.validBytes, idat.offset);
    EXPECT_NE(verdict.detail.find("CRC"), std::string::npos) << verdict.detail;
    // A damaged CRC field itself is caught the same way.
    EXPECT_TRUE(isInvalid(format(), overwritten(file, idat.offset + 8 + idat.length, {0xFF}), idat.offset));
}

TEST(PngFormatTest, ChunkOrderAndRequiredChunksAreChecked) {
    const Bytes file = test::makePng();
    const test::PngChunkPosition ihdr = chunkOf(file, "IHDR");
    const test::PngChunkPosition idat = chunkOf(file, "IDAT");
    const test::PngChunkPosition iend = chunkOf(file, "IEND");

    // IEND with data, and no image data at all.
    Bytes withData = concat({testing::prefix(file, iend.offset), test::pngChunk("IEND", testing::noise(4, 1))});
    EXPECT_TRUE(isInvalid(format(), withData));
    const Bytes noImage = concat({testing::prefix(file, idat.offset),
                                  std::span<const std::byte>(file).subspan(iend.offset)});
    EXPECT_EQ(endOf(format(), noImage).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), noImage));

    // A palette image without PLTE, and PLTE after the image data.
    PngOptions options;
    options.color = PngColor::Palette;
    options.bitDepth = 4;
    const Bytes palette = test::makePng(options);
    ASSERT_TRUE(isIntact(format(), palette));
    const test::PngChunkPosition plte = chunkOf(palette, "PLTE");
    const Bytes withoutPlte = testing::erased(palette, plte.offset, 12 + plte.length);
    EXPECT_TRUE(isInvalid(format(), withoutPlte));
    const Bytes plteChunk(palette.begin() + static_cast<std::ptrdiff_t>(plte.offset),
                          palette.begin() + static_cast<std::ptrdiff_t>(plte.offset + 12 + plte.length));
    const test::PngChunkPosition paletteEnd = chunkOf(palette, "IEND");
    EXPECT_TRUE(isInvalid(format(), testing::inserted(palette, paletteEnd.offset, plteChunk)));

    // IDAT chunks that are not consecutive, and a second IHDR.
    PngOptions split;
    split.idatSize = 100;
    const Bytes many = test::makePng(split);
    const test::PngChunkPosition second = chunkOf(many, "IDAT", 1);
    const Bytes interrupted = testing::inserted(many, second.offset, test::pngChunk("tEXt", testing::noise(4, 2)));
    EXPECT_EQ(endOf(format(), interrupted).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), interrupted));
    const Bytes ihdrChunk(file.begin() + static_cast<std::ptrdiff_t>(ihdr.offset),
                          file.begin() + static_cast<std::ptrdiff_t>(ihdr.offset + 12 + ihdr.length));
    EXPECT_TRUE(isInvalid(format(), testing::inserted(file, idat.offset, ihdrChunk)));
}

TEST(PngFormatTest, UnknownChunksAreJudgedByTheirCriticalBit) {
    const Bytes file = test::makePng();
    const test::PngChunkPosition idat = chunkOf(file, "IDAT");
    const Bytes unknownAncillary = testing::inserted(file, idat.offset, test::pngChunk("prVt", testing::noise(8, 4)));
    EXPECT_TRUE(isIntact(format(), unknownAncillary));
    const Bytes unknownCritical = testing::inserted(file, idat.offset, test::pngChunk("PrVt", testing::noise(8, 4)));
    EXPECT_EQ(endOf(format(), unknownCritical).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), unknownCritical));
}

TEST(PngFormatTest, ImageDataMustStartAZlibStream) {
    const Bytes file = test::makePng();
    const test::PngChunkPosition idat = chunkOf(file, "IDAT");
    // The zlib header is the first two bytes of the image data; the chunk's CRC is fixed up.
    Bytes damaged = overwritten(file, idat.offset + 8, {0x00, 0x00});
    const std::uint32_t crc = crc32(std::span<const std::byte>(damaged).subspan(idat.offset + 4, 4 + idat.length));
    storeBe32(damaged, idat.offset + 8 + idat.length, crc);
    EXPECT_EQ(endOf(format(), damaged).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), damaged));
    EXPECT_NE(verdictOf(format(), damaged).detail.find("zlib"), std::string::npos);
}

TEST(PngFormatTest, FragmentedAndOverwrittenImageDataIsNotValid) {
    PngOptions options;
    options.width = 64;
    options.height = 64;
    options.idatSize = 1024;
    const Bytes file = test::makePng(options);
    const test::PngChunkPosition idat = chunkOf(file, "IDAT", 1);
    // Another file's data in place of one cluster: the chunk's CRC fails.
    const Bytes overwrittenData = testing::overwritten(file, idat.offset + 8, testing::noise(512, 9));
    EXPECT_EQ(endOf(format(), overwrittenData).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), overwrittenData, idat.offset));
    // Data inserted between two fragments shifts every following chunk header.
    const Bytes fragmented = testing::inserted(file, idat.offset + 8, testing::noise(4096, 10));
    EXPECT_NE(endOf(format(), fragmented).status, EndStatus::Found);
    EXPECT_NE(verdictOf(format(), fragmented).status, ValidationStatus::Valid);
    // Zeros between the fragments are caught the same way (a chunk type of zeros is not letters).
    const Bytes zeros = testing::inserted(file, idat.offset + 8, Bytes(4096, std::byte{0}));
    EXPECT_NE(verdictOf(format(), zeros).status, ValidationStatus::Valid);
}

TEST(PngFormatTest, FuzzedFilesStayWithinTheirData) {
    PngOptions options;
    options.idatSize = 128;
    options.textChunks = true;
    testing::fuzz(format(), test::makePng(options), 1500, 21);
    options = PngOptions{};
    options.color = PngColor::Palette;
    options.bitDepth = 2;
    options.interlaced = true;
    testing::fuzz(format(), test::makePng(options), 1000, 22);
}

TEST(PngFormatTest, HostileChunkLengthsEndQuickly) {
    // Chunks of length 0 in a row, and a chunk whose length reaches the limit.
    Bytes many = testing::prefix(test::makePng(), 33);
    for (int i = 0; i < 50'000; ++i) {
        const Bytes chunk = test::pngChunk("prVt", {});
        many.insert(many.end(), chunk.begin(), chunk.end());
    }
    EXPECT_EQ(endOf(format(), many).status, EndStatus::Truncated);
    EXPECT_EQ(verdictOf(format(), many).status, ValidationStatus::Truncated);
}

}  // namespace
}  // namespace recovery::formats
