// WAV: intact files from the builder (PCM of every depth, IEEE float, A-law,
// mu-law, IMA ADPCM, WAVE_FORMAT_EXTENSIBLE, JUNK, bext, fact, LIST and cue
// chunks, odd sizes) and from SoX, FFmpeg, Python and the Windows speech
// synthesizer; truncation at every position; header rejection; size fields
// that disagree with the chunks; fmt fields that disagree with each other;
// false signatures; fragments; fuzzing.

#include "formats/wav_format.hpp"

#include "format_test_helpers.hpp"
#include "support/audio_builders.hpp"
#include "support/audio_samples.hpp"
#include "support/image_builders.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <string>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::ValidationStatus;
using test::WavEncoding;
using test::WavOptions;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::isIntact;
using testing::isInvalid;
using testing::overwritten;
using testing::verdictOf;

const WavFormat& format() {
    static const WavFormat instance;
    return instance;
}

Bytes le16(std::uint32_t value) {
    return Bytes{static_cast<std::byte>(value & 0xFF), static_cast<std::byte>((value >> 8) & 0xFF)};
}

Bytes le32(std::uint32_t value) {
    Bytes out;
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
    }
    return out;
}

const test::RiffChunk& chunk(const std::vector<test::RiffChunk>& chunks, std::string_view id) {
    for (const test::RiffChunk& candidate : chunks) {
        if (candidate.id == id) {
            return candidate;
        }
    }
    throw std::invalid_argument("no such chunk");
}

TEST(WavFormatTest, DescriptorIsValid) {
    RECOVERY_EXPECT_OK(carving::validateDescriptor(format().descriptor()));
    EXPECT_EQ(format().descriptor().id, "wav");
    EXPECT_EQ(format().descriptor().signatures.size(), 1u);
    EXPECT_EQ(format().descriptor().endDetection, carving::EndDetectionMethod::SizeField);
    EXPECT_FALSE(format().descriptor().selfSynchronizing);
}

TEST(WavFormatTest, BuilderFilesOfEveryShapeAreIntact) {
    struct Shape {
        WavEncoding encoding;
        std::uint16_t bits;
    };
    for (const Shape shape : {Shape{WavEncoding::Pcm, 8}, Shape{WavEncoding::Pcm, 16}, Shape{WavEncoding::Pcm, 24},
                              Shape{WavEncoding::Pcm, 32}, Shape{WavEncoding::Float, 32}, Shape{WavEncoding::Float, 64},
                              Shape{WavEncoding::ALaw, 8}, Shape{WavEncoding::MuLaw, 8},
                              Shape{WavEncoding::ImaAdpcm, 4}}) {
        for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}, std::uint16_t{6}}) {
            for (const std::uint32_t rate : {8000U, 44100U, 96000U}) {
                WavOptions options;
                options.encoding = shape.encoding;
                options.bitsPerSample = shape.bits;
                options.channels = channels;
                options.sampleRate = rate;
                options.frames = shape.encoding == WavEncoding::ImaAdpcm ? 3 : 333;  // odd: a pad byte for 8-bit mono
                options.extensible = (shape.encoding == WavEncoding::Pcm || shape.encoding == WavEncoding::Float) &&
                                     (channels > 2 || shape.bits > 16);
                SCOPED_TRACE(std::to_string(static_cast<int>(shape.encoding)) + " " + std::to_string(shape.bits) +
                             "-bit " + std::to_string(channels) + " channels " + std::to_string(rate) + " Hz");
                EXPECT_TRUE(isIntact(format(), test::makeWav(options)));
            }
        }
    }
    WavOptions everything;
    everything.junk = true;
    everything.bext = true;
    everything.fact = true;
    everything.listInfo = true;
    everything.cue = true;
    EXPECT_TRUE(isIntact(format(), test::makeWav(everything)));
    WavOptions empty;
    empty.frames = 0;
    const Bytes smallest = test::makeWav(empty);
    EXPECT_TRUE(isIntact(format(), smallest));
    EXPECT_EQ(smallest.size(), WavFormat::kMinimumSize);
}

TEST(WavFormatTest, FilesFromRealEncodersAreIntact) {
    std::size_t checked = 0;
    for (const test::audio_samples::Sample& sample : test::audio_samples::all()) {
        if (sample.format != "wav") {
            continue;
        }
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes file = sample.data();
        EXPECT_TRUE(isIntact(format(), file));
        EXPECT_TRUE(testing::prefixesAreTruncated(format(), file));
        ++checked;
    }
    EXPECT_GE(checked, 14u);
}

TEST(WavFormatTest, EveryPrefixIsTruncated) {
    WavOptions options;
    options.frames = 1500;
    options.bext = true;
    options.fact = true;
    options.listInfo = true;
    options.cue = true;
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), test::makeWav(options)));
}

TEST(WavFormatTest, HeaderCheckRejectsFalseSignatures) {
    const Bytes file = test::makeWav();
    EXPECT_TRUE(testing::headerOf(format(), file).plausible);
    // The size of a recording that was never finished, and sizes too small for fmt and data.
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 4, le32(0xFFFFFFFF))).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 4, le32(0))).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 4, le32(35))).plausible);
    // A chunk id that is not text, a chunk beyond the RIFF data, a fmt with zero fields.
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 12, {0x00})).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 16, le32(0x7FFFFFFF))).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 22, le16(0))).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 20, le16(0))).plausible);
    // Other RIFF types do not even match the signature.
    const Bytes avi = overwritten(file, 8, {'A', 'V', 'I', ' '});
    EXPECT_FALSE(format().descriptor().signatures[0].matches(avi));
    // fmt does not have to come first.
    WavOptions broadcast;
    broadcast.junk = true;
    broadcast.bext = true;
    EXPECT_TRUE(testing::headerOf(format(), test::makeWav(broadcast)).plausible);
}

TEST(WavFormatTest, TheRiffSizeEndsTheFile) {
    WavOptions options;
    options.frames = 500;
    const Bytes file = test::makeWav(options);
    const std::vector<test::RiffChunk> chunks = test::riffChunks(file);
    const test::RiffChunk& data = chunk(chunks, "data");
    // A RIFF size smaller than the chunks: the data chunk runs beyond it.
    const Bytes smaller = overwritten(file, 4, le32(static_cast<std::uint32_t>(file.size() - 8 - 100)));
    const carving::EndDetection broken = endOf(format(), concat({smaller, testing::noise(1000, 1)}));
    EXPECT_EQ(broken.status, EndStatus::Broken) << testing::describe(broken);
    EXPECT_EQ(broken.length, data.offset);
    // A RIFF size larger than the chunks: what follows them is not a chunk.
    const Bytes larger = overwritten(file, 4, le32(static_cast<std::uint32_t>(file.size() - 8 + 1000)));
    const carving::EndDetection beyond = endOf(format(), concat({larger, testing::quietAudioNoise(2000, 2)}));
    EXPECT_NE(beyond.status, EndStatus::Found) << testing::describe(beyond);
    // Odd-sized data whose pad byte is missing at the very end: the RIFF size decides.
    WavOptions odd;
    odd.channels = 1;
    odd.bitsPerSample = 8;
    odd.frames = 101;
    const Bytes padded = test::makeWav(odd);
    Bytes unpadded = testing::prefix(padded, padded.size() - 1);
    unpadded = overwritten(unpadded, 4, le32(static_cast<std::uint32_t>(unpadded.size() - 8)));
    EXPECT_TRUE(isIntact(format(), unpadded));
}

TEST(WavFormatTest, FmtFieldsThatDisagreeAreCaught) {
    WavOptions options;
    options.frames = 400;
    const Bytes file = test::makeWav(options);  // PCM, 2 channels, 16 bits: block alignment 4
    const std::size_t fmt = chunk(test::riffChunks(file), "fmt ").offset + 8;
    const auto expectInvalid = [&](const Bytes& damaged, const std::string& what) {
        SCOPED_TRACE(what);
        EXPECT_EQ(endOf(format(), damaged).length, file.size());
        EXPECT_TRUE(isInvalid(format(), damaged));
    };
    expectInvalid(overwritten(file, fmt + 12, le16(3)), "block alignment");
    expectInvalid(overwritten(file, fmt + 8, le32(12345)), "byte rate");
    expectInvalid(overwritten(file, fmt + 14, le16(0)), "bits per sample");
    expectInvalid(overwritten(file, fmt + 2, le16(0)), "no channels");
    // A-law with 16 bits per sample, float with 24.
    WavOptions law;
    law.encoding = WavEncoding::ALaw;
    const Bytes alaw = test::makeWav(law);
    const std::size_t lawFmt = chunk(test::riffChunks(alaw), "fmt ").offset + 8;
    EXPECT_TRUE(isInvalid(format(), overwritten(alaw, lawFmt + 14, le16(16))));
    // Data that is not a whole number of blocks (the size and the RIFF size agree).
    const std::vector<test::RiffChunk> chunks = test::riffChunks(file);
    const test::RiffChunk& data = chunk(chunks, "data");
    Bytes ragged = overwritten(file, data.offset + 4, le32(static_cast<std::uint32_t>(data.size - 2)));
    ragged = overwritten(ragged, 4, le32(static_cast<std::uint32_t>(file.size() - 8 - 2)));
    ragged = testing::prefix(ragged, file.size() - 2);
    EXPECT_TRUE(isInvalid(format(), ragged));
    // WAVE_FORMAT_EXTENSIBLE: valid bits above the container, a subtype that wraps nothing.
    WavOptions extensible;
    extensible.extensible = true;
    extensible.bitsPerSample = 24;
    const Bytes wide = test::makeWav(extensible);
    const std::size_t wideFmt = chunk(test::riffChunks(wide), "fmt ").offset + 8;
    EXPECT_TRUE(isInvalid(format(), overwritten(wide, wideFmt + 18, le16(32))));
    EXPECT_TRUE(isInvalid(format(), overwritten(wide, wideFmt + 30, {0x42})));
}

TEST(WavFormatTest, ChunkOrderAndCountsAreChecked) {
    WavOptions options;
    options.listInfo = true;
    options.fact = true;
    const Bytes file = test::makeWav(options);
    const std::vector<test::RiffChunk> chunks = test::riffChunks(file);
    // data renamed: no data chunk.
    EXPECT_TRUE(isInvalid(format(), overwritten(file, chunk(chunks, "data").offset, {'d', 'a', 't', 'x'})));
    // fmt renamed: no fmt, and data before any fmt.
    EXPECT_TRUE(isInvalid(format(), overwritten(file, chunk(chunks, "fmt ").offset, {'f', 'm', 't', 'x'})));
    // fact renamed to a second fmt (too short for one).
    EXPECT_TRUE(isInvalid(format(), overwritten(file, chunk(chunks, "fact").offset, {'f', 'm', 't', ' '})));
    // A LIST whose sub-chunks do not fit in it.
    const test::RiffChunk& list = chunk(chunks, "LIST");
    EXPECT_TRUE(isInvalid(format(), overwritten(file, list.offset + 16, le32(1000))));
    // Unknown chunks are skipped.
    EXPECT_TRUE(isIntact(format(), overwritten(file, list.offset, {'x', 'y', 'z', 'w'})));
}

TEST(WavFormatTest, FragmentsInsideTheSamplesAreNotNoticed) {
    // PCM samples have no structure: another file's data over them changes
    // nothing a walk can check (L65 for audio: L71).
    WavOptions options;
    options.frames = 4000;
    const Bytes file = test::makeWav(options);
    const Bytes foreign = test::makeJpeg();
    EXPECT_TRUE(isIntact(format(), overwritten(file, 5000, foreign)));
    // Data inserted between fragments: the RIFF size still ends the file,
    // which then holds the gap and loses as much of its end.
    const Bytes inserted = testing::inserted(file, 5000, testing::noise(4096, 3));
    EXPECT_EQ(endOf(format(), inserted).length, file.size());
    // Chunks after the data move, and that breaks the walk.
    WavOptions listed = options;
    listed.listInfo = true;
    const Bytes withList = test::makeWav(listed);
    const Bytes shifted = testing::inserted(withList, 5000, testing::quietAudioNoise(4000, 4));
    EXPECT_NE(verdictOf(format(), testing::prefix(shifted, endOf(format(), shifted).length)).status,
              ValidationStatus::Valid);
}

TEST(WavFormatTest, FuzzedFilesStayWithinTheirData) {
    WavOptions options;
    options.frames = 50;
    options.junk = true;
    options.bext = true;
    options.fact = true;
    options.listInfo = true;
    options.cue = true;
    testing::fuzz(format(), test::makeWav(options), 3000, 71);
    options.encoding = WavEncoding::Float;
    options.extensible = true;
    options.bitsPerSample = 32;
    testing::fuzz(format(), test::makeWav(options), 1000, 72);
    for (const test::audio_samples::Sample& sample : test::audio_samples::all()) {
        if (sample.format == "wav") {
            testing::fuzz(format(), sample.data(), 200, 73);
        }
    }
}

}  // namespace
}  // namespace recovery::formats
