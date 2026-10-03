// Cross-checks against audio files this project did not write.
//
// AudioSamplesTest runs always: every embedded sample (LAME, FFmpeg, faac,
// fdkaac, SoX, Python, the Windows speech synthesizer and Media Foundation)
// must be recognised by its format and no other, validated, and carved back
// byte for byte.
//
// AudioReferenceTest reads a directory of audio files made elsewhere, for a
// corpus larger than the embedded samples (music, recordings from phones and
// voice recorders). It is skipped unless RECOVERY_AUDIO_REFERENCE_DIR names a
// directory of .mp3/.wav/.m4a/.aac files; see docs/testing/testing.md.
//
// AudioBuilderExport writes the test builders' own files to
// RECOVERY_AUDIO_EXPORT_DIR (skipped otherwise), so that independent decoders
// can check them: tests/reference/check_audio_builder_files.sh.

#include "formats/audio_formats.hpp"

#include "format_test_helpers.hpp"
#include "formats/image_formats.hpp"
#include "support/audio_builders.hpp"
#include "support/audio_samples.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace recovery::formats {
namespace {

using carving::FileCandidate;
using carving::IFileFormat;
using carving::ValidationStatus;
using testing::Bytes;
using testing::carvedBytes;
using testing::Carved;

const IFileFormat& formatWithId(std::string_view id) {
    static const std::vector<std::shared_ptr<const IFileFormat>> formats = audioFormats();
    for (const auto& format : formats) {
        if (format->descriptor().id == id) {
            return *format;
        }
    }
    std::abort();
}

std::string formatOfExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".mp3" || extension == ".wav" || extension == ".m4a" || extension == ".aac") {
        return extension.substr(1);
    }
    return {};
}

// No format but `id` makes a file of these bytes: its header is refused, or
// its end detection finds nothing (MP3 and AAC share the ID3v2 signature).
void expectNoOtherFormatClaims(const Bytes& bytes, std::string_view id) {
    std::vector<std::shared_ptr<const IFileFormat>> formats = audioFormats();
    for (const auto& image : imageFormats()) {
        formats.push_back(image);
    }
    for (const auto& other : formats) {
        if (other->descriptor().id == id || !testing::headerOf(*other, bytes).plausible) {
            continue;
        }
        const bool signed_ = std::any_of(other->descriptor().signatures.begin(), other->descriptor().signatures.end(),
                                         [&](const carving::FileSignature& s) {
                                             return bytes.size() >= s.reach() && s.matches(std::span(bytes).subspan(s.offset));
                                         });
        if (signed_) {
            EXPECT_EQ(testing::endOf(*other, bytes).length, 0u) << other->descriptor().id;
        }
    }
}

// Every file must be recognised where it lies in a noisy image, carved with
// exactly its bytes, and validated, with every format registered.
void expectCarvedExactly(const std::vector<std::pair<std::string, Bytes>>& files) {
    std::uint64_t size = 1 * kMiB;
    for (const auto& [name, bytes] : files) {
        size += bytes.size() + 4096;
    }
    test::VirtualSource source(size);
    source.setNoise(0xA0D10);
    std::vector<std::uint64_t> offsets;
    std::uint64_t offset = 4096;
    for (const auto& [name, bytes] : files) {
        offsets.push_back(offset);
        source.plant(offset, bytes);
        offset += bytes.size() + 4096 - (bytes.size() % 4096);
    }
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = testing::carve(source, testing::Formats::All);
    for (std::size_t i = 0; i < files.size(); ++i) {
        SCOPED_TRACE(files[i].first);
        const auto found = std::find_if(carved.candidates.begin(), carved.candidates.end(),
                                        [&](const FileCandidate& c) { return c.sourceOffset == offsets[i]; });
        ASSERT_NE(found, carved.candidates.end());
        EXPECT_EQ(found->formatId, formatOfExtension(files[i].first));
        EXPECT_EQ(found->validation.status, ValidationStatus::Valid) << testing::describe(*found);
        EXPECT_EQ(found->length, files[i].second.size()) << testing::describe(*found);
        EXPECT_TRUE(carvedBytes(source, *found) == files[i].second);
    }
}

TEST(AudioSamplesTest, EverySampleIsRecognisedValidatedAndCarvedBack) {
    ASSERT_GE(test::audio_samples::all().size(), 30u);
    std::vector<std::pair<std::string, Bytes>> files;
    for (const test::audio_samples::Sample& sample : test::audio_samples::all()) {
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes bytes = sample.data();
        EXPECT_EQ(formatOfExtension(std::string(sample.name)), sample.format);
        EXPECT_TRUE(testing::isIntact(formatWithId(sample.format), bytes));
        expectNoOtherFormatClaims(bytes, sample.format);
        files.emplace_back(sample.name, bytes);
    }
    expectCarvedExactly(files);
}

TEST(AudioReferenceTest, ExternalFilesAreValidAndCarvedExactly) {
    const std::optional<std::filesystem::path> directory =
        testing::directoryFromEnvironment(L"RECOVERY_AUDIO_REFERENCE_DIR");
    if (!directory) {
        GTEST_SKIP() << "set RECOVERY_AUDIO_REFERENCE_DIR to run against audio files made elsewhere";
    }
    std::vector<std::pair<std::string, Bytes>> files;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(*directory, ec)) {
        const std::string id = formatOfExtension(item.path());
        if (id.empty() || !item.is_regular_file()) {
            continue;
        }
        const Bytes bytes = test::readFile(item.path());
        if (bytes.size() > 64 * kMiB) {
            continue;  // the corpus is meant for ordinary songs and recordings
        }
        SCOPED_TRACE(item.path().filename().string());
        EXPECT_TRUE(testing::isIntact(formatWithId(id), bytes));
        files.emplace_back(item.path().filename().string(), bytes);
    }
    ASSERT_FALSE(files.empty()) << "no audio files in " << directory->string();
    expectCarvedExactly(files);
    std::cout << "checked " << files.size() << " reference audio files\n";
}

TEST(AudioBuilderExport, WritesFiles) {
    const std::optional<std::filesystem::path> directory =
        testing::directoryFromEnvironment(L"RECOVERY_AUDIO_EXPORT_DIR", false);
    if (!directory) {
        GTEST_SKIP() << "set RECOVERY_AUDIO_EXPORT_DIR to export the builders' files for other decoders";
    }
    std::vector<std::pair<std::string, Bytes>> files;

    struct Mp3Shape {
        std::string name;
        test::MpegVersion version;
        std::uint32_t rate;
        std::uint32_t bitrate;
        test::Mp3Channels channels;
    };
    for (const Mp3Shape& shape : std::vector<Mp3Shape>{
             {"mpeg1_joint", test::MpegVersion::Mpeg1, 44100, 128, test::Mp3Channels::JointStereo},
             {"mpeg1_vbr", test::MpegVersion::Mpeg1, 48000, 0, test::Mp3Channels::Stereo},
             {"mpeg1_mono", test::MpegVersion::Mpeg1, 32000, 64, test::Mp3Channels::Mono},
             {"mpeg2_dual", test::MpegVersion::Mpeg2, 22050, 64, test::Mp3Channels::DualChannel},
             {"mpeg25_mono", test::MpegVersion::Mpeg25, 8000, 16, test::Mp3Channels::Mono}}) {
        test::Mp3Options options;
        options.version = shape.version;
        options.sampleRate = shape.rate;
        options.bitrate = shape.bitrate;
        options.channels = shape.channels;
        options.frames = 40;
        files.emplace_back("builder_" + shape.name + ".mp3", test::makeMp3(options));
    }
    test::Mp3Options mp3;
    mp3.frames = 40;
    mp3.crc = true;
    mp3.reservoir = false;
    files.emplace_back("builder_crc.mp3", test::makeMp3(mp3));
    mp3.crc = false;
    mp3.reservoir = true;
    mp3.infoTag = test::Mp3InfoTag::None;
    mp3.id3v2 = 4;
    mp3.apeTag = true;
    mp3.lyrics3 = true;
    mp3.id3v1 = true;
    files.emplace_back("builder_tags.mp3", test::makeMp3(mp3));

    test::AacOptions aac;
    aac.frames = 40;
    files.emplace_back("builder_stereo.aac", test::makeAdts(aac));
    aac.channels = 1;
    aac.sampleRate = 22050;
    aac.id3v2 = 3;
    files.emplace_back("builder_mono_id3.aac", test::makeAdts(aac));

    test::M4aOptions m4a;
    m4a.audio.frames = 40;
    files.emplace_back("builder_moov_last.m4a", test::makeM4a(m4a));
    m4a.moovFirst = true;
    m4a.metadata = true;
    files.emplace_back("builder_faststart.m4a", test::makeM4a(m4a));
    m4a.moovFirst = false;
    m4a.co64 = true;
    m4a.largeMdat = true;
    m4a.majorBrand = "mp42";
    m4a.compatibleBrands = {"mp42", "isom"};
    m4a.audio.channels = 1;
    m4a.audio.sampleRate = 22050;
    files.emplace_back("builder_co64.m4a", test::makeM4a(m4a));

    struct WavShape {
        std::string name;
        test::WavEncoding encoding;
        std::uint16_t bits;
        std::uint16_t channels;
        bool extensible;
    };
    for (const WavShape& shape : std::vector<WavShape>{{"pcm8", test::WavEncoding::Pcm, 8, 1, false},
                                                       {"pcm16", test::WavEncoding::Pcm, 16, 2, false},
                                                       {"pcm24", test::WavEncoding::Pcm, 24, 2, true},
                                                       {"pcm32_6ch", test::WavEncoding::Pcm, 32, 6, true},
                                                       {"float32", test::WavEncoding::Float, 32, 2, false},
                                                       {"float64", test::WavEncoding::Float, 64, 1, true},
                                                       {"alaw", test::WavEncoding::ALaw, 8, 1, false},
                                                       {"mulaw", test::WavEncoding::MuLaw, 8, 2, false},
                                                       {"ima_adpcm", test::WavEncoding::ImaAdpcm, 4, 2, false}}) {
        test::WavOptions options;
        options.encoding = shape.encoding;
        options.bitsPerSample = shape.bits;
        options.channels = shape.channels;
        options.extensible = shape.extensible;
        options.sampleRate = 22050;
        options.frames = shape.encoding == test::WavEncoding::ImaAdpcm ? 8 : 2205;
        options.listInfo = true;
        files.emplace_back("builder_" + shape.name + ".wav", test::makeWav(options));
    }
    test::WavOptions broadcast;
    broadcast.junk = true;
    broadcast.bext = true;
    broadcast.fact = true;
    broadcast.cue = true;
    files.emplace_back("builder_bwf.wav", test::makeWav(broadcast));

    std::filesystem::create_directories(*directory);
    for (const auto& [name, bytes] : files) {
        test::writeFile(*directory / name, bytes);
    }
    // What is written must also pass the engine's own checks.
    for (const auto& [name, bytes] : files) {
        SCOPED_TRACE(name);
        const std::string id = formatOfExtension(name);
        ASSERT_FALSE(id.empty());
        EXPECT_TRUE(testing::isIntact(formatWithId(id), bytes));
    }
    std::cout << "wrote " << files.size() << " files to " << directory->string() << "\n";
}

}  // namespace
}  // namespace recovery::formats
