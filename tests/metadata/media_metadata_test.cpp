// Media metadata (P17) on files the test builders write, and on files put
// together here where the builders do not reach (ID3 frames of every
// version and encoding, MP4 user data, patched MP4 headers):
//
//  * metadata extraction: every format's technical metadata, tags,
//    thumbnails and cover art, previews and their bytes;
//  * missing metadata: files without tags, Exif or header times give empty
//    fields, never made-up ones;
//  * options and limits.
//
// Invalid media (damage, truncation, fuzzing) are in
// metadata_robustness_test.cpp, files of independent writers in
// metadata_reference_test.cpp.

#include "metadata/media_metadata.hpp"

#include "carving/content_reader.hpp"
#include "metadata_test_helpers.hpp"
#include "recovery/byte_order.hpp"
#include "support/audio_builders.hpp"
#include "support/image_builders.hpp"
#include "support/mp4_builders.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace recovery::metadata {
namespace {

using namespace std::chrono_literals;
using test::Bytes;
using test::extract;
using test::issuesText;

const PreviewSource* previewOf(const MediaMetadata& metadata, PreviewKind kind, std::size_t index = 0) {
    for (const PreviewSource& preview : metadata.previews) {
        if (preview.kind == kind && index-- == 0) {
            return &preview;
        }
    }
    return nullptr;
}

// The content preview is the whole file, last.
void expectContentPreview(const MediaMetadata& metadata, std::size_t size, std::uint32_t width = 0,
                          std::uint32_t height = 0) {
    ASSERT_FALSE(metadata.previews.empty());
    const PreviewSource& preview = metadata.previews.back();
    EXPECT_EQ(preview.kind, PreviewKind::Content);
    EXPECT_EQ(preview.formatId, metadata.formatId);
    EXPECT_EQ(preview.mediaType, metadata.mediaType);
    EXPECT_EQ(preview.offset, 0U);
    EXPECT_EQ(preview.length, size);
    EXPECT_EQ(preview.width, width);
    EXPECT_EQ(preview.height, height);
}

MediaDuration exactly(std::uint64_t units, std::uint64_t scale) {
    return MediaDuration{static_cast<MediaDuration::rep>((units * 1'000'000 + scale / 2) / scale)};
}

// ---------------------------------------------------------------------------
// Kinds, media types, dates, orientations
// ---------------------------------------------------------------------------

TEST(MetadataTypesTest, FormatsHaveKindsAndMediaTypes) {
    EXPECT_EQ(kindOfFormat("jpeg"), MediaKind::Image);
    EXPECT_EQ(kindOfFormat("webp"), MediaKind::Image);
    EXPECT_EQ(kindOfFormat("mp3"), MediaKind::Audio);
    EXPECT_EQ(kindOfFormat("m4a"), MediaKind::Audio);
    EXPECT_EQ(kindOfFormat("mp4"), MediaKind::Video);
    EXPECT_EQ(kindOfFormat(""), MediaKind::Unknown);
    EXPECT_EQ(kindOfFormat("docx"), MediaKind::Unknown);
    EXPECT_EQ(mediaTypeOfFormat("jpeg"), "image/jpeg");
    EXPECT_EQ(mediaTypeOfFormat("wav"), "audio/wav");
    EXPECT_EQ(mediaTypeOfFormat("aac"), "audio/aac");
    EXPECT_EQ(mediaTypeOfFormat("mp4"), "video/mp4");
    EXPECT_EQ(mediaTypeOfFormat("unknown"), "");
}

TEST(MetadataTypesTest, DatesPrintToTheirPrecisionAndZone) {
    MediaDateTime date;
    date.year = 2024;
    EXPECT_EQ(date.iso8601(), "2024");
    EXPECT_FALSE(date.utc().has_value());
    date.month = 5;
    date.day = 17;
    date.precision = MediaDateTime::Precision::Day;
    EXPECT_EQ(date.iso8601(), "2024-05-17");
    date.hour = 14;
    date.minute = 23;
    date.second = 5;
    date.precision = MediaDateTime::Precision::Second;
    EXPECT_EQ(date.iso8601(), "2024-05-17T14:23:05");
    date.zone = MediaDateTime::Zone::Offset;
    date.offsetMinutes = 120;
    EXPECT_EQ(date.iso8601(), "2024-05-17T14:23:05+02:00");
    ASSERT_TRUE(date.utc().has_value());
    EXPECT_EQ(date.utc()->time_since_epoch(), 1715948585s);
    date.offsetMinutes = -330;
    EXPECT_EQ(date.iso8601(), "2024-05-17T14:23:05-05:30");
    date.zone = MediaDateTime::Zone::Utc;
    EXPECT_EQ(date.iso8601(), "2024-05-17T14:23:05Z");
    EXPECT_EQ(date.utc()->time_since_epoch(), 1715955785s);
    date.precision = MediaDateTime::Precision::Minute;
    EXPECT_EQ(date.iso8601(), "2024-05-17T14:23Z");
}

TEST(MetadataTypesTest, OrientationsTurnAndMirror) {
    EXPECT_EQ(rotationOf(Orientation::Normal), 0);
    EXPECT_EQ(rotationOf(Orientation::Rotate90), 90);
    EXPECT_EQ(rotationOf(Orientation::Rotate180), 180);
    EXPECT_EQ(rotationOf(Orientation::Rotate270), 270);
    EXPECT_EQ(rotationOf(Orientation::MirrorRotate90), 90);
    EXPECT_EQ(rotationOf(Orientation::MirrorRotate270), 270);
    EXPECT_FALSE(mirrored(Orientation::Rotate90));
    EXPECT_TRUE(mirrored(Orientation::Mirror));
    EXPECT_TRUE(mirrored(Orientation::MirrorRotate180));
    EXPECT_EQ(orientationForRotation(90), Orientation::Rotate90);
    EXPECT_EQ(orientationForRotation(0), Orientation::Normal);
    EXPECT_FALSE(orientationForRotation(45).has_value());
}

TEST(MetadataTypesTest, OptionsMustNotBeZero) {
    MetadataOptions options;
    RECOVERY_EXPECT_OK(validate(options));
    options.maxScanBytes = 0;
    RECOVERY_EXPECT_ERROR(validate(options), ErrorCode::InvalidInput);
    options = {};
    options.maxTextLength = 0;
    RECOVERY_EXPECT_ERROR(validate(options), ErrorCode::InvalidInput);
    options = {};
    options.mp4.maxBoxes = 0;
    RECOVERY_EXPECT_ERROR(validate(options), ErrorCode::InvalidInput);
    const Bytes jpeg = ::recovery::test::makeJpeg({});
    carving::MemoryContentReader reader(jpeg);
    RECOVERY_EXPECT_ERROR(extractMetadata(reader, "jpeg", options), ErrorCode::InvalidInput);
}

TEST(MetadataTypesTest, UnknownFormatsGiveNothing) {
    const Bytes jpeg = ::recovery::test::makeJpeg({});
    for (const std::string_view format : {"", "docx"}) {
        const MediaMetadata metadata = extract(jpeg, format);
        EXPECT_EQ(metadata.kind, MediaKind::Unknown);
        EXPECT_EQ(metadata.formatId, format);
        EXPECT_FALSE(metadata.image.has_value());
        EXPECT_TRUE(metadata.previews.empty());
        EXPECT_EQ(metadata.issueCount, 0U);
    }
}

// ---------------------------------------------------------------------------
// JPEG
// ---------------------------------------------------------------------------

TEST(JpegMetadataTest, FrameHeaderGivesSizeAndColour) {
    struct Case {
        ::recovery::test::JpegSampling sampling;
        bool progressive;
        ColorModel color;
        std::uint8_t channels;
    };
    for (const Case& c : {Case{::recovery::test::JpegSampling::Yuv420, false, ColorModel::YCbCr, 3},
                          Case{::recovery::test::JpegSampling::Yuv444, true, ColorModel::YCbCr, 3},
                          Case{::recovery::test::JpegSampling::Gray, false, ColorModel::Grayscale, 1},
                          Case{::recovery::test::JpegSampling::Yuv422, true, ColorModel::YCbCr, 3}}) {
        ::recovery::test::JpegOptions options;
        options.width = 72;
        options.height = 40;
        options.sampling = c.sampling;
        options.progressive = c.progressive;
        const Bytes jpeg = ::recovery::test::makeJpeg(options);
        const MediaMetadata metadata = extract(jpeg, "jpeg");
        EXPECT_EQ(metadata.kind, MediaKind::Image);
        EXPECT_EQ(metadata.mediaType, "image/jpeg");
        ASSERT_TRUE(metadata.image.has_value()) << issuesText(metadata);
        const ImageMetadata& image = *metadata.image;
        EXPECT_EQ(image.width, 72U);
        EXPECT_EQ(image.height, 40U);
        EXPECT_EQ(image.color, c.color);
        EXPECT_EQ(image.channels, c.channels);
        EXPECT_EQ(image.bitsPerChannel, 8);
        EXPECT_EQ(image.bitsPerPixel, 8U * c.channels);
        EXPECT_EQ(image.progressive, c.progressive);
        EXPECT_EQ(image.frames, 1U);
        EXPECT_FALSE(image.alpha);
        EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
        EXPECT_EQ(metadata.previews.size(), 1U);
        expectContentPreview(metadata, jpeg.size(), 72, 40);
        EXPECT_FALSE(metadata.duration.has_value());
    }
}

TEST(JpegMetadataTest, ExifGivesOrientationDateCameraAndThumbnail) {
    for (const bool bigEndian : {false, true}) {
        ::recovery::test::JpegOptions options;
        options.exifThumbnail = true;
        options.exifOrientation = 6;
        options.exifMake = "Canon";
        options.exifModel = "Canon EOS 5D Mark IV";
        options.exifDateTaken = "2024:05:17 14:23:05";
        options.exifOffsetTime = "+02:00";
        options.exifBigEndian = bigEndian;
        const Bytes jpeg = ::recovery::test::makeJpeg(options);
        const MediaMetadata metadata = extract(jpeg, "jpeg");
        ASSERT_TRUE(metadata.image.has_value());
        const ImageMetadata& image = *metadata.image;
        EXPECT_EQ(image.orientation, Orientation::Rotate90);
        EXPECT_EQ(image.cameraMake, "Canon");
        EXPECT_EQ(image.cameraModel, "Canon EOS 5D Mark IV");
        ASSERT_TRUE(image.dateTaken.has_value());
        EXPECT_EQ(image.dateTaken->iso8601(), "2024-05-17T14:23:05+02:00");
        EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);

        // The thumbnail: a 16x16 JPEG inside the segment, first; then the content.
        ASSERT_EQ(metadata.previews.size(), 2U);
        const PreviewSource& thumbnail = metadata.previews[0];
        EXPECT_EQ(thumbnail.kind, PreviewKind::Thumbnail);
        EXPECT_EQ(thumbnail.formatId, "jpeg");
        EXPECT_EQ(thumbnail.mediaType, "image/jpeg");
        EXPECT_EQ(thumbnail.width, 16U);
        EXPECT_EQ(thumbnail.height, 16U);
        EXPECT_EQ(thumbnail.orientation, Orientation::Rotate90);
        ::recovery::test::JpegOptions small;
        small.width = 16;
        small.height = 16;
        small.jfif = false;
        small.seed = options.seed + 1000;
        const Bytes expected = ::recovery::test::makeJpeg(small);
        EXPECT_EQ(thumbnail.offset, test::find(jpeg, expected));
        EXPECT_EQ(thumbnail.length, expected.size());
        carving::MemoryContentReader reader(jpeg);
        Result<std::vector<std::byte>> bytes = readPreview(reader, thumbnail);
        RECOVERY_ASSERT_OK(bytes);
        EXPECT_EQ(*bytes, expected);
        expectContentPreview(metadata, jpeg.size(), 64, 48);
        EXPECT_EQ(metadata.previews.back().orientation, Orientation::Rotate90);
    }
}

TEST(JpegMetadataTest, ExifWithoutDateOffsetIsLocalTime) {
    ::recovery::test::JpegOptions options;
    options.exifDateTaken = "2023:12:31 23:59:59";
    const MediaMetadata metadata = extract(::recovery::test::makeJpeg(options), "jpeg");
    ASSERT_TRUE(metadata.image.has_value());
    ASSERT_TRUE(metadata.image->dateTaken.has_value());
    EXPECT_EQ(metadata.image->dateTaken->zone, MediaDateTime::Zone::Unknown);
    EXPECT_EQ(metadata.image->dateTaken->iso8601(), "2023-12-31T23:59:59");
    EXPECT_FALSE(metadata.image->dateTaken->utc().has_value());
    EXPECT_FALSE(metadata.image->orientation.has_value());
    EXPECT_TRUE(metadata.image->cameraMake.empty());
}

TEST(JpegMetadataTest, ThumbnailWithoutOtherExif) {
    ::recovery::test::JpegOptions options;
    options.exifThumbnail = true;
    const Bytes jpeg = ::recovery::test::makeJpeg(options);
    const MediaMetadata metadata = extract(jpeg, "jpeg");
    ASSERT_NE(previewOf(metadata, PreviewKind::Thumbnail), nullptr) << issuesText(metadata);
    EXPECT_FALSE(previewOf(metadata, PreviewKind::Thumbnail)->orientation.has_value());
    EXPECT_FALSE(metadata.image->orientation.has_value());
    EXPECT_FALSE(metadata.image->dateTaken.has_value());
    EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
}

TEST(JpegMetadataTest, MissingExifGivesNoCameraFields) {
    const Bytes jpeg = ::recovery::test::makeJpeg({});
    const MediaMetadata metadata = extract(jpeg, "jpeg");
    ASSERT_TRUE(metadata.image.has_value());
    EXPECT_FALSE(metadata.image->orientation.has_value());
    EXPECT_FALSE(metadata.image->dateTaken.has_value());
    EXPECT_TRUE(metadata.image->cameraMake.empty());
    EXPECT_TRUE(metadata.image->cameraModel.empty());
    EXPECT_EQ(previewOf(metadata, PreviewKind::Thumbnail), nullptr);
    EXPECT_TRUE(metadata.tags.empty());
}

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------

TEST(PngMetadataTest, HeaderGivesColourTypeDepthAndInterlace) {
    using ::recovery::test::PngColor;
    struct Case {
        PngColor color;
        std::uint8_t depth;
        ColorModel model;
        std::uint8_t channels;
        bool alpha;
    };
    for (const Case& c : {Case{PngColor::Gray, 1, ColorModel::Grayscale, 1, false},
                          Case{PngColor::Gray, 16, ColorModel::Grayscale, 1, false},
                          Case{PngColor::Rgb, 8, ColorModel::Rgb, 3, false},
                          Case{PngColor::Rgb, 16, ColorModel::Rgb, 3, false},
                          // The builder gives palettes a tRNS chunk: transparent colours.
                          Case{PngColor::Palette, 4, ColorModel::Indexed, 1, true},
                          Case{PngColor::GrayAlpha, 8, ColorModel::Grayscale, 2, true},
                          Case{PngColor::Rgba, 16, ColorModel::Rgb, 4, true}}) {
        for (const bool interlaced : {false, true}) {
            ::recovery::test::PngOptions options;
            options.width = 33;
            options.height = 17;
            options.color = c.color;
            options.bitDepth = c.depth;
            options.interlaced = interlaced;
            const Bytes png = ::recovery::test::makePng(options);
            const MediaMetadata metadata = extract(png, "png");
            ASSERT_TRUE(metadata.image.has_value()) << issuesText(metadata);
            const ImageMetadata& image = *metadata.image;
            EXPECT_EQ(image.width, 33U);
            EXPECT_EQ(image.height, 17U);
            EXPECT_EQ(image.color, c.model);
            EXPECT_EQ(image.channels, c.channels);
            EXPECT_EQ(image.bitsPerChannel, c.depth);
            EXPECT_EQ(image.bitsPerPixel, c.depth * c.channels);
            EXPECT_EQ(image.alpha, c.alpha);
            EXPECT_EQ(image.progressive, interlaced);
            EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
            expectContentPreview(metadata, png.size(), 33, 17);
        }
    }
}

TEST(PngMetadataTest, AnimationGivesFramesPlaysAndDuration) {
    ::recovery::test::PngOptions options;
    options.animated = true;
    options.textChunks = true;
    const MediaMetadata metadata = extract(::recovery::test::makePng(options), "png");
    ASSERT_TRUE(metadata.image.has_value()) << issuesText(metadata);
    EXPECT_EQ(metadata.image->frames, 2U);
    EXPECT_EQ(metadata.image->loopCount, 0U);
    // Two frames of 1/10 s.
    EXPECT_EQ(metadata.duration, 200ms);
    EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
}

// ---------------------------------------------------------------------------
// GIF
// ---------------------------------------------------------------------------

TEST(GifMetadataTest, ScreenFramesLoopAndDelays) {
    ::recovery::test::GifOptions options;
    options.width = 40;
    options.height = 30;
    options.colorBits = 5;
    options.frames = 3;
    options.loop = true;
    options.interlaced = true;
    options.comment = "made by the tests";
    const Bytes gif = ::recovery::test::makeGif(options);
    const MediaMetadata metadata = extract(gif, "gif");
    ASSERT_TRUE(metadata.image.has_value()) << issuesText(metadata);
    const ImageMetadata& image = *metadata.image;
    EXPECT_EQ(image.width, 40U);
    EXPECT_EQ(image.height, 30U);
    EXPECT_EQ(image.color, ColorModel::Indexed);
    EXPECT_EQ(image.channels, 1);
    EXPECT_EQ(image.bitsPerPixel, 5U);
    EXPECT_EQ(image.frames, 3U);
    EXPECT_EQ(image.loopCount, 0U);
    EXPECT_TRUE(image.progressive);
    EXPECT_FALSE(image.alpha);
    // Three frames of 10/100 s.
    EXPECT_EQ(metadata.duration, 300ms);
    EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
    expectContentPreview(metadata, gif.size(), 40, 30);
}

TEST(GifMetadataTest, StillImagesHaveNoDuration) {
    for (const bool version89a : {false, true}) {
        ::recovery::test::GifOptions options;
        options.version89a = version89a;
        options.globalColorTable = version89a;
        const MediaMetadata metadata = extract(::recovery::test::makeGif(options), "gif");
        ASSERT_TRUE(metadata.image.has_value()) << issuesText(metadata);
        EXPECT_EQ(metadata.image->frames, 1U);
        EXPECT_FALSE(metadata.image->loopCount.has_value());
        EXPECT_EQ(metadata.image->bitsPerPixel, 4U);
        EXPECT_FALSE(metadata.duration.has_value());
        EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
    }
}

// ---------------------------------------------------------------------------
// BMP
// ---------------------------------------------------------------------------

TEST(BmpMetadataTest, EveryHeaderAndDepth) {
    using ::recovery::test::BmpCompression;
    using ::recovery::test::BmpHeader;
    struct Case {
        BmpHeader header;
        std::uint16_t bits;
        BmpCompression compression;
        ColorModel color;
        std::uint8_t bitsPerChannel;
    };
    for (const Case& c : {Case{BmpHeader::Core, 24, BmpCompression::Rgb, ColorModel::Rgb, 8},
                          Case{BmpHeader::Info, 1, BmpCompression::Rgb, ColorModel::Indexed, 1},
                          Case{BmpHeader::Info, 4, BmpCompression::Rle4, ColorModel::Indexed, 4},
                          Case{BmpHeader::Info, 8, BmpCompression::Rle8, ColorModel::Indexed, 8},
                          Case{BmpHeader::Info, 16, BmpCompression::Rgb, ColorModel::Rgb, 5},
                          Case{BmpHeader::Info, 32, BmpCompression::Rgb, ColorModel::Rgb, 8},
                          Case{BmpHeader::V4, 24, BmpCompression::Rgb, ColorModel::Rgb, 8},
                          Case{BmpHeader::V5, 32, BmpCompression::Rgb, ColorModel::Rgb, 8}}) {
        for (const bool topDown : {false, true}) {
            if (topDown && c.compression != BmpCompression::Rgb) {
                continue;  // RLE bitmaps are bottom-up
            }
            ::recovery::test::BmpOptions options;
            options.header = c.header;
            options.bitsPerPixel = c.bits;
            options.compression = c.compression;
            options.topDown = topDown && c.header != BmpHeader::Core;
            const Bytes bmp = ::recovery::test::makeBmp(options);
            const MediaMetadata metadata = extract(bmp, "bmp");
            ASSERT_TRUE(metadata.image.has_value()) << issuesText(metadata);
            EXPECT_EQ(metadata.image->width, 17U);
            EXPECT_EQ(metadata.image->height, 11U);
            EXPECT_EQ(metadata.image->color, c.color);
            EXPECT_EQ(metadata.image->bitsPerPixel, c.bits);
            EXPECT_EQ(metadata.image->bitsPerChannel, c.bitsPerChannel);
            EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
            expectContentPreview(metadata, bmp.size(), 17, 11);
        }
    }
}

TEST(BmpMetadataTest, BitFieldsGiveChannelDepth) {
    ::recovery::test::BmpOptions options;
    options.header = ::recovery::test::BmpHeader::V5;
    options.compression = ::recovery::test::BmpCompression::Bitfields;
    for (const int depth : {16, 32}) {
        const auto bits = static_cast<std::uint16_t>(depth);
        options.bitsPerPixel = bits;
        const MediaMetadata metadata = extract(::recovery::test::makeBmp(options), "bmp");
        ASSERT_TRUE(metadata.image.has_value()) << issuesText(metadata);
        EXPECT_EQ(metadata.image->color, ColorModel::Rgb);
        EXPECT_EQ(metadata.image->bitsPerPixel, bits);
        EXPECT_GT(metadata.image->channels, 2);
    }
}

// ---------------------------------------------------------------------------
// WebP
// ---------------------------------------------------------------------------

TEST(WebpMetadataTest, SimpleExtendedAndAnimated) {
    using ::recovery::test::WebpKind;
    struct Case {
        WebpKind kind;
        bool extended;
        ColorModel color;
        bool alpha;
    };
    for (const Case& c : {Case{WebpKind::Lossy, false, ColorModel::YCbCr, false},
                          Case{WebpKind::Lossy, true, ColorModel::YCbCr, false},
                          Case{WebpKind::Lossless, false, ColorModel::Rgb, false},
                          Case{WebpKind::Lossless, true, ColorModel::Rgb, false},
                          Case{WebpKind::LossyWithAlpha, true, ColorModel::YCbCr, true}}) {
        ::recovery::test::WebpOptions options;
        options.kind = c.kind;
        options.extended = c.extended;
        options.unknownChunk = true;
        const Bytes webp = ::recovery::test::makeWebp(options);
        const MediaMetadata metadata = extract(webp, "webp");
        ASSERT_TRUE(metadata.image.has_value()) << issuesText(metadata);
        EXPECT_GT(metadata.image->width, 0U);
        EXPECT_GT(metadata.image->height, 0U);
        EXPECT_EQ(metadata.image->color, c.color);
        EXPECT_EQ(metadata.image->alpha, c.alpha);
        EXPECT_EQ(metadata.image->channels, c.alpha ? 4 : 3);
        EXPECT_EQ(metadata.image->frames, 1U);
        EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
        expectContentPreview(metadata, webp.size(), metadata.image->width, metadata.image->height);
    }
    ::recovery::test::WebpOptions animated;
    animated.kind = WebpKind::Animated;
    animated.frames = 3;
    const MediaMetadata metadata = extract(::recovery::test::makeWebp(animated), "webp");
    ASSERT_TRUE(metadata.image.has_value()) << issuesText(metadata);
    EXPECT_EQ(metadata.image->width, 32U);
    EXPECT_EQ(metadata.image->height, 16U);
    EXPECT_EQ(metadata.image->frames, 3U);
    EXPECT_TRUE(metadata.image->loopCount.has_value());
    EXPECT_TRUE(metadata.duration.has_value());
    EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
}

TEST(WebpMetadataTest, ExifChunkThatIsNotTiffIsAnIssue) {
    ::recovery::test::WebpOptions options;
    options.exifSize = 40;
    const MediaMetadata metadata = extract(::recovery::test::makeWebp(options), "webp");
    ASSERT_TRUE(metadata.image.has_value());
    EXPECT_EQ(metadata.issueCount, 1U) << issuesText(metadata);
    EXPECT_FALSE(metadata.image->orientation.has_value());
}

// ---------------------------------------------------------------------------
// MP3
// ---------------------------------------------------------------------------

TEST(Mp3MetadataTest, InfoTagGivesDuration) {
    ::recovery::test::Mp3Options options;
    options.frames = 30;
    const Bytes mp3 = ::recovery::test::makeMp3(options);
    const MediaMetadata metadata = extract(mp3, "mp3");
    EXPECT_EQ(metadata.kind, MediaKind::Audio);
    EXPECT_EQ(metadata.mediaType, "audio/mpeg");
    ASSERT_TRUE(metadata.audio.has_value()) << issuesText(metadata);
    const AudioStreamMetadata& audio = *metadata.audio;
    EXPECT_EQ(audio.codec, "mp3");
    EXPECT_EQ(audio.profile, "MPEG-1 Layer III");
    EXPECT_EQ(audio.sampleRate, 44100U);
    EXPECT_EQ(audio.channels, 2U);
    EXPECT_EQ(audio.bitsPerSample, 0U);
    EXPECT_EQ(audio.bitrate, 128000U);
    EXPECT_FALSE(audio.variableBitrate);
    EXPECT_EQ(metadata.duration, exactly(30 * 1152, 44100));
    EXPECT_FALSE(metadata.durationEstimated);
    ASSERT_TRUE(metadata.bitrate.has_value());
    EXPECT_NEAR(static_cast<double>(*metadata.bitrate), mp3.size() * 8.0 / (30 * 1152 / 44100.0), 2.0);
    EXPECT_TRUE(metadata.tags.empty());
    EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
    expectContentPreview(metadata, mp3.size());
}

TEST(Mp3MetadataTest, FramesAreCountedWithoutAnInfoTag) {
    using ::recovery::test::MpegVersion;
    struct Case {
        MpegVersion version;
        std::uint32_t rate;
        std::uint32_t bitrate;
        ::recovery::test::Mp3Channels channels;
        std::string_view profile;
        std::uint32_t samples;
    };
    for (const Case& c : {Case{MpegVersion::Mpeg1, 48000, 192, ::recovery::test::Mp3Channels::Stereo,
                               "MPEG-1 Layer III", 1152},
                          Case{MpegVersion::Mpeg2, 22050, 32, ::recovery::test::Mp3Channels::Mono,
                               "MPEG-2 Layer III", 576},
                          Case{MpegVersion::Mpeg25, 8000, 0, ::recovery::test::Mp3Channels::Mono,
                               "MPEG-2.5 Layer III", 576}}) {
        ::recovery::test::Mp3Options options;
        options.version = c.version;
        options.sampleRate = c.rate;
        options.bitrate = c.bitrate;
        options.channels = c.channels;
        options.infoTag = ::recovery::test::Mp3InfoTag::None;
        options.frames = 25;
        const MediaMetadata metadata = extract(::recovery::test::makeMp3(options), "mp3");
        ASSERT_TRUE(metadata.audio.has_value()) << issuesText(metadata);
        EXPECT_EQ(metadata.audio->profile, c.profile);
        EXPECT_EQ(metadata.audio->sampleRate, c.rate);
        EXPECT_EQ(metadata.audio->channels, c.channels == ::recovery::test::Mp3Channels::Mono ? 1U : 2U);
        EXPECT_EQ(metadata.duration, exactly(25 * c.samples, c.rate));
        EXPECT_FALSE(metadata.durationEstimated);
        EXPECT_EQ(metadata.audio->variableBitrate, c.bitrate == 0);
        if (c.bitrate != 0) {
            EXPECT_EQ(metadata.audio->bitrate, c.bitrate * 1000U);
        } else {
            EXPECT_GT(metadata.audio->bitrate, 0U);
        }
        EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
    }
}

TEST(Mp3MetadataTest, XingTagMarksVariableBitrate) {
    ::recovery::test::Mp3Options options;
    options.bitrate = 0;
    options.infoTag = ::recovery::test::Mp3InfoTag::Xing;
    const MediaMetadata metadata = extract(::recovery::test::makeMp3(options), "mp3");
    ASSERT_TRUE(metadata.audio.has_value()) << issuesText(metadata);
    EXPECT_TRUE(metadata.audio->variableBitrate);
    EXPECT_EQ(metadata.duration, exactly(20 * 1152, 44100));
    EXPECT_GT(metadata.audio->bitrate, 0U);
}

TEST(Mp3MetadataTest, LongStreamsAreExtrapolatedBeyondTheScanLimit) {
    ::recovery::test::Mp3Options options;
    options.infoTag = ::recovery::test::Mp3InfoTag::None;
    options.frames = 200;
    const Bytes mp3 = ::recovery::test::makeMp3(options);
    MetadataOptions limits;
    limits.maxScanBytes = 8 * 1024;
    const MediaMetadata metadata = extract(mp3, "mp3", limits);
    ASSERT_TRUE(metadata.duration.has_value());
    EXPECT_TRUE(metadata.durationEstimated);
    EXPECT_EQ(metadata.issueCount, 1U) << issuesText(metadata);
    const double exact = 200 * 1152 / 44100.0;
    EXPECT_NEAR(std::chrono::duration<double>(*metadata.duration).count(), exact, exact * 0.02);
}

TEST(Mp3MetadataTest, Id3TagsOfEveryVersion) {
    for (const int tagVersion : {2, 3, 4}) {
        const auto version = static_cast<std::uint8_t>(tagVersion);
        ::recovery::test::Mp3Options options;
        options.id3v2 = version;
        options.id3v1 = true;
        options.apeTag = true;
        options.lyrics3 = true;
        const Bytes mp3 = ::recovery::test::makeMp3(options);
        const MediaMetadata metadata = extract(mp3, "mp3");
        ASSERT_TRUE(metadata.audio.has_value()) << issuesText(metadata);
        // ID3v2 first; ID3v1 fills the rest.
        EXPECT_EQ(metadata.tags.title, "Test title");
        EXPECT_EQ(metadata.tags.artist, "Test artist");
        EXPECT_EQ(metadata.tags.album, "Test album");
        EXPECT_EQ(metadata.tags.genre, "Other");
        EXPECT_EQ(metadata.tags.track, 1U);
        ASSERT_TRUE(metadata.tags.date.has_value());
        EXPECT_EQ(metadata.tags.date->iso8601(), "2026");
        EXPECT_EQ(metadata.duration, exactly(20 * 1152, 44100));
        EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
    }
}

TEST(Mp3MetadataTest, CoverArtIsLocatedNotRead) {
    ::recovery::test::JpegOptions cover;
    cover.width = 24;
    cover.height = 20;
    const Bytes picture = ::recovery::test::makeJpeg(cover);
    for (const int tagVersion : {2, 3, 4}) {
        const auto version = static_cast<std::uint8_t>(tagVersion);
        ::recovery::test::Mp3Options options;
        options.id3v2 = version;
        options.picture = picture;
        const Bytes mp3 = ::recovery::test::makeMp3(options);
        const MediaMetadata metadata = extract(mp3, "mp3");
        ASSERT_EQ(metadata.previews.size(), 2U) << issuesText(metadata);
        const PreviewSource& art = metadata.previews[0];
        EXPECT_EQ(art.kind, PreviewKind::CoverArt);
        EXPECT_EQ(art.formatId, "jpeg");
        EXPECT_EQ(art.mediaType, "image/jpeg");
        EXPECT_EQ(art.width, 24U);
        EXPECT_EQ(art.height, 20U);
        EXPECT_EQ(art.pictureType, 3);
        EXPECT_EQ(art.offset, test::find(mp3, picture));
        EXPECT_EQ(art.length, picture.size());
        carving::MemoryContentReader reader(mp3);
        Result<std::unique_ptr<carving::IContentReader>> opened = openPreview(reader, art);
        RECOVERY_ASSERT_OK(opened);
        EXPECT_EQ((*opened)->size(), picture.size());
        Result<std::span<const std::byte>> head = (*opened)->read(0, 3);
        RECOVERY_ASSERT_OK(head);
        EXPECT_EQ(Bytes(head->begin(), head->end()), Bytes(picture.begin(), picture.begin() + 3));
        RECOVERY_EXPECT_ERROR((*opened)->read(picture.size() - 1, 2), ErrorCode::InvalidInput);
        // A preview larger than the caller's limit is refused.
        RECOVERY_EXPECT_ERROR(readPreview(reader, art, picture.size() - 1), ErrorCode::InvalidInput);
        PreviewSource outside = art;
        outside.offset = mp3.size();
        RECOVERY_EXPECT_ERROR(readPreview(reader, outside), ErrorCode::InvalidInput);
        RECOVERY_EXPECT_ERROR(openPreview(reader, outside), ErrorCode::InvalidInput);
    }
}

TEST(Mp3MetadataTest, TextFramesInEveryEncoding) {
    const Bytes frames = ::recovery::test::makeMp3({});
    struct Case {
        std::uint8_t version;
        std::uint8_t encoding;
        Bytes title;
    };
    // "Gruesse" with a u umlaut, a sharp s and a snowman, in each encoding the version allows.
    const std::u16string_view wide = u"Gr\u00FC\u00DFe \u2603";
    const Bytes utf8 = test::text("Gr\xC3\xBC\xC3\x9F" "e \xE2\x98\x83");
    std::vector<Case> cases = {
        {3, 1, test::utf16(wide, false)},
        {4, 1, test::utf16(wide, false)},
        {4, 2, test::utf16(wide, true)},
        {4, 3, utf8},
        {2, 1, test::utf16(wide, false)},
    };
    for (const Case& c : cases) {
        const std::string title = c.version == 2 ? "TT2" : "TIT2";
        const Bytes tag = test::id3Tag(c.version, {{title, test::id3Text(c.encoding, c.title)}});
        Bytes mp3 = tag;
        mp3.insert(mp3.end(), frames.begin(), frames.end());
        const MediaMetadata metadata = extract(mp3, "mp3");
        EXPECT_EQ(metadata.tags.title, "Gr\xC3\xBC\xC3\x9F" "e \xE2\x98\x83")
            << "version " << int{c.version} << " encoding " << int{c.encoding};
    }
    // ISO 8859-1.
    const Bytes latin = test::id3Tag(3, {{"TIT2", test::id3Text(0, test::text("Gr\xFC\xDF" "e"))}});
    Bytes mp3 = latin;
    mp3.insert(mp3.end(), frames.begin(), frames.end());
    EXPECT_EQ(extract(mp3, "mp3").tags.title, "Gr\xC3\xBC\xC3\x9F" "e");
}

TEST(Mp3MetadataTest, DatesTracksAndGenres) {
    const Bytes frames = ::recovery::test::makeMp3({});
    const auto tagged = [&](std::uint8_t version, const std::vector<test::Id3Frame>& tagFrames) {
        Bytes mp3 = test::id3Tag(version, tagFrames);
        mp3.insert(mp3.end(), frames.begin(), frames.end());
        return extract(mp3, "mp3");
    };
    const auto latin = [](std::string_view value) { return test::id3Text(0, test::text(value)); };

    MediaMetadata v4 = tagged(4, {{"TDRC", latin("2024-05-17T14:23")}, {"TRCK", latin("3/12")},
                                  {"TCON", latin("17")}});
    ASSERT_TRUE(v4.tags.date.has_value());
    EXPECT_EQ(v4.tags.date->iso8601(), "2024-05-17T14:23");
    EXPECT_EQ(v4.tags.track, 3U);
    EXPECT_EQ(v4.tags.trackTotal, 12U);
    EXPECT_EQ(v4.tags.genre, "Rock");

    MediaMetadata v3 = tagged(3, {{"TYER", latin("2019")}, {"TDAT", latin("1705")}, {"TIME", latin("1423")},
                                  {"TRCK", latin("7")}, {"TCON", latin("(13)")}});
    ASSERT_TRUE(v3.tags.date.has_value());
    EXPECT_EQ(v3.tags.date->iso8601(), "2019-05-17T14:23");
    EXPECT_EQ(v3.tags.track, 7U);
    EXPECT_FALSE(v3.tags.trackTotal.has_value());
    EXPECT_EQ(v3.tags.genre, "Pop");

    EXPECT_EQ(tagged(3, {{"TCON", latin("(4)Eurodisco")}}).tags.genre, "Eurodisco");
    EXPECT_EQ(tagged(3, {{"TCON", latin("(RX)")}}).tags.genre, "Remix");
    EXPECT_EQ(tagged(3, {{"TCON", latin("Chiptune")}}).tags.genre, "Chiptune");
    EXPECT_EQ(tagged(2, {{"TYE", latin("1999")}}).tags.date->iso8601(), "1999");

    // A date that is not one is an issue, and no date.
    MediaMetadata bad = tagged(4, {{"TDRC", latin("last summer")}});
    EXPECT_FALSE(bad.tags.date.has_value());
    EXPECT_EQ(bad.issueCount, 1U) << issuesText(bad);
}

TEST(Mp3MetadataTest, UnsynchronisedTagsAreRestoredWithoutCoverArt) {
    const Bytes frames = ::recovery::test::makeMp3({});
    const Bytes picture = ::recovery::test::makeJpeg({});
    Bytes apic = {std::byte{0}};
    const Bytes mime = test::text("image/jpeg");
    apic.insert(apic.end(), mime.begin(), mime.end());
    apic.push_back(std::byte{0});
    apic.push_back(std::byte{3});
    apic.push_back(std::byte{0});
    apic.insert(apic.end(), picture.begin(), picture.end());
    // A title with 0xFF bytes, which unsynchronisation changes.
    const Bytes title = test::id3Text(1, test::utf16(u"\u00FF\u00FF title", false));
    Bytes mp3 = test::id3Tag(3, {{"TIT2", title}, {"APIC", apic}}, 16, true);
    mp3.insert(mp3.end(), frames.begin(), frames.end());
    const MediaMetadata metadata = extract(mp3, "mp3");
    EXPECT_EQ(metadata.tags.title, "\xC3\xBF\xC3\xBF title");
    EXPECT_EQ(previewOf(metadata, PreviewKind::CoverArt), nullptr);
    ASSERT_TRUE(metadata.audio.has_value()) << issuesText(metadata);
    EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
}

TEST(Mp3MetadataTest, LongTextIsCutAtACharacterBoundary) {
    const Bytes frames = ::recovery::test::makeMp3({});
    std::string longTitle;
    for (int i = 0; i < 100; ++i) {
        longTitle += "\xC3\xA9";  // e acute: two bytes
    }
    Bytes mp3 = test::id3Tag(4, {{"TIT2", test::id3Text(3, test::text(longTitle))}});
    mp3.insert(mp3.end(), frames.begin(), frames.end());
    MetadataOptions options;
    options.maxTextLength = 9;
    const MediaMetadata metadata = extract(mp3, "mp3", options);
    EXPECT_EQ(metadata.tags.title, std::string("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"));
}

TEST(Mp3MetadataTest, ControlCharactersBecomeSpaces) {
    const Bytes frames = ::recovery::test::makeMp3({});
    Bytes mp3 = test::id3Tag(4, {{"TIT2", test::id3Text(3, test::text("\n Line one\tand\x07two \r\n"))}});
    mp3.insert(mp3.end(), frames.begin(), frames.end());
    EXPECT_EQ(extract(mp3, "mp3").tags.title, "Line one and two");
}

TEST(Mp3MetadataTest, MissingTagsLeaveTagsEmpty) {
    const MediaMetadata metadata = extract(::recovery::test::makeMp3({}), "mp3");
    EXPECT_TRUE(metadata.tags.empty());
    EXPECT_EQ(metadata.previews.size(), 1U);
}

// ---------------------------------------------------------------------------
// ADTS
// ---------------------------------------------------------------------------

TEST(AdtsMetadataTest, FramesGiveDurationAndProfile) {
    struct Case {
        std::uint32_t rate;
        std::uint8_t channels;
        std::uint8_t profile;
        std::string_view name;
    };
    for (const Case& c : {Case{44100, 2, 1, "AAC LC"}, Case{22050, 1, 0, "AAC Main"},
                          Case{48000, 2, 3, "AAC LTP"}}) {
        ::recovery::test::AacOptions options;
        options.sampleRate = c.rate;
        options.channels = c.channels;
        options.profile = c.profile;
        options.frames = 33;
        const Bytes aac = ::recovery::test::makeAdts(options);
        const MediaMetadata metadata = extract(aac, "aac");
        EXPECT_EQ(metadata.mediaType, "audio/aac");
        ASSERT_TRUE(metadata.audio.has_value()) << issuesText(metadata);
        EXPECT_EQ(metadata.audio->codec, "aac");
        EXPECT_EQ(metadata.audio->profile, c.name);
        EXPECT_EQ(metadata.audio->sampleRate, c.rate);
        EXPECT_EQ(metadata.audio->channels, c.channels);
        EXPECT_EQ(metadata.duration, exactly(33 * 1024, c.rate));
        EXPECT_GT(metadata.audio->bitrate, 0U);
        EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
        expectContentPreview(metadata, aac.size());
    }
}

TEST(AdtsMetadataTest, TagsAroundTheFrames) {
    ::recovery::test::AacOptions options;
    options.id3v2 = 3;
    options.id3v1 = true;
    const MediaMetadata metadata = extract(::recovery::test::makeAdts(options), "aac");
    EXPECT_EQ(metadata.tags.title, "Test title");
    EXPECT_EQ(metadata.tags.album, "Test album");
    EXPECT_EQ(metadata.duration, exactly(20 * 1024, 44100));
    EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
}

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------

TEST(WavMetadataTest, EveryEncoding) {
    using ::recovery::test::WavEncoding;
    struct Case {
        WavEncoding encoding;
        std::uint16_t bits;
        bool extensible;
        std::string_view codec;
    };
    for (const Case& c : {Case{WavEncoding::Pcm, 16, false, "pcm"}, Case{WavEncoding::Pcm, 24, true, "pcm"},
                          Case{WavEncoding::Pcm, 8, false, "pcm"}, Case{WavEncoding::Float, 32, false, "pcm_float"},
                          Case{WavEncoding::Float, 64, true, "pcm_float"}, Case{WavEncoding::ALaw, 8, false, "alaw"},
                          Case{WavEncoding::MuLaw, 8, false, "mulaw"}}) {
        ::recovery::test::WavOptions options;
        options.encoding = c.encoding;
        options.bitsPerSample = c.bits;
        options.extensible = c.extensible;
        options.channels = 2;
        options.sampleRate = 22050;
        options.frames = 2205;
        const Bytes wav = ::recovery::test::makeWav(options);
        const MediaMetadata metadata = extract(wav, "wav");
        EXPECT_EQ(metadata.mediaType, "audio/wav");
        ASSERT_TRUE(metadata.audio.has_value()) << issuesText(metadata);
        EXPECT_EQ(metadata.audio->codec, c.codec);
        EXPECT_EQ(metadata.audio->sampleRate, 22050U);
        EXPECT_EQ(metadata.audio->channels, 2U);
        EXPECT_EQ(metadata.audio->bitsPerSample, c.bits);
        EXPECT_EQ(metadata.audio->bitrate, 22050U * 2 * c.bits);
        EXPECT_EQ(metadata.duration, 100ms);
        EXPECT_FALSE(metadata.durationEstimated);
        EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
        expectContentPreview(metadata, wav.size());
    }
}

TEST(WavMetadataTest, AdpcmDurationComesFromTheFactChunk) {
    ::recovery::test::WavOptions options;
    options.encoding = ::recovery::test::WavEncoding::ImaAdpcm;
    options.channels = 1;
    options.sampleRate = 8000;
    options.frames = 10;
    const Bytes wav = ::recovery::test::makeWav(options);
    const MediaMetadata metadata = extract(wav, "wav");
    ASSERT_TRUE(metadata.audio.has_value()) << issuesText(metadata);
    EXPECT_EQ(metadata.audio->codec, "adpcm_ima");
    std::uint32_t samples = 0;
    for (const ::recovery::test::RiffChunk& chunk : ::recovery::test::riffChunks(wav)) {
        if (chunk.id == "fact") {
            samples = loadLe32(wav, chunk.offset + 8);
        }
    }
    ASSERT_GT(samples, 0U);
    EXPECT_EQ(metadata.duration, exactly(samples, 8000));
    EXPECT_FALSE(metadata.durationEstimated);
}

TEST(WavMetadataTest, InfoListAndId3Chunk) {
    ::recovery::test::WavOptions options;
    options.listInfo = true;
    options.bext = true;
    options.junk = true;
    options.cue = true;
    const MediaMetadata info = extract(::recovery::test::makeWav(options), "wav");
    EXPECT_EQ(info.tags.title, "Test title");
    EXPECT_TRUE(info.tags.artist.empty());
    EXPECT_EQ(info.issueCount, 0U) << issuesText(info);

    // An "id3 " chunk after the data: its frames, and INFO for the rest.
    Bytes wav = ::recovery::test::makeWav(options);
    const Bytes id3 = test::id3Tag(3, {{"TIT2", test::id3Text(0, test::text("ID3 title"))},
                                       {"TPE1", test::id3Text(0, test::text("ID3 artist"))}});
    const Bytes chunk = ::recovery::test::riffChunk("id3 ", id3);
    wav.insert(wav.end(), chunk.begin(), chunk.end());
    storeLe32(wav, 4, static_cast<std::uint32_t>(wav.size() - 8));
    const MediaMetadata both = extract(wav, "wav");
    EXPECT_EQ(both.tags.title, "ID3 title");
    EXPECT_EQ(both.tags.artist, "ID3 artist");
    EXPECT_EQ(both.issueCount, 0U) << issuesText(both);
}

TEST(WavMetadataTest, MissingInfoLeavesTagsEmpty) {
    const MediaMetadata metadata = extract(::recovery::test::makeWav({}), "wav");
    EXPECT_TRUE(metadata.tags.empty());
}

TEST(WavMetadataTest, CutDataCountsTheBytesThere) {
    ::recovery::test::WavOptions options;
    options.channels = 1;
    options.sampleRate = 1000;
    options.frames = 1000;
    Bytes wav = ::recovery::test::makeWav(options);
    wav.resize(wav.size() - 1000);  // half the samples
    const MediaMetadata metadata = extract(wav, "wav");
    EXPECT_EQ(metadata.duration, 500ms);
    EXPECT_EQ(metadata.issueCount, 1U) << issuesText(metadata);
}

// ---------------------------------------------------------------------------
// M4A and MP4
// ---------------------------------------------------------------------------

TEST(M4aMetadataTest, TrackBrandsAndTitle) {
    ::recovery::test::M4aOptions options;
    options.metadata = true;
    options.audio.sampleRate = 32000;
    options.audio.channels = 1;
    options.audio.frames = 40;
    const Bytes m4a = ::recovery::test::makeM4a(options);
    const MediaMetadata metadata = extract(m4a, "m4a");
    EXPECT_EQ(metadata.kind, MediaKind::Audio);
    EXPECT_EQ(metadata.mediaType, "audio/mp4");
    ASSERT_TRUE(metadata.movie.has_value()) << issuesText(metadata);
    EXPECT_EQ(metadata.movie->majorBrand, "M4A ");
    EXPECT_EQ(metadata.movie->compatibleBrands, (std::vector<std::string>{"M4A ", "mp42", "isom"}));
    EXPECT_FALSE(metadata.movie->fragmented);
    EXPECT_FALSE(metadata.movie->created.has_value());
    ASSERT_EQ(metadata.movie->tracks.size(), 1U);
    const TrackMetadata& track = metadata.movie->tracks[0];
    EXPECT_EQ(track.kind, MediaKind::Audio);
    EXPECT_EQ(track.handler, "soun");
    EXPECT_EQ(track.sampleEntry, "mp4a");
    EXPECT_EQ(track.samples, 40U);
    ASSERT_TRUE(metadata.audio.has_value());
    EXPECT_EQ(metadata.audio->codec, "aac");
    EXPECT_EQ(metadata.audio->profile, "AAC LC");
    EXPECT_EQ(metadata.audio->sampleRate, 32000U);
    EXPECT_EQ(metadata.audio->channels, 1U);
    EXPECT_EQ(track.duration, exactly(40 * 1024, 32000));
    EXPECT_TRUE(metadata.duration.has_value());
    EXPECT_EQ(metadata.tags.title, "Title");
    EXPECT_FALSE(metadata.video.has_value());
    EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
    expectContentPreview(metadata, m4a.size());
}

TEST(Mp4MetadataTest, TracksCodecsSizesAndFrameRate) {
    for (const bool version1 : {false, true}) {
        ::recovery::test::Mp4Options options;
        options.movieVersion1 = version1;
        for (::recovery::test::Mp4TrackOptions& track : options.tracks) {
            track.version1 = version1;
        }
        options.tracks.push_back(::recovery::test::Mp4TrackOptions{::recovery::test::Mp4TrackKind::Text});
        const Bytes mp4 = ::recovery::test::makeMp4(options).bytes;
        const MediaMetadata metadata = extract(mp4, "mp4");
        EXPECT_EQ(metadata.kind, MediaKind::Video);
        EXPECT_EQ(metadata.mediaType, "video/mp4");
        ASSERT_TRUE(metadata.movie.has_value()) << issuesText(metadata);
        ASSERT_EQ(metadata.movie->tracks.size(), 3U);
        EXPECT_EQ(metadata.movie->majorBrand, "isom");
        const TrackMetadata& video = metadata.movie->tracks[0];
        EXPECT_EQ(video.id, 1U);
        EXPECT_EQ(video.kind, MediaKind::Video);
        EXPECT_EQ(video.handler, "vide");
        EXPECT_EQ(video.language, "und");
        EXPECT_TRUE(video.enabled);
        EXPECT_EQ(video.samples, 12U);
        EXPECT_EQ(video.duration, exactly(12 * 1001, 30000));
        ASSERT_TRUE(video.video.has_value());
        EXPECT_EQ(video.video->codec, "h264");
        EXPECT_EQ(video.video->profile, "Constrained Baseline");
        EXPECT_EQ(video.video->level, "1.0");
        EXPECT_EQ(video.video->width, 64U);
        EXPECT_EQ(video.video->height, 48U);
        EXPECT_EQ(video.video->displayWidth, 64U);
        EXPECT_EQ(video.video->displayHeight, 48U);
        EXPECT_EQ(video.video->frameRate, (FrameRate{30000, 1001}));
        EXPECT_EQ(video.video->rotation, 0);
        EXPECT_GT(video.video->bitrate, 0U);
        const TrackMetadata& audio = metadata.movie->tracks[1];
        EXPECT_EQ(audio.kind, MediaKind::Audio);
        ASSERT_TRUE(audio.audio.has_value());
        EXPECT_EQ(audio.audio->codec, "aac");
        EXPECT_EQ(audio.audio->sampleRate, 44100U);
        EXPECT_EQ(audio.audio->channels, 2U);
        EXPECT_EQ(audio.duration, exactly(12 * 1024, 44100));
        const TrackMetadata& text = metadata.movie->tracks[2];
        EXPECT_EQ(text.kind, MediaKind::Unknown);
        EXPECT_EQ(text.handler, "text");
        EXPECT_FALSE(text.video.has_value());
        EXPECT_FALSE(text.audio.has_value());
        // The primary streams and the movie's duration.
        ASSERT_TRUE(metadata.video.has_value());
        EXPECT_EQ(metadata.video->codec, "h264");
        ASSERT_TRUE(metadata.audio.has_value());
        EXPECT_EQ(metadata.audio->codec, "aac");
        // The movie header's duration (ms): the text track's 12 samples of 1/2 s.
        EXPECT_EQ(metadata.duration, 6s);
        EXPECT_TRUE(metadata.tags.empty());
        EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
        expectContentPreview(metadata, mp4.size(), 64, 48);
        EXPECT_EQ(metadata.previews.back().orientation, Orientation::Normal);
    }
}

TEST(Mp4MetadataTest, HevcProfileAndLevel) {
    ::recovery::test::Mp4Options options;
    options.tracks = {::recovery::test::Mp4TrackOptions{::recovery::test::Mp4TrackKind::Video}};
    options.tracks[0].hevc = true;
    const MediaMetadata metadata = extract(::recovery::test::makeMp4(options).bytes, "mp4");
    ASSERT_TRUE(metadata.video.has_value()) << issuesText(metadata);
    EXPECT_EQ(metadata.video->codec, "hevc");
    EXPECT_EQ(metadata.video->profile, "Main");
    EXPECT_EQ(metadata.video->level, "3.1");
    EXPECT_FALSE(metadata.audio.has_value());
}

TEST(Mp4MetadataTest, FragmentDurationsAreSummed) {
    for (const ::recovery::test::Mp4FragmentBase base :
         {::recovery::test::Mp4FragmentBase::Moof, ::recovery::test::Mp4FragmentBase::Explicit,
          ::recovery::test::Mp4FragmentBase::Implicit}) {
        for (const std::size_t inMoov : {std::size_t{0}, std::size_t{4}}) {
            ::recovery::test::Mp4Options options;
            options.moov = ::recovery::test::Mp4MoovPlace::First;
            options.fragmentSamples = 3;
            options.samplesInMoov = inMoov;
            options.fragmentBase = base;
            const MediaMetadata metadata = extract(::recovery::test::makeMp4(options).bytes, "mp4");
            ASSERT_TRUE(metadata.movie.has_value()) << issuesText(metadata);
            EXPECT_TRUE(metadata.movie->fragmented);
            ASSERT_EQ(metadata.movie->tracks.size(), 2U);
            EXPECT_EQ(metadata.movie->tracks[0].samples, 12U);
            EXPECT_EQ(metadata.movie->tracks[0].duration, exactly(12 * 1001, 30000));
            EXPECT_EQ(metadata.movie->tracks[1].duration, exactly(12 * 1024, 44100));
            ASSERT_TRUE(metadata.video.has_value());
            EXPECT_EQ(metadata.video->frameRate, (FrameRate{30000, 1001}));
            // The longest track's (the movie header's is 0 in fragmented files).
            EXPECT_EQ(metadata.duration, metadata.movie->tracks[0].duration);
            EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
        }
    }
}

// The track header's matrix, patched: rotations and other transforms.
Bytes withMatrix(const Bytes& mp4, std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d) {
    Bytes out = mp4;
    const ::recovery::test::BoxPosition tkhd =
        ::recovery::test::findBox(::recovery::test::mp4Boxes(out), "moov/trak/tkhd");
    const std::size_t matrix = tkhd.offset + 8 + 40;
    storeBe32(out, matrix, static_cast<std::uint32_t>(a));
    storeBe32(out, matrix + 4, static_cast<std::uint32_t>(b));
    storeBe32(out, matrix + 12, static_cast<std::uint32_t>(c));
    storeBe32(out, matrix + 16, static_cast<std::uint32_t>(d));
    return out;
}

TEST(Mp4MetadataTest, MatrixGivesRotation) {
    const Bytes mp4 = ::recovery::test::makeMp4({}).bytes;
    constexpr std::int32_t one = 0x10000;
    struct Case {
        std::int32_t a, b, c, d;
        std::uint16_t rotation;
        Orientation orientation;
    };
    for (const Case& t : {Case{0, one, -one, 0, 90, Orientation::Rotate90},
                          Case{-one, 0, 0, -one, 180, Orientation::Rotate180},
                          Case{0, -one, one, 0, 270, Orientation::Rotate270}}) {
        const MediaMetadata metadata = extract(withMatrix(mp4, t.a, t.b, t.c, t.d), "mp4");
        ASSERT_TRUE(metadata.video.has_value()) << issuesText(metadata);
        EXPECT_EQ(metadata.video->rotation, t.rotation);
        EXPECT_EQ(metadata.movie->tracks[0].video->rotation, t.rotation);
        EXPECT_EQ(metadata.previews.back().orientation, t.orientation);
        EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
    }
    // A mirror is no rotation: an issue, and rotation 0.
    const MediaMetadata mirror = extract(withMatrix(mp4, -one, 0, 0, one), "mp4");
    ASSERT_TRUE(mirror.video.has_value());
    EXPECT_EQ(mirror.video->rotation, 0);
    EXPECT_EQ(mirror.issueCount, 1U) << issuesText(mirror);
}

TEST(Mp4MetadataTest, HeaderTimesAreUtc) {
    Bytes mp4 = ::recovery::test::makeMp4({}).bytes;
    const ::recovery::test::BoxPosition mvhd =
        ::recovery::test::findBox(::recovery::test::mp4Boxes(mp4), "moov/mvhd");
    // 2024-05-17T14:23:05Z and 2024-05-18T00:00:00Z, in seconds since 1904.
    storeBe32(mp4, mvhd.offset + 12, static_cast<std::uint32_t>(1715955785 + 2082844800ULL));
    storeBe32(mp4, mvhd.offset + 16, static_cast<std::uint32_t>(1715990400 + 2082844800ULL));
    const MediaMetadata metadata = extract(mp4, "mp4");
    ASSERT_TRUE(metadata.movie.has_value());
    ASSERT_TRUE(metadata.movie->created.has_value());
    EXPECT_EQ(metadata.movie->created->iso8601(), "2024-05-17T14:23:05Z");
    ASSERT_TRUE(metadata.movie->modified.has_value());
    EXPECT_EQ(metadata.movie->modified->iso8601(), "2024-05-18T00:00:00Z");
}

TEST(Mp4MetadataTest, ItunesItemsAndCoverArt) {
    const Bytes cover = ::recovery::test::makePng({});
    const Bytes trkn = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{4}, std::byte{0}, std::byte{9},
                        std::byte{0}, std::byte{0}};
    const Bytes gnre = {std::byte{0}, std::byte{18}};  // ID3v1 genre 17 + 1: Rock
    const Bytes udta = test::userData({
        test::ilstItem("\xA9" "nam", 1, test::text("A title")),
        test::ilstItem("\xA9" "ART", 1, test::text("An artist")),
        test::ilstItem("\xA9" "alb", 1, test::text("An album")),
        test::ilstItem("\xA9" "day", 1, test::text("2021-07-04T12:00:00Z")),
        test::ilstItem("trkn", 0, trkn),
        test::ilstItem("gnre", 0, gnre),
        test::ilstItem("covr", 14, cover),
    });
    const Bytes mp4 = test::withMoovChild(::recovery::test::makeMp4({}).bytes, udta);
    const MediaMetadata metadata = extract(mp4, "mp4");
    EXPECT_EQ(metadata.tags.title, "A title");
    EXPECT_EQ(metadata.tags.artist, "An artist");
    EXPECT_EQ(metadata.tags.album, "An album");
    ASSERT_TRUE(metadata.tags.date.has_value());
    EXPECT_EQ(metadata.tags.date->iso8601(), "2021-07-04T12:00:00Z");
    EXPECT_EQ(metadata.tags.track, 4U);
    EXPECT_EQ(metadata.tags.trackTotal, 9U);
    EXPECT_EQ(metadata.tags.genre, "Rock");
    EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
    ASSERT_EQ(metadata.previews.size(), 2U);
    const PreviewSource& art = metadata.previews[0];
    EXPECT_EQ(art.kind, PreviewKind::CoverArt);
    EXPECT_EQ(art.formatId, "png");
    EXPECT_EQ(art.width, 32U);
    EXPECT_EQ(art.height, 24U);
    EXPECT_EQ(art.offset, test::find(mp4, cover));
    EXPECT_EQ(art.length, cover.size());
    EXPECT_FALSE(art.pictureType.has_value());
}

TEST(Mp4MetadataTest, QuickTimeTextAtoms) {
    const auto atom = [](std::string_view type, std::string_view value) {
        Bytes payload;
        payload.push_back(static_cast<std::byte>(value.size() >> 8));
        payload.push_back(static_cast<std::byte>(value.size() & 0xFF));
        payload.push_back(std::byte{0x55});  // "und"
        payload.push_back(std::byte{0xC4});
        const Bytes text = test::text(value);
        payload.insert(payload.end(), text.begin(), text.end());
        return test::box(type, payload);
    };
    Bytes udta;
    for (const Bytes& item : {atom("\xA9" "nam", "QuickTime title"), atom("\xA9" "aut", "An author"),
                              atom("\xA9" "day", "2020")}) {
        udta.insert(udta.end(), item.begin(), item.end());
    }
    ::recovery::test::Mp4Options options;
    options.majorBrand = "qt  ";
    options.compatibleBrands = {"qt  "};
    const Bytes mov = test::withMoovChild(::recovery::test::makeMp4(options).bytes, test::box("udta", udta));
    const MediaMetadata metadata = extract(mov, "mp4");
    EXPECT_EQ(metadata.mediaType, "video/quicktime");
    EXPECT_EQ(metadata.tags.title, "QuickTime title");
    EXPECT_EQ(metadata.tags.artist, "An author");
    EXPECT_EQ(metadata.tags.date->iso8601(), "2020");
    EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
}

TEST(Mp4MetadataTest, BrandsDecideTheMediaType) {
    ::recovery::test::Mp4Options options;
    options.majorBrand = "3gp6";
    options.compatibleBrands = {"3gp6", "isom"};
    EXPECT_EQ(extract(::recovery::test::makeMp4(options).bytes, "mp4").mediaType, "video/3gpp");
    // A video brand makes it video even without a video track.
    options.majorBrand = "M4V ";
    options.tracks = {::recovery::test::Mp4TrackOptions{::recovery::test::Mp4TrackKind::Audio}};
    const MediaMetadata m4v = extract(::recovery::test::makeMp4(options).bytes, "mp4");
    EXPECT_EQ(m4v.kind, MediaKind::Video);
    EXPECT_FALSE(m4v.video.has_value());
    // No video track: no content preview of a video.
    EXPECT_TRUE(m4v.previews.empty());
    // An image brand is not media.
    options.majorBrand = "heic";
    const MediaMetadata heic = extract(::recovery::test::makeMp4(options).bytes, "mp4");
    EXPECT_EQ(heic.kind, MediaKind::Unknown);
    EXPECT_TRUE(heic.mediaType.empty());
    EXPECT_TRUE(heic.previews.empty());
}

}  // namespace
}  // namespace recovery::metadata
