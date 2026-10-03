// The playability level on Windows: the Windows Imaging Component and Media
// Foundation decode whole files. Builder files and files of independent
// encoders decode; content the Windows decoders are known not to take
// (arithmetic-coded JPEG, PNG inside BMP, compact MP4 sample sizes, AVC
// beyond High) is Unsupported before any decoder runs; a missing codec
// (WebP, HEVC) skips; the level runs only after a sound structure and
// media; reader errors are errors. Decoders conceal much damage, so the
// failures tested here are only those the decoders report.

#include "validation/media_test_helpers.hpp"
#include "validation/png_media_vectors.hpp"
#include "validation/video_media_vectors.hpp"
#include "validation/windows_playability.hpp"

#include "carving/content_reader.hpp"
#include "support/audio_builders.hpp"
#include "support/audio_samples.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"
#include "support/mp4_samples.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

namespace recovery::validation {
namespace {

using testing::Bytes;

LevelResult play(std::span<const std::byte> data, std::string_view format, std::string_view extension) {
    WindowsPlayabilityChecker checker;
    carving::MemoryContentReader content(data);
    Result<LevelResult> result = checker.check(content, format, extension);
    if (!result.ok()) {
        ADD_FAILURE() << "playability failed: " << describe(result.error());
        return {};
    }
    return std::move(*result);
}

::testing::AssertionResult plays(std::span<const std::byte> data, std::string_view format,
                                 std::string_view extension, LevelStatus status, std::string_view detail = {}) {
    const LevelResult result = play(data, format, extension);
    if (result.status != status) {
        return ::testing::AssertionFailure() << "expected " << toString(status) << ", got "
                                             << testing::describe(result);
    }
    if (!detail.empty() && result.detail.find(detail) == std::string::npos) {
        return ::testing::AssertionFailure() << "expected a detail with \"" << detail << "\", got "
                                             << testing::describe(result);
    }
    return ::testing::AssertionSuccess();
}

std::string_view extensionOf(std::string_view format) {
    return format == "jpeg" ? "jpg" : format;
}

// A reader that fails every read after the first `allowed` with Cancelled.
class CancellingReader final : public carving::IContentReader {
public:
    CancellingReader(std::span<const std::byte> data, int allowed) : inner_(data), allowed_(allowed) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return inner_.size(); }
    [[nodiscard]] Result<std::span<const std::byte>> read(std::uint64_t offset, std::size_t length) override {
        if (allowed_-- <= 0) {
            return makeError(ErrorCode::Cancelled, "cancelled by the test");
        }
        return inner_.read(offset, length);
    }

private:
    carving::MemoryContentReader inner_;
    int allowed_;
};

TEST(PlayabilityTest, ImagesOfTheBuildersAndOfIndependentEncodersDecode) {
    EXPECT_TRUE(plays(test::makeJpeg({}), "jpeg", "jpg", LevelStatus::Passed,
                      "1 frame decoded (64x48), 3072 pixels"));
    EXPECT_TRUE(plays(test::makePng({}), "png", "png", LevelStatus::Passed));
    EXPECT_TRUE(plays(test::makeGif({}), "gif", "gif", LevelStatus::Passed));
    EXPECT_TRUE(plays(test::makeBmp({}), "bmp", "bmp", LevelStatus::Passed));
    for (const test::samples::Sample& sample : test::samples::all()) {
        if (sample.format == "webp" || sample.name == "cjpeg_arithmetic.jpg") {
            continue;
        }
        EXPECT_TRUE(plays(sample.data(), sample.format, extensionOf(sample.format), LevelStatus::Passed))
            << sample.name;
    }
    const LevelResult jpeg = play(test::samples::named("cjpeg_progressive.jpg").data(), "jpeg", "jpg");
    EXPECT_EQ(jpeg.checker, "Windows Imaging Component, JPEG Decoder");
}

TEST(PlayabilityTest, WebpDecodesWithItsExtension) {
    if (play(test::makeWebp({}), "webp", "webp").status == LevelStatus::Unsupported) {
        GTEST_SKIP() << "no WebP decoder installed (Microsoft Store: WebP Image Extensions)";
    }
    for (const test::samples::Sample& sample : test::samples::all()) {
        if (sample.format == "webp") {
            EXPECT_TRUE(plays(sample.data(), "webp", "webp", LevelStatus::Passed)) << sample.name;
        }
    }
    EXPECT_TRUE(plays(test::samples::named("img2webp_animated.webp").data(), "webp", "webp", LevelStatus::Passed,
                      "2 frames decoded"));
}

TEST(PlayabilityTest, AudioAndVideoDecode) {
    for (const test::audio_samples::Sample& sample : test::audio_samples::all()) {
        const LevelResult result = play(sample.data(), sample.format, sample.format);
        if (sample.format == "aac" && result.status == LevelStatus::Unsupported) {
            continue;  // no ADTS source on this system
        }
        EXPECT_EQ(result.status, LevelStatus::Passed) << sample.name << ": " << testing::describe(result);
    }
    EXPECT_TRUE(plays(test::makeWav({}), "wav", "wav", LevelStatus::Passed, "0 video frames"));
    EXPECT_TRUE(plays(test::mp4_samples::named("ffmpeg_h264_aac.mp4").data(), "mp4", "mp4", LevelStatus::Passed,
                      "decoded to the end: 5 video frames and 23 audio buffers"));
    for (const test::mp4_samples::Sample& sample : test::mp4_samples::all()) {
        const LevelResult result = play(sample.data(), "mp4", "mp4");
        if (sample.name == "gpac_stz2.mp4" || (sample.name == "ffmpeg_hevc.mp4" &&
                                               result.status == LevelStatus::Unsupported)) {
            continue;  // tested below; no HEVC decoder installed
        }
        EXPECT_EQ(result.status, LevelStatus::Passed) << sample.name << ": " << testing::describe(result);
    }
    for (const std::string_view name : {"avc_cabac_bframes.mp4", "avc_cavlc_baseline.mp4", "avc_interlaced.mp4",
                                        "avc_slices.mp4", "avc_weighted.mp4", "avc_avc3.mp4"}) {
        EXPECT_TRUE(plays(test::video_vectors::named(name).data(), "mp4", "mp4", LevelStatus::Passed,
                          "10 video frames"))
            << name;
    }
}

TEST(PlayabilityTest, KnownGapsAreUnsupportedBeforeDecoding) {
    // WIC fails arithmetic-coded JPEG as a bad image, although it is valid.
    EXPECT_TRUE(plays(test::samples::named("cjpeg_arithmetic.jpg").data(), "jpeg", "jpg", LevelStatus::Unsupported,
                      "not decoded: arithmetic-coded sequential JPEG (SOF9)"));
    // A JPEG of 12-bit samples: SOI, then SOF1 with precision 12.
    const Bytes twelveBit = {std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0xC1}, std::byte{0x00},
                             std::byte{0x0B}, std::byte{12},   std::byte{0},    std::byte{8},    std::byte{0},
                             std::byte{8},    std::byte{1},    std::byte{1},    std::byte{0x11}, std::byte{0}};
    EXPECT_TRUE(plays(twelveBit, "jpeg", "jpg", LevelStatus::Unsupported, "a JPEG of 12-bit samples"));
    // A bitmap of PNG data (BI_PNG, bit count 0).
    Bytes pngInBmp = test::makeBmp({});
    pngInBmp[28] = std::byte{0};
    pngInBmp[29] = std::byte{0};
    pngInBmp[30] = std::byte{5};
    EXPECT_TRUE(plays(pngInBmp, "bmp", "bmp", LevelStatus::Unsupported, "a bitmap of PNG data"));
    // Windows' MPEG-4 source cannot read stz2 (it fails such files with E_FAIL).
    EXPECT_TRUE(plays(test::mp4_samples::named("gpac_stz2.mp4").data(), "mp4", "mp4", LevelStatus::Unsupported,
                      "compact sample sizes (stz2)"));
    // Windows' H.264 decoder accepts AVC beyond High and then delivers nothing.
    EXPECT_TRUE(plays(test::video_vectors::named("avc_422_10bit.mp4").data(), "mp4", "mp4", LevelStatus::Unsupported,
                      "video stream 1: AVC profile 122 (Windows' H.264 decoder takes Baseline, Main and High)"));
    EXPECT_TRUE(plays(test::video_vectors::named("avc_444_10bit.mp4").data(), "mp4", "mp4", LevelStatus::Unsupported,
                      "AVC profile 244"));
}

TEST(PlayabilityTest, HevcDecodesWithItsExtension) {
    const Bytes file = test::mp4_samples::named("ffmpeg_hevc.mp4").data();
    const LevelResult result = play(file, "mp4", "mp4");
    if (result.status == LevelStatus::Unsupported) {
        EXPECT_NE(result.detail.find("video stream 1: no installed decoder"), std::string::npos) << result.detail;
        GTEST_SKIP() << "no HEVC decoder installed (Microsoft Store: HEVC Video Extensions)";
    }
    EXPECT_EQ(result.status, LevelStatus::Passed) << testing::describe(result);
}

TEST(PlayabilityTest, WhatTheDecodersRejectFails) {
    // A GIF whose image descriptor is zeroed: WIC finds no frame.
    Bytes gif = test::samples::named("giflib.gif").data();
    std::fill_n(gif.begin() + static_cast<std::ptrdiff_t>(gif.size() / 3), 64, std::byte{0});
    EXPECT_TRUE(plays(gif, "gif", "gif", LevelStatus::Failed, "(0x88982F62)"));
    // An MP4 whose movie is missing: no media source opens it.
    const Bytes mp4 = test::mp4_samples::named("ffmpeg_h264_aac.mp4").data();
    EXPECT_TRUE(plays(std::span(mp4).first(mp4.size() / 2), "mp4", "mp4", LevelStatus::Failed,
                      "no media source opens the file"));
}

TEST(PlayabilityTest, OtherFormatsAndUnknownContentAreUnsupported) {
    EXPECT_TRUE(plays(Bytes(16), "zip", "zip", LevelStatus::Unsupported, "no Windows decoder is used for zip files"));
    const Bytes garbage(4096, std::byte{0x5A});
    EXPECT_TRUE(plays(garbage, "png", "png", LevelStatus::Unsupported, "no installed decoder takes the file"));
    EXPECT_TRUE(plays(garbage, "mp4", "mp4", LevelStatus::Unsupported, "no installed media source takes the file"));
}

TEST(PlayabilityTest, ValidationRunsPlayabilityAfterSoundStructureAndMedia) {
    WindowsPlayabilityChecker checker;
    ValidationOptions options;
    options.playability = &checker;
    const ValidationState sound = testing::validated("png", test::makePng({}), options);
    EXPECT_EQ(sound.playability.status, LevelStatus::Passed) << testing::describe(sound);
    EXPECT_EQ(sound.status(), carving::ValidationStatus::Valid);
    EXPECT_EQ(sound.deepestPassed(), ValidationLevel::Playability);
    // Media that fail: the platform decoder is not asked (WIC would conceal the damage anyway).
    for (const test::png_vectors::Vector& vector : test::png_vectors::all()) {
        if (vector.name == "block_type_3") {
            const ValidationState failed = testing::validated("png", vector.data(), options);
            EXPECT_EQ(failed.media.status, LevelStatus::Failed) << testing::describe(failed);
            EXPECT_EQ(failed.playability.status, LevelStatus::NotRun);
            EXPECT_EQ(failed.playability.detail, "the media failed");
        }
    }
    // Content cut short: nothing to play to the end.
    const Bytes png = test::makePng({});
    const ValidationState cut = testing::validated("png", std::span(png).first(png.size() - 20), options);
    EXPECT_EQ(cut.structural.status, LevelStatus::Truncated) << testing::describe(cut);
    EXPECT_EQ(cut.playability.status, LevelStatus::NotRun);
    EXPECT_EQ(cut.playability.detail, "the content is cut short");
    EXPECT_EQ(cut.status(), carving::ValidationStatus::Truncated);
    // Not asked for.
    EXPECT_EQ(testing::validated("png", png).playability.detail, "not requested");
}

TEST(PlayabilityTest, ReaderErrorsAreErrors) {
    WindowsPlayabilityChecker checker;
    const auto cancelled = [&](const Bytes& data, std::string_view format) {
        CancellingReader content(data, 0);
        Result<LevelResult> result = checker.check(content, format, format);
        return !result.ok() && result.error().code == ErrorCode::Cancelled;
    };
    EXPECT_TRUE(cancelled(test::makePng({}), "png"));                                     // WIC
    EXPECT_TRUE(cancelled(test::makeWav({}), "wav"));                                     // Media Foundation
    EXPECT_TRUE(cancelled(test::mp4_samples::named("ffmpeg_h264_aac.mp4").data(), "mp4"));  // the MP4 gap check
    EXPECT_TRUE(cancelled(test::samples::named("cjpeg_baseline.jpg").data(), "jpeg"));    // the JPEG gap check
}

TEST(PlayabilityTest, InvalidOptionsAreRejected) {
    WindowsPlayabilityOptions options;
    options.sampleTimeout = std::chrono::milliseconds(0);
    EXPECT_FALSE(validate(options).ok());
    WindowsPlayabilityChecker checker(options);
    const Bytes png = test::makePng({});
    carving::MemoryContentReader content(png);
    Result<LevelResult> result = checker.check(content, "png", "png");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
    EXPECT_TRUE(validate(WindowsPlayabilityOptions{}).ok());
}

}  // namespace
}  // namespace recovery::validation
