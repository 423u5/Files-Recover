// WebP media validation: libwebp's lossy, lossless, alpha, animated and
// metadata files; builder files of every kind; VP8L bitstreams written here
// bit by bit (one rule broken each: transforms, color caches, prefix codes,
// backward references); alpha planes, frames against the canvas, VP8 frame
// headers; truncation, limits, fuzzing.

#include "validation/media_test_helpers.hpp"
#include "validation/webp_media_vectors.hpp"

#include "formats/format_test_helpers.hpp"
#include "recovery/byte_order.hpp"
#include "support/carving_formats.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string_view>
#include <vector>

namespace recovery::validation {
namespace {

using testing::Bytes;
using testing::mediaIs;
using testing::mediaOf;
using testing::passesEveryLevel;
using testing::validated;

// Bits from the least significant end of each byte, as VP8L reads them.
class LsbWriter {
public:
    void put(std::uint32_t value, unsigned bits) {
        for (unsigned i = 0; i < bits; ++i) {
            if (count_ % 8 == 0) {
                bytes_.push_back(std::byte{0});
            }
            if (((value >> i) & 1U) != 0) {
                bytes_.back() |= static_cast<std::byte>(1U << (count_ % 8));
            }
            ++count_;
        }
    }
    // A simple prefix code of one 8-bit symbol (it takes no bits to read).
    void single(std::uint32_t symbol) {
        put(1, 1);
        put(0, 1);
        put(1, 1);
        put(symbol, 8);
    }
    // A normal prefix code giving `symbols` one bit each (two symbols), in an
    // alphabet of `alphabet`: code length code with lengths 0 and 1 only.
    void twoSymbols(std::uint32_t alphabet, std::uint32_t first, std::uint32_t second) {
        put(0, 1);  // normal
        put(0, 4);  // 4 code length code lengths: 17, 18, 0, 1
        put(0, 3);
        put(0, 3);
        put(1, 3);
        put(1, 3);
        put(0, 1);  // every symbol's length follows
        for (std::uint32_t symbol = 0; symbol < alphabet; ++symbol) {
            put(symbol == first || symbol == second ? 1 : 0, 1);
        }
    }
    [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }

private:
    Bytes bytes_;
    unsigned count_ = 0;
};

// A simple lossless WebP of width x height whose image stream `stream` writes.
Bytes losslessWebp(std::uint32_t width, std::uint32_t height, const std::function<void(LsbWriter&)>& stream,
                   std::uint32_t version = 0) {
    LsbWriter bits;
    bits.put(0x2F, 8);
    bits.put(width - 1, 14);
    bits.put(height - 1, 14);
    bits.put(0, 1);
    bits.put(version, 3);
    stream(bits);
    const Bytes chunk = test::riffChunk("VP8L", bits.bytes());
    Bytes file = test::bytesOf("RIFF....WEBP");
    storeLe32(file, 4, static_cast<std::uint32_t>(4 + chunk.size()));
    file.insert(file.end(), chunk.begin(), chunk.end());
    return file;
}

// Where the first chunk of this type starts.
std::size_t chunkAt(const Bytes& file, std::string_view type) {
    const Bytes wanted = test::bytesOf(type);
    return static_cast<std::size_t>(std::search(file.begin(), file.end(), wanted.begin(), wanted.end()) - file.begin());
}

// The image streams below: no transform, no color cache, no meta codes, then the five codes.
void plainCodes(LsbWriter& bits, const std::function<void(LsbWriter&)>& green, std::uint32_t distance = 0) {
    bits.put(0, 1);  // no transform
    bits.put(0, 1);  // no color cache
    bits.put(0, 1);  // no meta prefix codes
    green(bits);
    bits.single(10);   // red
    bits.single(20);   // blue
    bits.single(255);  // alpha
    bits.single(distance);
}

TEST(WebpMediaTest, IndependentEncodersFilesPass) {
    struct Expected {
        const char* name;
        Coverage coverage;
    };
    for (const Expected expected : {Expected{"cwebp_lossless.webp", Coverage::Full},
                                    Expected{"cwebp_lossless_alpha.webp", Coverage::Full},
                                    Expected{"cwebp_lossy.webp", Coverage::Partial},
                                    Expected{"cwebp_alpha.webp", Coverage::Partial},
                                    Expected{"img2webp_animated.webp", Coverage::Partial},
                                    Expected{"webpmux_metadata.webp", Coverage::Partial}}) {
        const Bytes file = test::samples::named(expected.name).data();
        EXPECT_TRUE(passesEveryLevel("webp", file, expected.coverage)) << expected.name;
    }
    const LevelResult alpha = mediaOf("webp", test::samples::named("cwebp_alpha.webp").data());
    EXPECT_NE(alpha.detail.find("1 alpha plane"), std::string::npos) << alpha.detail;
}

TEST(WebpMediaTest, LibwebpFilesExercisingEveryCodingToolPass) {
    for (const test::webp_vectors::Vector& vector : test::webp_vectors::all()) {
        const Bytes file = vector.data();
        const bool lossy = vector.name.starts_with("lossy");
        EXPECT_TRUE(passesEveryLevel("webp", file, lossy ? Coverage::Partial : Coverage::Full)) << vector.name;
    }
    // Meta prefix codes: the entropy image's values choose among 10 groups of codes.
    EXPECT_TRUE(mediaIs("webp", test::webp_vectors::named("lossless_photo.webp").data(), LevelStatus::Passed,
                        "160x120, 2 transforms, no color cache, 10 prefix code groups"));
    EXPECT_TRUE(mediaIs("webp", test::webp_vectors::named("lossless_alpha.webp").data(), LevelStatus::Passed,
                        "a 1-bit color cache, 3 prefix code groups"));
    // Color indexing packs 8, 4 or 2 pixels to one (2, 3 and 16 colors) or none (200 colors).
    for (const char* name : {"lossless_palette2.webp", "lossless_palette3.webp", "lossless_palette16.webp",
                             "lossless_palette200.webp"}) {
        EXPECT_TRUE(mediaIs("webp", test::webp_vectors::named(name).data(), LevelStatus::Passed, "1 transform"))
            << name;
    }
    EXPECT_TRUE(mediaIs("webp", test::webp_vectors::named("lossy_alpha_filtered.webp").data(), LevelStatus::Passed,
                        "1 alpha plane"));
}

TEST(WebpMediaTest, BuilderFilesOfEveryKindPass) {
    for (const test::WebpKind kind : {test::WebpKind::Lossy, test::WebpKind::Lossless,
                                      test::WebpKind::LossyWithAlpha, test::WebpKind::Animated}) {
        for (const bool extended : {false, true}) {
            test::WebpOptions options;
            options.kind = kind;
            options.extended = extended;
            options.iccSize = extended ? 30 : 0;
            options.exifSize = extended ? 11 : 0;
            options.unknownChunk = extended;
            const Bytes file = test::makeWebp(options);
            const Coverage coverage = kind == test::WebpKind::Lossless ? Coverage::Full : Coverage::Partial;
            EXPECT_TRUE(passesEveryLevel("webp", file, coverage)) << static_cast<int>(kind) << " " << extended;
        }
    }
    test::WebpOptions frames;
    frames.kind = test::WebpKind::Animated;
    frames.frames = 4;
    EXPECT_TRUE(mediaIs("webp", test::makeWebp(frames), LevelStatus::Passed, "(4 frames)"));
}

TEST(WebpMediaTest, LosslessBitstreamsWrittenBitByBit) {
    // One opaque pixel: every code has one symbol, so the pixel takes no bits.
    const Bytes one = losslessWebp(1, 1, [](LsbWriter& bits) {
        plainCodes(bits, [](LsbWriter& b) { b.single(128); });
    });
    EXPECT_TRUE(passesEveryLevel("webp", one));
    // Two pixels: a literal, then a copy of it (length symbol 256: 1 pixel;
    // distance symbol 1: code 2, one column back).
    const auto copy = [](std::uint32_t first, std::uint32_t second) {
        return [=](LsbWriter& bits) {
            plainCodes(
                bits, [](LsbWriter& b) { b.twoSymbols(256 + 24, 128, 256); }, 1);
            bits.put(first, 1);
            bits.put(second, 1);
        };
    };
    EXPECT_TRUE(passesEveryLevel("webp", losslessWebp(2, 1, copy(0, 1))));
    // The copy first: nothing to copy from.
    EXPECT_TRUE(mediaIs("webp", losslessWebp(2, 1, copy(1, 0)), LevelStatus::Failed, "before the image"));
    // A copy of 3 pixels (length symbol 258) where 1 is left.
    const Bytes longCopy = losslessWebp(2, 1, [](LsbWriter& bits) {
        plainCodes(
            bits, [](LsbWriter& b) { b.twoSymbols(256 + 24, 128, 258); }, 1);
        bits.put(0, 1);
        bits.put(1, 1);
    });
    EXPECT_TRUE(mediaIs("webp", longCopy, LevelStatus::Failed, "runs past the image"));

    // A transform used twice (subtract green, twice).
    const Bytes twice = losslessWebp(1, 1, [](LsbWriter& bits) {
        bits.put(1, 1);
        bits.put(2, 2);
        bits.put(1, 1);
        bits.put(2, 2);
    });
    EXPECT_TRUE(mediaIs("webp", twice, LevelStatus::Failed, "used twice"));
    // A color cache of 12 bits (at most 11).
    const Bytes cache = losslessWebp(1, 1, [](LsbWriter& bits) {
        bits.put(0, 1);
        bits.put(1, 1);
        bits.put(12, 4);
    });
    EXPECT_TRUE(mediaIs("webp", cache, LevelStatus::Failed, "color cache of 12 bits"));
    // A distance symbol beyond the 40 of its alphabet.
    const Bytes distance =
        losslessWebp(1, 1, [](LsbWriter& bits) { plainCodes(bits, [](LsbWriter& b) { b.single(128); }, 45); });
    EXPECT_TRUE(mediaIs("webp", distance, LevelStatus::Failed, "beyond its alphabet"));
    // A normal code that is not complete: one symbol of length 1 and one of length 2.
    const Bytes incomplete = losslessWebp(1, 1, [](LsbWriter& bits) {
        plainCodes(bits, [](LsbWriter& b) {
            b.put(0, 1);
            b.put(1, 4);  // 5 code length code lengths: 17, 18, 0, 1, 2
            b.put(0, 3);
            b.put(0, 3);
            b.put(1, 3);
            b.put(2, 3);
            b.put(2, 3);
            b.put(0, 1);
            // Code length 0 is '0', 1 is '10', 2 is '11': symbol 5 gets one bit,
            // symbol 6 two, and a quarter of the code space is left over.
            for (std::uint32_t symbol = 0; symbol < 280; ++symbol) {
                if (symbol == 5) {
                    b.put(1, 1);
                    b.put(0, 1);
                } else if (symbol == 6) {
                    b.put(1, 1);
                    b.put(1, 1);
                } else {
                    b.put(0, 1);
                }
            }
        });
    });
    EXPECT_TRUE(mediaIs("webp", incomplete, LevelStatus::Failed, "not a complete prefix code"));
    // VP8L version 1.
    EXPECT_TRUE(mediaIs("webp",
                        losslessWebp(
                            1, 1, [](LsbWriter& bits) { plainCodes(bits, [](LsbWriter& b) { b.single(128); }); }, 1),
                        LevelStatus::Failed, "version 1"));
    // The data ends inside the codes (the chunk is complete).
    const Bytes cut = losslessWebp(1, 1, [](LsbWriter& bits) {
        bits.put(0, 1);
        bits.put(0, 1);
        bits.put(0, 1);
        bits.put(1, 1);
    });
    EXPECT_TRUE(mediaIs("webp", cut, LevelStatus::Failed, "ends inside"));
}

TEST(WebpMediaTest, DamagedLosslessDataIsCaught) {
    // The structure walk checks the chunks and the VP8L header, not the image
    // data: noise over the middle of a lossless image.
    const Bytes file = test::samples::named("cwebp_lossless.webp").data();
    const Bytes damaged =
        formats::testing::overwritten(file, file.size() / 2, formats::testing::noise(file.size() / 4, 9));
    const ValidationState state = validated("webp", damaged);
    EXPECT_EQ(state.structural.status, LevelStatus::Passed) << testing::describe(state);
    EXPECT_EQ(state.media.status, LevelStatus::Failed) << testing::describe(state);
}

TEST(WebpMediaTest, AlphaPlanesAndFramesAreChecked) {
    // The builder's alpha is uncompressed: shorter than the image, it fails.
    test::WebpOptions alpha;
    alpha.kind = test::WebpKind::LossyWithAlpha;
    const Bytes file = test::makeWebp(alpha);
    const std::size_t alph = chunkAt(file, "ALPH");
    ASSERT_LT(alph, file.size());
    Bytes bad = file;
    bad[alph + 8] = std::byte{0xC0};  // reserved bits set
    EXPECT_TRUE(mediaIs("webp", bad, LevelStatus::Failed, "ALPH header"));

    // An animation frame whose bitstream is not the frame's size.
    test::WebpOptions animated;
    animated.kind = test::WebpKind::Animated;
    Bytes frame = test::makeWebp(animated);
    const std::size_t anmf = chunkAt(frame, "ANMF");
    ASSERT_LT(anmf, frame.size());
    frame[anmf + 8 + 6] = static_cast<std::byte>(static_cast<std::uint8_t>(frame[anmf + 8 + 6]) - 1);
    EXPECT_EQ(mediaOf("webp", frame).status, LevelStatus::Failed);
}

TEST(WebpMediaTest, LossyFrameHeadersAreChecked) {
    const Bytes file = test::samples::named("cwebp_lossy.webp").data();
    // The first partition's size beyond the data (the frame tag's upper bits).
    Bytes size = file;
    const std::size_t tag = 20;
    size[tag + 2] = std::byte{0xFF};
    EXPECT_EQ(mediaOf("webp", size).status, LevelStatus::Failed);
    // The start code damaged.
    EXPECT_TRUE(mediaIs("webp", formats::testing::overwritten(file, tag + 3, {0x9D, 0x01, 0x2B}),
                        LevelStatus::Failed, "start code"));
}

TEST(WebpMediaTest, FilesCutShortAreTruncated) {
    for (const char* name : {"cwebp_lossless.webp", "cwebp_lossy.webp", "img2webp_animated.webp"}) {
        const Bytes file = test::samples::named(name).data();
        for (std::size_t length = 0; length + 1 < file.size(); length += length < 64 ? 1 : 29) {
            const LevelResult result = mediaOf("webp", std::span(file).first(length));
            EXPECT_EQ(result.status, LevelStatus::Truncated) << name << " " << length << ": "
                                                              << testing::describe(result);
        }
    }
}

TEST(WebpMediaTest, LimitsMakeLargeImagesUnsupported) {
    MediaLimits limits;
    limits.maxDecodedBytes = 16;
    EXPECT_EQ(mediaOf("webp", test::samples::named("cwebp_lossless.webp").data(), limits).status,
              LevelStatus::Unsupported);
    limits = {};
    limits.maxMemory = 100;
    EXPECT_EQ(mediaOf("webp", test::samples::named("cwebp_lossless.webp").data(), limits).status,
              LevelStatus::Unsupported);
}

TEST(WebpMediaTest, DamagedFilesNeverBreakTheDecoder) {
    for (const test::samples::Sample& sample : test::samples::all()) {
        if (sample.format == "webp") {
            testing::fuzzMedia("webp", sample.data(), 300, sample.bytes.size());
        }
    }
    test::WebpOptions animated;
    animated.kind = test::WebpKind::Animated;
    testing::fuzzMedia("webp", test::makeWebp(animated), 300, 51);
}

}  // namespace
}  // namespace recovery::validation
