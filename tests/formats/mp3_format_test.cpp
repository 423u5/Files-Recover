// MP3: intact files from the builder (MPEG-1, 2 and 2.5, every channel mode,
// constant and variable bitrates, CRCs, info tags with and without a LAME
// extension, the bit reservoir, ID3v2, APEv2, Lyrics3 and ID3v1 tags) and
// from LAME and FFmpeg; truncation at every position; header rejection; the
// info tag's frame count; damage caught by CRCs, the LAME checksums, the side
// information and the bit reservoir; tags; false streams; fragments; fuzzing.

#include "formats/mp3_format.hpp"

#include "format_test_helpers.hpp"
#include "support/audio_builders.hpp"
#include "support/audio_samples.hpp"
#include "support/image_builders.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::ValidationStatus;
using test::Mp3Channels;
using test::Mp3InfoTag;
using test::Mp3Options;
using test::MpegVersion;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::isIntact;
using testing::isInvalid;
using testing::overwritten;
using testing::verdictOf;

const Mp3Format& format() {
    static const Mp3Format instance;
    return instance;
}

bool startsWith(const Bytes& file, std::size_t offset, std::string_view text) {
    if (offset + text.size() > file.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (file[offset + i] != static_cast<std::byte>(text[i])) {
            return false;
        }
    }
    return true;
}

// Side information size of the stream whose first frame header is at `offset`.
std::size_t sideInfoSize(const Bytes& file, std::size_t offset) {
    const auto b1 = static_cast<std::uint8_t>(file[offset + 1]);
    const auto b3 = static_cast<std::uint8_t>(file[offset + 3]);
    const bool mpeg1 = ((b1 >> 3) & 3) == 3;
    const bool mono = (b3 >> 6) == 3;
    return mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
}

bool hasInfoTag(const Bytes& file, const test::Mp3Frame& first) {
    const std::size_t at = first.offset + 4 + sideInfoSize(file, first.offset);
    return startsWith(file, at, "Xing") || startsWith(file, at, "Info") || startsWith(file, first.offset + 36, "VBRI");
}

// The prefix lengths of `file` that are complete MP3 files: every tag boundary
// after the last frame, and for a stream without an info tag, every frame
// boundary from the fourth frame on.
std::vector<std::size_t> completeLengths(const Bytes& file) {
    const std::vector<test::Mp3Frame> frames = test::mp3Frames(file);
    std::vector<std::size_t> lengths;
    if (frames.empty()) {
        return lengths;
    }
    if (!hasInfoTag(file, frames.front())) {
        for (std::size_t i = Mp3Format::kMinimumFrames - 1; i + 1 < frames.size(); ++i) {
            lengths.push_back(frames[i].offset + frames[i].length);
        }
    }
    std::size_t position = frames.back().offset + frames.back().length;
    while (position < file.size()) {
        lengths.push_back(position);
        if (startsWith(file, position, "APETAGEX")) {
            position += 32 + static_cast<std::uint8_t>(file[position + 12]) +
                        (std::size_t{static_cast<std::uint8_t>(file[position + 13])} << 8);
        } else if (startsWith(file, position, "LYRICSBEGIN")) {
            std::size_t end = position;
            while (!startsWith(file, end, "LYRICS200")) {
                ++end;
            }
            position = end + 9;
        } else if (startsWith(file, position, "TAG")) {
            position += 128;
        } else {
            ADD_FAILURE() << "unknown trailing data at " << position;
            break;
        }
    }
    return lengths;
}

// Prefixes shorter than this may be rejected: a stream without a leading
// ID3v2 tag, cut inside its first frame, is too little to be a file.
std::size_t rejectedBelow(const Bytes& file) {
    if (startsWith(file, 0, "ID3")) {
        return 0;
    }
    const std::vector<test::Mp3Frame> frames = test::mp3Frames(file);
    return frames.empty() ? 0 : frames.front().offset + frames.front().length;
}

// Sets `count` bits at bit `bit` of the frame at `offset` (most significant first).
Bytes withBits(Bytes file, std::size_t offset, std::size_t bit, unsigned count, std::uint32_t value) {
    for (unsigned i = 0; i < count; ++i) {
        const std::size_t position = bit + i;
        const auto mask = static_cast<std::byte>(0x80U >> (position % 8));
        std::byte& target = file.at(offset + position / 8);
        target = ((value >> (count - 1 - i)) & 1U) != 0 ? (target | mask) : (target & ~mask);
    }
    return file;
}

TEST(Mp3FormatTest, DescriptorIsValid) {
    RECOVERY_EXPECT_OK(carving::validateDescriptor(format().descriptor()));
    EXPECT_EQ(format().descriptor().id, "mp3");
    EXPECT_EQ(format().descriptor().signatures.size(), 4u);
    EXPECT_EQ(format().descriptor().endDetection, carving::EndDetectionMethod::StructureWalk);
    EXPECT_TRUE(format().descriptor().selfSynchronizing);
}

TEST(Mp3FormatTest, BuilderFilesOfEveryShapeAreIntact) {
    struct Shape {
        MpegVersion version;
        std::uint32_t rate;
        std::uint32_t bitrate;
    };
    const std::vector<Shape> shapes = {{MpegVersion::Mpeg1, 44100, 128}, {MpegVersion::Mpeg1, 48000, 320},
                                       {MpegVersion::Mpeg1, 32000, 0},   {MpegVersion::Mpeg2, 22050, 64},
                                       {MpegVersion::Mpeg2, 16000, 0},   {MpegVersion::Mpeg25, 8000, 8},
                                       {MpegVersion::Mpeg25, 12000, 0}};
    for (const Shape& shape : shapes) {
        for (const Mp3Channels channels :
             {Mp3Channels::Stereo, Mp3Channels::JointStereo, Mp3Channels::DualChannel, Mp3Channels::Mono}) {
            for (const bool crc : {false, true}) {
                for (const Mp3InfoTag tag : {Mp3InfoTag::None, Mp3InfoTag::Xing, Mp3InfoTag::Lame}) {
                    Mp3Options options;
                    options.version = shape.version;
                    options.sampleRate = shape.rate;
                    options.bitrate = shape.bitrate;
                    options.channels = channels;
                    options.crc = crc;
                    options.infoTag = tag;
                    options.reservoir = !crc;
                    SCOPED_TRACE(std::to_string(shape.rate) + " Hz " + std::to_string(shape.bitrate) + " kbit/s mode " +
                                 std::to_string(static_cast<int>(channels)) + (crc ? " crc" : "") + " tag " +
                                 std::to_string(static_cast<int>(tag)));
                    EXPECT_TRUE(isIntact(format(), test::makeMp3(options)));
                }
            }
        }
    }
}

TEST(Mp3FormatTest, TagsAroundTheFramesBelongToTheFile) {
    for (const std::uint8_t id3v2 : {std::uint8_t{0}, std::uint8_t{2}, std::uint8_t{3}, std::uint8_t{4}}) {
        Mp3Options options;
        options.id3v2 = id3v2;
        options.apeTag = id3v2 % 2 == 0;
        options.lyrics3 = id3v2 >= 3;
        options.id3v1 = true;
        SCOPED_TRACE("ID3v2." + std::to_string(id3v2));
        EXPECT_TRUE(isIntact(format(), test::makeMp3(options)));
    }
    // A cover picture in the ID3v2 tag: a whole JPEG inside the file.
    Mp3Options picture;
    picture.id3v2 = 3;
    picture.picture = test::makeJpeg();
    EXPECT_TRUE(isIntact(format(), test::makeMp3(picture)));
    // Tags without an info tag, and the smallest stream the format accepts.
    Mp3Options plain;
    plain.infoTag = Mp3InfoTag::None;
    plain.frames = Mp3Format::kMinimumFrames;
    plain.id3v1 = true;
    EXPECT_TRUE(isIntact(format(), test::makeMp3(plain)));
    Mp3Options smallest;
    smallest.version = MpegVersion::Mpeg2;
    smallest.sampleRate = 24000;
    smallest.bitrate = 8;
    smallest.channels = Mp3Channels::Mono;
    smallest.infoTag = Mp3InfoTag::None;
    smallest.frames = Mp3Format::kMinimumFrames;
    const Bytes tiny = test::makeMp3(smallest);
    EXPECT_TRUE(isIntact(format(), tiny));
    EXPECT_EQ(tiny.size(), Mp3Format::kMinimumSize);
}

TEST(Mp3FormatTest, FilesFromRealEncodersAreIntact) {
    std::size_t checked = 0;
    for (const test::audio_samples::Sample& sample : test::audio_samples::all()) {
        if (sample.format != "mp3") {
            continue;
        }
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes file = sample.data();
        EXPECT_TRUE(isIntact(format(), file));
        const std::vector<std::size_t> complete = completeLengths(file);
        EXPECT_TRUE(testing::prefixesAreTruncated(format(), file, complete, rejectedBelow(file)));
        ++checked;
    }
    EXPECT_GE(checked, 8u);
}

TEST(Mp3FormatTest, EveryPrefixIsTruncatedUnlessItIsAFileItself) {
    Mp3Options options;
    options.crc = true;
    options.id3v2 = 4;
    options.apeTag = true;
    options.lyrics3 = true;
    options.id3v1 = true;
    const Bytes tagged = test::makeMp3(options);
    const std::vector<std::size_t> complete = completeLengths(tagged);
    ASSERT_EQ(complete.size(), 3u);  // before APE, before Lyrics3, before ID3v1
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), tagged, complete));

    // Without an info tag nothing says how long the stream is: cut at a frame
    // boundary, it is a complete (shorter) file (L68).
    Mp3Options bare;
    bare.infoTag = Mp3InfoTag::None;
    bare.frames = 12;
    const Bytes stream = test::makeMp3(bare);
    const std::vector<std::size_t> boundaries = completeLengths(stream);
    ASSERT_EQ(boundaries.size(), 12u - Mp3Format::kMinimumFrames);
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), stream, boundaries, rejectedBelow(stream)));
}

TEST(Mp3FormatTest, BytesAfterTheLastFrameOrTagAreNotPartOfTheFile) {
    Mp3Options options;
    options.id3v1 = true;
    const Bytes file = test::makeMp3(options);
    const Bytes longer = concat({file, testing::noise(50, 6)});
    EXPECT_TRUE(isInvalid(format(), longer, file.size()));
    EXPECT_EQ(endOf(format(), longer).length, file.size());
    // Erased flash and zeros after it are not frames either.
    for (const std::byte fill : {std::byte{0xFF}, std::byte{0x00}}) {
        const carving::EndDetection end = endOf(format(), concat({file, Bytes(1 << 20, fill)}));
        EXPECT_EQ(end.status, EndStatus::Found) << testing::describe(end);
        EXPECT_EQ(end.length, file.size());
    }
}

TEST(Mp3FormatTest, HeaderCheckRejectsFalseSignatures) {
    Mp3Options options;
    options.infoTag = Mp3InfoTag::None;
    options.frames = 30;
    const Bytes file = test::makeMp3(options);
    const std::vector<test::Mp3Frame> frames = test::mp3Frames(file);
    EXPECT_TRUE(testing::headerOf(format(), file).plausible);
    // A frame in the middle of the stream whose main data begins in an earlier frame.
    std::size_t middle = 0;
    for (std::size_t i = 1; i < frames.size() && middle == 0; ++i) {
        const std::size_t at = frames[i].offset;
        const std::uint32_t begin = (std::uint32_t{static_cast<std::uint8_t>(file[at + 4])} << 1) |
                                    (static_cast<std::uint8_t>(file[at + 5]) >> 7);
        if (begin != 0) {
            middle = at;
        }
    }
    ASSERT_NE(middle, 0u);
    // It is accepted (after a break, the rest of a stream starts there), and
    // validation says the stream's start is missing.
    const testing::Bytes fromMiddle(file.begin() + static_cast<std::ptrdiff_t>(middle), file.end());
    EXPECT_TRUE(testing::headerOf(format(), fromMiddle).plausible);
    EXPECT_EQ(endOf(format(), fromMiddle).length, fromMiddle.size());
    const carving::ValidationResult tail = verdictOf(format(), fromMiddle);
    EXPECT_EQ(tail.status, ValidationStatus::Invalid) << testing::describe(tail);
    EXPECT_NE(tail.detail.find("start is missing"), std::string::npos);
    // Invalid frame headers: reserved version, Layer I and II, free and bad bitrates,
    // reserved sample rate and emphasis.
    struct Damage {
        std::size_t offset;
        int value;
    };
    for (const Damage& damage : std::vector<Damage>{
             {1, 0xEB}, {1, 0xFF}, {1, 0xFD}, {2, 0x00}, {2, 0xF0}, {2, 0x9C}, {3, 0x46}}) {
        const Bytes damaged = overwritten(file, damage.offset, {static_cast<std::uint8_t>(damage.value)});
        EXPECT_FALSE(testing::headerOf(format(), damaged).plausible) << damage.offset << " " << damage.value;
    }
    // A single frame followed by something else.
    const Bytes lone = concat({testing::prefix(file, frames[0].length), testing::quietAudioNoise(4096, 3)});
    EXPECT_FALSE(testing::headerOf(format(), lone).plausible);
    // Impossible side information: big_values above 288 in the first granule.
    EXPECT_FALSE(testing::headerOf(format(), withBits(file, 4, 32, 9, 300)).plausible);
    // ID3v2 headers: versions 2 to 4 with syncsafe sizes only.
    const Bytes id3 = test::id3v2Tag(3);
    EXPECT_TRUE(testing::headerOf(format(), id3).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(id3, 3, {5})).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(id3, 7, {0x80})).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(id3, 5, {0x01})).plausible);
}

TEST(Mp3FormatTest, TheInfoTagsFrameCountEndsTheStream) {
    Mp3Options first;
    first.frames = 15;
    Mp3Options second = first;
    second.infoTag = Mp3InfoTag::None;
    second.seed = 99;
    const Bytes a = test::makeMp3(first);
    const Bytes b = test::makeMp3(second);
    // The same parameters, back to back: the count says where the first file ends.
    const Bytes both = concat({a, b});
    const carving::EndDetection end = endOf(format(), both);
    EXPECT_EQ(end.status, EndStatus::Found) << testing::describe(end);
    EXPECT_EQ(end.length, a.size());
    // Without an info tag, another file's info tag ends a stream too.
    const Bytes reversed = concat({b, a});
    EXPECT_EQ(endOf(format(), reversed).length, b.size());
    // Fewer frames than the count: the stream breaks where they stop.
    const std::vector<test::Mp3Frame> frames = test::mp3Frames(a);
    const std::size_t cut = frames[10].offset;
    const Bytes stopped = concat({testing::prefix(a, cut), testing::quietAudioNoise(4096, 5)});
    const carving::EndDetection broken = endOf(format(), stopped);
    EXPECT_EQ(broken.status, EndStatus::Broken) << testing::describe(broken);
    EXPECT_EQ(broken.length, cut);
    EXPECT_EQ(verdictOf(format(), testing::prefix(a, cut)).status, ValidationStatus::Truncated);
}

TEST(Mp3FormatTest, DamageIsCaughtByCrcsAndTheLameChecksums) {
    Mp3Options options;
    options.crc = true;
    const Bytes file = test::makeMp3(options);
    const std::vector<test::Mp3Frame> frames = test::mp3Frames(file);
    ASSERT_GT(frames.size(), 6u);
    const std::size_t frame = frames[5].offset;
    // Side information under the frame's CRC.
    const Bytes badSide = overwritten(file, frame + 7, {static_cast<std::uint8_t>(~static_cast<std::uint8_t>(file[frame + 7]))});
    EXPECT_EQ(endOf(format(), badSide).length, file.size());
    EXPECT_TRUE(isInvalid(format(), badSide, frame));
    // Main data is not under the frame's CRC, but under the LAME tag's music CRC.
    const std::size_t data = frame + frames[5].length - 3;
    const Bytes badData = overwritten(file, data, {static_cast<std::uint8_t>(~static_cast<std::uint8_t>(file[data]))});
    EXPECT_EQ(endOf(format(), badData).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), badData));
    // The LAME tag itself (its lowpass byte) is under the tag CRC.
    const std::size_t lowpass = frames[0].offset + 4 + 32 + 120 + 10;
    const Bytes badTag = overwritten(file, lowpass, {0x01});
    EXPECT_TRUE(isInvalid(format(), badTag));
    // Without the LAME extension the frame CRC is the only check of that data.
    options.infoTag = Mp3InfoTag::Xing;
    const Bytes xing = test::makeMp3(options);
    const std::size_t xingData = test::mp3Frames(xing)[5].offset + frames[5].length - 3;
    EXPECT_EQ(verdictOf(format(), overwritten(xing, xingData, {0x5A})).status, ValidationStatus::Valid);
}

TEST(Mp3FormatTest, TheBitReservoirAndSideInformationAreChecked) {
    // Without the reservoir every frame's main data starts in its own area, so
    // pointing 511 bytes back always reaches into the previous frame's data.
    Mp3Options options;
    options.infoTag = Mp3InfoTag::None;
    options.reservoir = false;
    options.frames = 30;
    const Bytes file = test::makeMp3(options);
    const std::vector<test::Mp3Frame> frames = test::mp3Frames(file);
    const std::size_t frame = frames[10].offset;
    // MPEG-1 stereo side information without CRC, in bits from the frame's
    // start: main_data_begin at 32, then per granule and channel (59 bits
    // each, from 52) part2_3_length, big_values at +12, the window switching
    // flag at +33 and the block type at +34.
    EXPECT_TRUE(isInvalid(format(), withBits(file, frame, 32, 9, 511), frame));
    // Main data larger than the frame's area.
    EXPECT_TRUE(isInvalid(format(), withBits(file, frame, 52, 12, 4095), frame));
    // big_values beyond 288.
    EXPECT_TRUE(isInvalid(format(), withBits(file, frame, 64, 9, 400), frame));
    // A window switch to a normal block.
    EXPECT_TRUE(isInvalid(format(), withBits(file, frame, 85, 3, 0b100), frame));
    // The layout is intact every time.
    EXPECT_EQ(endOf(format(), withBits(file, frame, 32, 9, 511)).length, file.size());
    EXPECT_EQ(verdictOf(format(), file).status, ValidationStatus::Valid);
}

TEST(Mp3FormatTest, TagsAreWalkedButOnlyTheirLayoutIsChecked) {
    Mp3Options options;
    options.id3v2 = 3;
    options.apeTag = true;
    options.id3v1 = true;
    const Bytes file = test::makeMp3(options);
    const std::size_t tag = test::id3v2Tag(3).size();
    // A frame inside the ID3v2 tag that runs beyond it.
    const Bytes oversized = overwritten(file, 10 + 4, {0x7F, 0x00, 0x00, 0x00});
    EXPECT_EQ(endOf(format(), oversized).length, file.size());
    EXPECT_TRUE(isInvalid(format(), oversized, 0));
    // A frame id that is not upper-case letters and digits.
    EXPECT_TRUE(isInvalid(format(), overwritten(file, 10, {'t'}), 0));
    // Version 2.4 frame sizes written as plain integers (as iTunes did) still fit.
    Mp3Options itunes;
    itunes.id3v2 = 4;
    itunes.picture = testing::noise(300, 3);
    Bytes plain = test::makeMp3(itunes);
    const std::size_t picture = 10 + 10 + 11 + 10 + 12;  // TIT2 and TPE1 frames before it
    ASSERT_TRUE(startsWith(plain, picture, "APIC"));
    plain = overwritten(plain, picture + 4, {0x00, 0x00, 0x01, 0x3A});  // 314, not syncsafe
    EXPECT_EQ(verdictOf(format(), plain).status, ValidationStatus::Valid);
    // An APE tag without its footer.
    const std::size_t ape = file.size() - 128 - test::apeTag().size();
    ASSERT_TRUE(startsWith(file, ape, "APETAGEX"));
    const std::size_t footer = file.size() - 128 - 32;
    EXPECT_TRUE(isInvalid(format(), overwritten(file, footer, {'X'})));
    // Frames right after the tag: the tag's size decides where they are.
    EXPECT_TRUE(startsWith(file, tag, std::string_view("\xFF", 1)));
}

TEST(Mp3FormatTest, FalseStreamsAreNotFiles) {
    // An ID3v2 tag with no frames after it.
    const Bytes lonely = concat({test::id3v2Tag(4), testing::quietAudioNoise(8192, 7)});
    EXPECT_TRUE(testing::headerOf(format(), lonely).plausible);
    const carving::EndDetection end = endOf(format(), lonely);
    EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
    EXPECT_EQ(end.length, 0u);
    // A tag followed by raw AAC is not an MP3 either.
    test::AacOptions aac;
    aac.id3v2 = 3;
    EXPECT_EQ(endOf(format(), test::makeAdts(aac)).length, 0u);
    // Three frames are too few.
    Mp3Options options;
    options.infoTag = Mp3InfoTag::None;
    options.frames = 3;
    const Bytes three = concat({test::makeMp3(options), testing::quietAudioNoise(4096, 8)});
    EXPECT_EQ(endOf(format(), three).length, 0u);
    // A flood of frame syncs in noise never holds together.
    const Bytes noise = testing::noise(1 << 20, 9);
    std::size_t accepted = 0;
    for (std::size_t i = 0; i + 4096 < noise.size(); ++i) {
        if (noise[i] == std::byte{0xFF}) {
            accepted += testing::headerOf(format(), std::span(noise).subspan(i, 4096)).plausible ? 1 : 0;
        }
    }
    EXPECT_EQ(accepted, 0u);
}

TEST(Mp3FormatTest, FragmentsAreNoticedWhereTheStructureAllows) {
    Mp3Options options;
    options.frames = 40;
    const Bytes file = test::makeMp3(options);
    const std::vector<test::Mp3Frame> frames = test::mp3Frames(file);
    const std::size_t gap = frames[20].offset;
    // Foreign data between two fragments: the frames stop before the count.
    const Bytes inserted = testing::inserted(file, gap, testing::quietAudioNoise(4096, 10));
    EXPECT_EQ(endOf(format(), inserted).status, EndStatus::Broken);
    EXPECT_EQ(endOf(format(), inserted).length, gap);
    // Frames of another stream with the same parameters in the middle: the
    // layout holds, but the music CRC does not.
    Mp3Options other = options;
    other.seed = 77;
    other.infoTag = Mp3InfoTag::None;
    const Bytes foreign = test::makeMp3(other);
    const std::vector<test::Mp3Frame> foreignFrames = test::mp3Frames(foreign);
    Bytes spliced = file;
    for (std::size_t i = 20; i < 25; ++i) {
        // frames[0] is the info tag's frame: audio frame i - 1 of both streams.
        const test::Mp3Frame& replacement = foreignFrames[i - 1];
        ASSERT_EQ(frames[i].length, replacement.length);
        std::copy(foreign.begin() + static_cast<std::ptrdiff_t>(replacement.offset),
                  foreign.begin() + static_cast<std::ptrdiff_t>(replacement.offset + replacement.length),
                  spliced.begin() + static_cast<std::ptrdiff_t>(frames[i].offset));
    }
    EXPECT_EQ(endOf(format(), spliced).length, file.size());
    EXPECT_TRUE(isInvalid(format(), spliced));
    // Without an info tag the same splice goes unnoticed unless the reservoir
    // gives it away (L69).
    Mp3Options bare = options;
    bare.infoTag = Mp3InfoTag::None;
    const Bytes stream = test::makeMp3(bare);
    const Bytes cut = concat({testing::prefix(stream, test::mp3Frames(stream)[20].offset), testing::noise(512, 4)});
    const carving::EndDetection end = endOf(format(), cut);
    EXPECT_EQ(end.status, EndStatus::Found) << testing::describe(end);
    EXPECT_EQ(verdictOf(format(), testing::prefix(cut, end.length)).status, ValidationStatus::Valid);
}

TEST(Mp3FormatTest, FuzzedFilesStayWithinTheirData) {
    Mp3Options options;
    options.crc = true;
    options.id3v2 = 4;
    options.apeTag = true;
    options.lyrics3 = true;
    options.id3v1 = true;
    options.frames = 12;
    testing::fuzz(format(), test::makeMp3(options), 3000, 51);
    options.infoTag = Mp3InfoTag::None;
    options.crc = false;
    options.bitrate = 0;
    testing::fuzz(format(), test::makeMp3(options), 2000, 52);
    for (const test::audio_samples::Sample& sample : test::audio_samples::all()) {
        if (sample.format == "mp3") {
            testing::fuzz(format(), sample.data(), 300, 53);
        }
    }
}

}  // namespace
}  // namespace recovery::formats
