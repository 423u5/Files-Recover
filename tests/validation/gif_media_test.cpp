// GIF media validation: the LZW data of builder files (every color depth,
// local tables, frames, interlacing) and of independent encoders; LZW
// streams written here code by code (the string-extension case, a full
// dictionary, a missing end code, extra pixels); foreign data that the
// sub-block walk accepts (L64); truncation, limits, fuzzing.

#include "validation/media_test_helpers.hpp"

#include "formats/format_test_helpers.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace recovery::validation {
namespace {

using testing::Bytes;
using testing::mediaIs;
using testing::mediaOf;
using testing::passesEveryLevel;
using testing::validated;

// Offset of the first image descriptor (0x2C) of a builder file.
std::size_t firstImage(const Bytes& file) {
    std::size_t position = 13;
    const auto packed = static_cast<std::uint8_t>(file[10]);
    if ((packed & 0x80U) != 0) {
        position += 3U << ((packed & 0x07U) + 1);
    }
    while (static_cast<std::uint8_t>(file[position]) == 0x21) {
        position += 2;
        while (file[position] != std::byte{0}) {
            position += 1 + static_cast<std::uint8_t>(file[position]);
        }
        ++position;
    }
    return position;
}

// LZW data from codes, packed with the width a decoder reads them at.
Bytes packCodes(std::uint32_t codeSize, const std::vector<std::uint32_t>& codes) {
    const std::uint32_t clear = 1U << codeSize;
    std::uint32_t next = clear + 2;
    unsigned width = codeSize + 1;
    bool first = true;
    Bytes out;
    std::uint64_t accumulator = 0;
    unsigned bits = 0;
    for (const std::uint32_t code : codes) {
        accumulator |= std::uint64_t{code} << bits;
        bits += width;
        while (bits >= 8) {
            out.push_back(static_cast<std::byte>(accumulator & 0xFF));
            accumulator >>= 8;
            bits -= 8;
        }
        if (code == clear) {
            next = clear + 2;
            width = codeSize + 1;
            first = true;
            continue;
        }
        if (code == clear + 1) {
            continue;
        }
        if (!first && next < 4096) {
            ++next;
        }
        first = false;
        if (next == (1U << width) && width < 12) {
            ++width;
        }
    }
    if (bits > 0) {
        out.push_back(static_cast<std::byte>(accumulator & 0xFF));
    }
    return out;
}

// A GIF89a of one width x height image with this LZW data, a 4-color global table.
Bytes gifWith(std::uint16_t width, std::uint16_t height, std::uint32_t codeSize, const Bytes& lzw) {
    Bytes out;
    for (const char c : std::string_view("GIF89a")) {
        out.push_back(static_cast<std::byte>(c));
    }
    const auto put16 = [&](std::uint16_t value) {
        out.push_back(static_cast<std::byte>(value & 0xFF));
        out.push_back(static_cast<std::byte>(value >> 8));
    };
    put16(width);
    put16(height);
    out.push_back(std::byte{0x81});  // global table of 4 colors
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
    for (int i = 0; i < 12; ++i) {
        out.push_back(static_cast<std::byte>(i * 20));
    }
    out.push_back(std::byte{0x2C});
    put16(0);
    put16(0);
    put16(width);
    put16(height);
    out.push_back(std::byte{0});
    out.push_back(static_cast<std::byte>(codeSize));
    for (std::size_t position = 0; position < lzw.size(); position += 255) {
        const std::size_t length = std::min<std::size_t>(255, lzw.size() - position);
        out.push_back(static_cast<std::byte>(length));
        out.insert(out.end(), lzw.begin() + static_cast<std::ptrdiff_t>(position),
                   lzw.begin() + static_cast<std::ptrdiff_t>(position + length));
    }
    out.push_back(std::byte{0});
    out.push_back(std::byte{0x3B});
    return out;
}

TEST(GifMediaTest, BuilderFilesPass) {
    for (int colorBits = 1; colorBits <= 8; ++colorBits) {
        test::GifOptions options;
        options.colorBits = static_cast<std::uint8_t>(colorBits);
        EXPECT_TRUE(passesEveryLevel("gif", test::makeGif(options))) << colorBits;
    }
    test::GifOptions animated;
    animated.frames = 3;
    animated.localColorTables = true;
    animated.interlaced = true;
    animated.loop = true;
    animated.comment = "frames";
    EXPECT_TRUE(passesEveryLevel("gif", test::makeGif(animated)));
    const LevelResult result = mediaOf("gif", test::makeGif(animated));
    EXPECT_NE(result.detail.find("3 images"), std::string::npos) << result.detail;
    test::GifOptions old;
    old.version89a = false;
    old.graphicControl = false;
    EXPECT_TRUE(passesEveryLevel("gif", test::makeGif(old)));
    test::GifOptions large;
    large.width = 300;
    large.height = 200;
    EXPECT_TRUE(passesEveryLevel("gif", test::makeGif(large)));
    test::GifOptions tiny;
    tiny.width = 1;
    tiny.height = 1;
    EXPECT_TRUE(passesEveryLevel("gif", test::makeGif(tiny)));
}

TEST(GifMediaTest, IndependentEncodersFilesPass) {
    EXPECT_TRUE(passesEveryLevel("gif", test::samples::named("giflib.gif").data()));
    EXPECT_TRUE(passesEveryLevel("gif", test::samples::named("gdiplus.gif").data()));
}

TEST(GifMediaTest, CodesWrittenOneByOne) {
    // clear, a, then the code being defined (a string extended by its own
    // first pixel): 3 pixels.
    EXPECT_TRUE(passesEveryLevel("gif", gifWith(3, 1, 2, packCodes(2, {4, 0, 6, 5}))));
    // No clear code at the start is fine.
    EXPECT_TRUE(passesEveryLevel("gif", gifWith(2, 1, 2, packCodes(2, {1, 2, 5}))));
    // Several clear codes, and a code size of 1.
    EXPECT_TRUE(passesEveryLevel("gif", gifWith(4, 1, 1, packCodes(1, {2, 1, 2, 2, 0, 1, 0, 3}))));
    // A full dictionary: 5000 literals without a clear code (the code width
    // stays at 12 bits once 4096 codes exist, and no entry is added).
    std::vector<std::uint32_t> literals = {256};
    for (int i = 0; i < 5000; ++i) {
        literals.push_back(static_cast<std::uint32_t>(i % 256));
    }
    literals.push_back(257);
    EXPECT_TRUE(passesEveryLevel("gif", gifWith(100, 50, 8, packCodes(8, literals))));

    // No end code after the last pixel: accepted, and said.
    const LevelResult noEnd = mediaOf("gif", gifWith(2, 2, 2, packCodes(2, {4, 0, 1, 2, 3})));
    EXPECT_EQ(noEnd.status, LevelStatus::Passed) << testing::describe(noEnd);
    EXPECT_NE(noEnd.detail.find("without an end-of-information code"), std::string::npos);

    // Too many pixels, too few, a code not yet defined, a dictionary code first.
    EXPECT_TRUE(mediaIs("gif", gifWith(2, 2, 2, packCodes(2, {4, 0, 1, 2, 3, 0, 5})), LevelStatus::Failed,
                        "more than the image's 4 pixels"));
    EXPECT_TRUE(mediaIs("gif", gifWith(2, 2, 2, packCodes(2, {4, 0, 1, 5})), LevelStatus::Failed, "2 of 4 pixels"));
    EXPECT_TRUE(mediaIs("gif", gifWith(2, 2, 2, packCodes(2, {4, 0})), LevelStatus::Failed, "1 of 4 pixels"));
    EXPECT_TRUE(mediaIs("gif", gifWith(4, 1, 2, packCodes(2, {4, 0, 7, 5})), LevelStatus::Failed,
                        "not in the dictionary"));
    EXPECT_TRUE(mediaIs("gif", gifWith(2, 1, 2, packCodes(2, {4, 6, 5})), LevelStatus::Failed,
                        "with no string before it"));
}

TEST(GifMediaTest, ForeignDataTheSubBlockWalkAcceptsIsCaught) {
    // L64: another file's data overwriting part of the image chains on as
    // sub-blocks and reaches the trailer; the LZW data does not decode.
    test::GifOptions options;
    options.width = 64;
    options.height = 64;
    const Bytes file = test::makeGif(options);
    const std::size_t inside = firstImage(file) + 11 + 200;
    const Bytes damaged = formats::testing::overwritten(file, inside, formats::testing::noise(2048, 31));
    const ValidationState state = validated("gif", damaged);
    EXPECT_EQ(state.structural.status, LevelStatus::Passed) << testing::describe(state);
    EXPECT_EQ(state.media.status, LevelStatus::Failed) << testing::describe(state);
    EXPECT_EQ(state.status(), carving::ValidationStatus::Invalid);
}

TEST(GifMediaTest, TheImageSizeMustMatchTheData) {
    const Bytes file = test::makeGif({});
    const std::size_t image = firstImage(file);
    for (const int delta : {-1, 1}) {
        Bytes resized = file;
        const auto width = static_cast<std::uint16_t>(static_cast<std::uint8_t>(file[image + 5]) +
                                                      (static_cast<std::uint8_t>(file[image + 6]) << 8) + delta);
        resized[image + 5] = static_cast<std::byte>(width & 0xFF);
        resized[image + 6] = static_cast<std::byte>(width >> 8);
        EXPECT_EQ(validated("gif", resized).structural.status, LevelStatus::Passed);
        EXPECT_EQ(mediaOf("gif", resized).status, LevelStatus::Failed) << delta;
    }
    // Erased LZW data (0xFF codes) is no dictionary's.
    Bytes erased = file;
    for (std::size_t i = image + 16; i < image + 40; ++i) {
        erased[i] = std::byte{0xFF};
    }
    EXPECT_TRUE(mediaIs("gif", erased, LevelStatus::Failed, "not in the dictionary"));
}

TEST(GifMediaTest, FilesCutShortAreTruncated) {
    test::GifOptions options;
    options.frames = 2;
    const Bytes file = test::makeGif(options);
    for (std::size_t length = 0; length + 1 < file.size(); length += length < 64 ? 1 : 23) {
        const LevelResult result = mediaOf("gif", std::span(file).first(length));
        EXPECT_EQ(result.status, LevelStatus::Truncated) << length << ": " << testing::describe(result);
    }
}

TEST(GifMediaTest, ImagesLargerThanTheLimitAreUnsupported) {
    MediaLimits limits;
    limits.maxDecodedBytes = 100;
    EXPECT_EQ(mediaOf("gif", test::makeGif({}), limits).status, LevelStatus::Unsupported);
}

TEST(GifMediaTest, DamagedFilesNeverBreakTheDecoder) {
    test::GifOptions options;
    options.frames = 2;
    options.localColorTables = true;
    options.comment = "fuzz";
    testing::fuzzMedia("gif", test::makeGif(options), 400, 21);
    testing::fuzzMedia("gif", test::samples::named("giflib.gif").data(), 300, 22);
    testing::fuzzMedia("gif", test::samples::named("gdiplus.gif").data(), 300, 23);
}

}  // namespace
}  // namespace recovery::validation
