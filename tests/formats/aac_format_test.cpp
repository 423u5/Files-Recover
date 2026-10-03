// Raw AAC (ADTS): intact files from the builder (sampling frequencies,
// channel configurations, profiles, MPEG-2 and MPEG-4 headers, ID3 tags) and
// from FFmpeg, faac and fdkaac; truncation at every position; header
// rejection; streams that end where their headers change; false streams;
// fragments; fuzzing.

#include "formats/aac_format.hpp"

#include "format_test_helpers.hpp"
#include "support/audio_builders.hpp"
#include "support/audio_samples.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <string>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::ValidationStatus;
using test::AacOptions;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::isIntact;
using testing::overwritten;
using testing::verdictOf;

const AacFormat& format() {
    static const AacFormat instance;
    return instance;
}

// The prefix lengths that are complete files: every frame boundary from the
// fourth frame on (nothing records the stream's length), and the start of an
// ID3v1 tag after the frames.
std::vector<std::size_t> completeLengths(const Bytes& file) {
    const std::vector<std::size_t> frames = test::adtsFrames(file);
    std::vector<std::size_t> lengths;
    for (std::size_t i = AacFormat::kMinimumFrames; i < frames.size(); ++i) {
        lengths.push_back(frames[i]);
    }
    const auto last = frames.back();
    const std::size_t length = ((static_cast<std::uint8_t>(file[last + 3]) & 3U) << 11) |
                               (std::size_t{static_cast<std::uint8_t>(file[last + 4])} << 3) |
                               (static_cast<std::uint8_t>(file[last + 5]) >> 5);
    if (last + length < file.size()) {
        lengths.push_back(last + length);
    }
    return lengths;
}

TEST(AacFormatTest, DescriptorIsValid) {
    RECOVERY_EXPECT_OK(carving::validateDescriptor(format().descriptor()));
    EXPECT_EQ(format().descriptor().id, "aac");
    EXPECT_EQ(format().descriptor().signatures.size(), 2u);
    EXPECT_EQ(format().descriptor().endDetection, carving::EndDetectionMethod::StructureWalk);
    EXPECT_TRUE(format().descriptor().selfSynchronizing);
}

TEST(AacFormatTest, BuilderFilesOfEveryShapeAreIntact) {
    for (const std::uint32_t rate : {8000U, 22050U, 44100U, 48000U, 96000U}) {
        for (const std::uint8_t channels : {std::uint8_t{1}, std::uint8_t{2}}) {
            for (const std::uint8_t profile : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{3}}) {
                for (const bool mpeg2 : {false, true}) {
                    AacOptions options;
                    options.sampleRate = rate;
                    options.channels = channels;
                    options.profile = profile;
                    options.mpeg2 = mpeg2;
                    SCOPED_TRACE(std::to_string(rate) + " Hz, " + std::to_string(channels) + " channels, profile " +
                                 std::to_string(profile) + (mpeg2 ? ", MPEG-2" : ", MPEG-4"));
                    EXPECT_TRUE(isIntact(format(), test::makeAdts(options)));
                }
            }
        }
    }
    AacOptions tagged;
    tagged.id3v2 = 4;
    tagged.id3v1 = true;
    EXPECT_TRUE(isIntact(format(), test::makeAdts(tagged)));
    AacOptions smallest;
    smallest.frames = AacFormat::kMinimumFrames;
    EXPECT_TRUE(isIntact(format(), test::makeAdts(smallest)));
}

TEST(AacFormatTest, FilesFromRealEncodersAreIntact) {
    std::size_t checked = 0;
    for (const test::audio_samples::Sample& sample : test::audio_samples::all()) {
        if (sample.format != "aac") {
            continue;
        }
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes file = sample.data();
        EXPECT_TRUE(isIntact(format(), file));
        // Cut inside its first frame, a stream without an ID3v2 tag is too little to be a file.
        const std::size_t second = test::adtsFrames(file).at(1);
        EXPECT_TRUE(testing::prefixesAreTruncated(format(), file, completeLengths(file), second));
        ++checked;
    }
    EXPECT_GE(checked, 3u);  // FFmpeg, faac and fdkaac
}

TEST(AacFormatTest, EveryPrefixIsTruncatedUnlessItIsAFileItself) {
    // Nothing records the stream's length: cut at a frame boundary, it is a
    // complete (shorter) file (L68).
    AacOptions options;
    options.id3v2 = 3;
    options.id3v1 = true;
    options.frames = 12;
    const Bytes file = test::makeAdts(options);
    const std::vector<std::size_t> complete = completeLengths(file);
    ASSERT_EQ(complete.size(), 12u - AacFormat::kMinimumFrames + 1);
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), file, complete));
}

TEST(AacFormatTest, HeaderCheckRejectsFalseSignatures) {
    const Bytes file = test::makeAdts();
    EXPECT_TRUE(testing::headerOf(format(), file).plausible);
    // A layer other than 0, reserved and escape sampling frequencies, a frame
    // shorter than its header.
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 1, {0xF3})).plausible);
    for (const std::uint8_t index : {std::uint8_t{13}, std::uint8_t{14}, std::uint8_t{15}}) {
        const auto byte2 = static_cast<std::uint8_t>((static_cast<std::uint8_t>(file[2]) & 0xC3) | (index << 2));
        EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 2, {byte2})).plausible) << int{index};
    }
    const Bytes tiny = overwritten(file, 3, {static_cast<std::uint8_t>(static_cast<std::uint8_t>(file[3]) & 0xFC),
                                             0x00, 0x1F});
    EXPECT_FALSE(testing::headerOf(format(), tiny).plausible);
    // The second frame with another sampling frequency or channel configuration.
    const std::size_t second = test::adtsFrames(file)[1];
    const auto rate = static_cast<std::uint8_t>(static_cast<std::uint8_t>(file[second + 2]) ^ 0x04);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, second + 2, {rate})).plausible);
    // A frame followed by something else.
    const Bytes lone = concat({testing::prefix(file, second), testing::quietAudioNoise(4096, 2)});
    EXPECT_FALSE(testing::headerOf(format(), lone).plausible);
    // The private bit may change from frame to frame.
    const auto bit = static_cast<std::uint8_t>(static_cast<std::uint8_t>(file[second + 2]) ^ 0x02);
    EXPECT_TRUE(isIntact(format(), overwritten(file, second + 2, {bit})));
}

TEST(AacFormatTest, AStreamEndsWhereItsHeadersChange) {
    AacOptions first;
    first.frames = 10;
    AacOptions second = first;
    second.sampleRate = 48000;
    second.seed = 5;
    const Bytes a = test::makeAdts(first);
    const Bytes b = test::makeAdts(second);
    const carving::EndDetection end = endOf(format(), concat({a, b}));
    EXPECT_EQ(end.status, EndStatus::Found) << testing::describe(end);
    EXPECT_EQ(end.length, a.size());
    // Two streams with the same parameters, back to back, are one stream (L70).
    AacOptions same = first;
    same.seed = 6;
    const Bytes c = test::makeAdts(same);
    EXPECT_EQ(endOf(format(), concat({a, c})).length, a.size() + c.size());
}

TEST(AacFormatTest, FalseStreamsAreNotFiles) {
    // An ID3v2 tag followed by MP3 frames.
    test::Mp3Options mp3;
    mp3.id3v2 = 3;
    const carving::EndDetection mp3End = endOf(format(), test::makeMp3(mp3));
    EXPECT_EQ(mp3End.status, EndStatus::Broken) << testing::describe(mp3End);
    EXPECT_EQ(mp3End.length, 0u);
    // Three frames are too few.
    AacOptions three;
    three.frames = 3;
    EXPECT_EQ(endOf(format(), concat({test::makeAdts(three), testing::quietAudioNoise(4096, 3)})).length, 0u);
    // Frame syncs in noise never chain into a stream. A header claiming a
    // frame longer than the header check can see past is only refused by end
    // detection, which needs four frames.
    const Bytes noise = testing::noise(1 << 20, 4);
    std::size_t plausible = 0;
    for (std::size_t i = 0; i + 16384 < noise.size(); ++i) {
        if (noise[i] != std::byte{0xFF}) {
            continue;
        }
        const std::span<const std::byte> hit = std::span(noise).subspan(i, 16384);
        if (testing::headerOf(format(), hit).plausible) {
            ++plausible;
            EXPECT_EQ(endOf(format(), hit).length, 0u) << "at " << i;
        }
    }
    // About one in twenty of the 250 or so sync positions in a MiB of noise:
    // frames longer than 4 KiB are only possible for 6 and 8 channels or a
    // program config element.
    EXPECT_LE(plausible, 16u);
}

TEST(AacFormatTest, FragmentsEndTheStreamAtTheGap) {
    AacOptions options;
    options.frames = 30;
    const Bytes file = test::makeAdts(options);
    const std::size_t gap = test::adtsFrames(file)[15];
    // Foreign data between the fragments ends the stream there, and what came
    // before is a complete stream of its own: only the headers can be checked
    // (L68, L70).
    const Bytes inserted = testing::inserted(file, gap, testing::quietAudioNoise(4096, 5));
    const carving::EndDetection end = endOf(format(), inserted);
    EXPECT_EQ(end.status, EndStatus::Found) << testing::describe(end);
    EXPECT_EQ(end.length, gap);
    EXPECT_EQ(verdictOf(format(), testing::prefix(inserted, gap)).status, ValidationStatus::Valid);
    // Data overwritten inside a frame, with the headers intact, goes unnoticed.
    const std::size_t inside = test::adtsFrames(file)[10] + 9;
    EXPECT_TRUE(isIntact(format(), overwritten(file, inside, {0x12, 0x34, 0x56})));
}

TEST(AacFormatTest, FuzzedFilesStayWithinTheirData) {
    AacOptions options;
    options.id3v2 = 4;
    options.id3v1 = true;
    options.frames = 12;
    testing::fuzz(format(), test::makeAdts(options), 3000, 61);
    for (const test::audio_samples::Sample& sample : test::audio_samples::all()) {
        if (sample.format == "aac") {
            testing::fuzz(format(), sample.data(), 500, 62);
        }
    }
}

}  // namespace
}  // namespace recovery::formats
