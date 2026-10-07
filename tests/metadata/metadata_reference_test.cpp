// Media metadata (P17) of files written by independent tools (FFmpeg,
// ExifTool, mutagen, libjpeg-turbo, libwebp, giflib, GDI+, LAME, faac,
// fdkaac, SoX, GPAC, Media Foundation), checked against what exiftool and
// ffprobe read in them (tests/reference/embed_metadata_samples.py). It
// guards against the builders and the extractors sharing a misreading of a
// specification.

#include "metadata/media_metadata.hpp"

#include "metadata_samples.hpp"
#include "metadata_test_helpers.hpp"
#include "support/audio_samples.hpp"
#include "support/image_samples.hpp"
#include "support/mp4_samples.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace recovery::metadata {
namespace {

using test::Bytes;
using ::recovery::test::metadata_samples::Sample;

Bytes bytesOf(const Sample& sample) {
    if (!sample.bytes.empty()) {
        Bytes out(sample.bytes.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<std::byte>(sample.bytes[i]);
        }
        return out;
    }
    for (const auto& image : ::recovery::test::samples::all()) {
        if (image.name == sample.name) {
            return image.data();
        }
    }
    for (const auto& audio : ::recovery::test::audio_samples::all()) {
        if (audio.name == sample.name) {
            return audio.data();
        }
    }
    return ::recovery::test::mp4_samples::named(sample.name).data();
}

std::string number(double value) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%.6f", value);
    return text.data();
}

// The engine's metadata under the expectations' keys.
std::map<std::string, std::string> actual(const MediaMetadata& metadata) {
    std::map<std::string, std::string> out;
    if (metadata.image.has_value()) {
        const ImageMetadata& image = *metadata.image;
        out["image.width"] = std::to_string(image.width);
        out["image.height"] = std::to_string(image.height);
        out["image.frames"] = std::to_string(image.frames);
        if (image.orientation.has_value()) {
            out["image.orientation"] = std::to_string(static_cast<int>(*image.orientation));
        }
        if (!image.cameraMake.empty()) {
            out["image.make"] = image.cameraMake;
        }
        if (!image.cameraModel.empty()) {
            out["image.model"] = image.cameraModel;
        }
        if (image.dateTaken.has_value()) {
            out["image.dateTaken"] = image.dateTaken->iso8601();
        }
        if (image.loopCount.has_value()) {
            out["image.loopCount"] = std::to_string(*image.loopCount);
        }
    }
    for (const PreviewSource& preview : metadata.previews) {
        if (preview.kind == PreviewKind::Thumbnail && !out.contains("thumbnail.offset")) {
            out["thumbnail.offset"] = std::to_string(preview.offset);
            out["thumbnail.length"] = std::to_string(preview.length);
        }
        if (preview.kind == PreviewKind::CoverArt && !out.contains("cover.width")) {
            out["cover.width"] = std::to_string(preview.width);
            out["cover.height"] = std::to_string(preview.height);
        }
    }
    if (metadata.audio.has_value()) {
        out["audio.codec"] = metadata.audio->codec;
        out["audio.profile"] = metadata.audio->profile;
        out["audio.sampleRate"] = std::to_string(metadata.audio->sampleRate);
        out["audio.channels"] = std::to_string(metadata.audio->channels);
    }
    if (metadata.video.has_value()) {
        const VideoStreamMetadata& video = *metadata.video;
        out["video.codec"] = video.codec;
        out["video.profile"] = video.profile;
        out["video.level"] = video.level;
        out["video.width"] = std::to_string(video.width);
        out["video.height"] = std::to_string(video.height);
        out["video.rotation"] = std::to_string(video.rotation);
        if (video.frameRate.has_value()) {
            out["video.frameRate"] = number(video.frameRate->value());
        }
    }
    if (metadata.movie.has_value()) {
        out["movie.majorBrand"] = metadata.movie->majorBrand;
        if (metadata.movie->created.has_value()) {
            out["movie.created"] = metadata.movie->created->iso8601();
        }
        for (std::size_t i = 0; i < metadata.movie->tracks.size(); ++i) {
            const TrackMetadata& track = metadata.movie->tracks[i];
            const std::string prefix = "track." + std::to_string(i + 1) + ".";
            out[prefix + "kind"] = std::string(toString(track.kind));
            if (!track.language.empty()) {
                out[prefix + "language"] = track.language;
            }
        }
    }
    const MediaTags& tags = metadata.tags;
    const auto text = [&](const char* key, const std::string& value) {
        if (!value.empty()) {
            out[key] = value;
        }
    };
    text("tags.title", tags.title);
    text("tags.artist", tags.artist);
    text("tags.album", tags.album);
    text("tags.genre", tags.genre);
    if (tags.date.has_value()) {
        out["tags.date"] = tags.date->iso8601();
    }
    if (tags.track.has_value()) {
        out["tags.track"] = std::to_string(*tags.track);
    }
    if (tags.trackTotal.has_value()) {
        out["tags.trackTotal"] = std::to_string(*tags.trackTotal);
    }
    if (metadata.duration.has_value()) {
        out["duration"] = number(std::chrono::duration<double>(*metadata.duration).count());
    }
    return out;
}

double fraction(const std::string& text) {
    const std::size_t slash = text.find('/');
    if (slash == std::string::npos) {
        return std::stod(text);
    }
    return std::stod(text.substr(0, slash)) / std::stod(text.substr(slash + 1));
}

TEST(MetadataReferenceTest, IndependentWritersAgreeWithExiftoolAndFfprobe) {
    const auto& samples = ::recovery::test::metadata_samples::all();
    ASSERT_GE(samples.size(), 100U);
    std::size_t checked = 0;
    for (const Sample& sample : samples) {
        const Bytes bytes = bytesOf(sample);
        const MediaMetadata metadata = test::extract(bytes, sample.format);
        const std::map<std::string, std::string> ours = actual(metadata);
        for (const auto& [key, value] : sample.expected) {
            const std::string name(key);
            const std::string expected(value);
            const auto found = ours.find(name);
            if (found == ours.end()) {
                ADD_FAILURE() << sample.name << ": no " << name << " (expected " << expected << ")\n"
                              << test::issuesText(metadata);
                continue;
            }
            ++checked;
            if (name == "duration") {
                // ffprobe counts what decoders play (encoder delay, edit
                // lists); the engine what the headers and frames say.
                const double theirs = std::stod(expected);
                const double mine = std::stod(found->second);
                EXPECT_NEAR(mine, theirs, std::max(0.07, theirs * 0.03)) << sample.name;
            } else if (name == "video.frameRate") {
                EXPECT_NEAR(std::stod(found->second), fraction(expected), 1e-4) << sample.name;
            } else {
                EXPECT_EQ(found->second, expected) << sample.name << ": " << name;
            }
        }
    }
    EXPECT_GT(checked, 500U);
}

TEST(MetadataReferenceTest, PreviewsOfIndependentFilesAreTheirPictures) {
    for (const Sample& sample : ::recovery::test::metadata_samples::all()) {
        const Bytes bytes = bytesOf(sample);
        const MediaMetadata metadata = test::extract(bytes, sample.format);
        for (const PreviewSource& preview : metadata.previews) {
            ASSERT_LE(preview.offset + preview.length, bytes.size()) << sample.name;
            if (preview.kind == PreviewKind::Content) {
                EXPECT_EQ(preview.length, bytes.size()) << sample.name;
                continue;
            }
            // A thumbnail or cover read on its own gives the same picture.
            const std::span<const std::byte> picture(bytes.data() + preview.offset, preview.length);
            const MediaMetadata own = test::extract(picture, preview.formatId);
            ASSERT_TRUE(own.image.has_value()) << sample.name;
            EXPECT_EQ(own.image->width, preview.width) << sample.name;
            EXPECT_EQ(own.image->height, preview.height) << sample.name;
        }
    }
}

}  // namespace
}  // namespace recovery::metadata
