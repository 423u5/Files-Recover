// WAV and MP3 media validation: PCM, IEEE float, A-law and mu-law samples
// have nothing coded; the block headers of IMA and Microsoft ADPCM in
// builder files, SoX's files and files written here, with step indexes,
// reserved bytes and predictors the structure does not look at; other
// codings and MPEG audio are not decoded; truncation and fuzzing.

#include "validation/media_test_helpers.hpp"

#include "formats/format_test_helpers.hpp"
#include "recovery/byte_order.hpp"
#include "support/audio_builders.hpp"
#include "support/audio_samples.hpp"
#include "support/image_builders.hpp"
#include "support/test_files.hpp"

#include <gtest/gtest.h>

#include <array>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::validation {
namespace {

using formats::testing::overwritten;
using testing::Bytes;
using testing::mediaIs;
using testing::mediaOf;
using testing::validated;
namespace audio = test::audio_samples;

// The offset of the payload of the first chunk `id` of a RIFF file.
std::size_t payloadOf(const Bytes& file, std::string_view id) {
    for (const test::RiffChunk& chunk : test::riffChunks(file)) {
        if (chunk.id == id) {
            return chunk.offset + 8;
        }
    }
    ADD_FAILURE() << "no " << id << " chunk";
    return 0;
}

Bytes imaWav(std::uint16_t channels, std::uint32_t sampleRate, std::size_t blocks) {
    test::WavOptions options;
    options.encoding = test::WavEncoding::ImaAdpcm;
    options.channels = channels;
    options.sampleRate = sampleRate;
    options.frames = blocks;
    return test::makeWav(options);
}

Bytes riffWave(std::initializer_list<std::span<const std::byte>> chunks) {
    Bytes file(12);
    file[0] = std::byte{'R'};
    file[1] = std::byte{'I'};
    file[2] = std::byte{'F'};
    file[3] = std::byte{'F'};
    file[8] = std::byte{'W'};
    file[9] = std::byte{'A'};
    file[10] = std::byte{'V'};
    file[11] = std::byte{'E'};
    for (const std::span<const std::byte> chunk : chunks) {
        file.insert(file.end(), chunk.begin(), chunk.end());
    }
    storeLe32(file, 4, static_cast<std::uint32_t>(file.size() - 8));
    return file;
}

// Microsoft ADPCM at 8000 Hz in blocks of 256 bytes per channel, as SoX
// writes it: fmt declares the 7 standard coefficient pairs and
// `extraPairs` more, and block b uses predictor b modulo their count.
Bytes msAdpcmWav(std::uint16_t channels, std::size_t blocks, std::uint16_t extraPairs = 0) {
    static constexpr std::array<std::int16_t, 14> kStandard = {256, 0,   512, -256, 0,   0,    192,
                                                               64,  240, 0,   460,  -208, 392, -232};
    const auto blockAlign = static_cast<std::uint16_t>(256 * channels);
    const auto samplesPerBlock = static_cast<std::uint16_t>((blockAlign - 7 * channels) * 8 / (4 * channels) + 2);
    const auto pairs = static_cast<std::uint16_t>(7 + extraPairs);
    Bytes fmt(22 + 4 * std::size_t{pairs});
    storeLe16(fmt, 0, 0x0002);
    storeLe16(fmt, 2, channels);
    storeLe32(fmt, 4, 8000);
    storeLe32(fmt, 8, 8000U * blockAlign / samplesPerBlock);
    storeLe16(fmt, 12, blockAlign);
    storeLe16(fmt, 14, 4);
    storeLe16(fmt, 16, static_cast<std::uint16_t>(4 + 4 * pairs));
    storeLe16(fmt, 18, samplesPerBlock);
    storeLe16(fmt, 20, pairs);
    for (std::size_t i = 0; i < 2 * std::size_t{pairs}; ++i) {
        storeLe16(fmt, 22 + 2 * i, static_cast<std::uint16_t>(i < kStandard.size() ? kStandard[i] : 128));
    }
    Bytes data;
    for (std::size_t block = 0; block < blocks; ++block) {
        Bytes bytes = test::makePattern(blockAlign, block + 1);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            bytes[channel] = static_cast<std::byte>(block % pairs);
            storeLe16(bytes, channels + 2 * channel, static_cast<std::uint16_t>(16 + block));  // initial delta
        }
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
    Bytes fact(4);
    storeLe32(fact, 0, static_cast<std::uint32_t>(blocks * samplesPerBlock));
    return riffWave({test::riffChunk("fmt ", fmt), test::riffChunk("fact", fact), test::riffChunk("data", data)});
}

// `file` with its data chunk (the last chunk) cut to `size` bytes.
Bytes withDataSize(Bytes file, std::size_t size) {
    const std::size_t data = payloadOf(file, "data");
    file.resize(data + size + (size & 1U));
    storeLe32(file, data - 4, static_cast<std::uint32_t>(size));
    storeLe32(file, 4, static_cast<std::uint32_t>(file.size() - 8));
    return file;
}

TEST(WavMediaTest, StoredSamplesHaveNothingCoded) {
    for (const test::WavEncoding encoding :
         {test::WavEncoding::Pcm, test::WavEncoding::Float, test::WavEncoding::ALaw, test::WavEncoding::MuLaw}) {
        for (const bool extensible : {false, true}) {
            test::WavOptions options;
            options.encoding = encoding;
            options.bitsPerSample = encoding == test::WavEncoding::Float ? 32 : 16;
            options.extensible = extensible;
            const ValidationState state = validated("wav", test::makeWav(options));
            EXPECT_EQ(state.structural.status, LevelStatus::Passed) << testing::describe(state);
            EXPECT_EQ(state.media.status, LevelStatus::NotApplicable) << testing::describe(state);
            EXPECT_EQ(state.status(), carving::ValidationStatus::Valid);
        }
    }
    int stored = 0;
    for (const audio::Sample& sample : audio::all()) {
        if (sample.format == "wav" && sample.name.find("adpcm") == std::string_view::npos) {
            const ValidationState state = validated("wav", sample.data());
            EXPECT_EQ(state.media.status, LevelStatus::NotApplicable) << sample.name << testing::describe(state);
            EXPECT_EQ(state.status(), carving::ValidationStatus::Valid) << sample.name;
            ++stored;
        }
    }
    EXPECT_GE(stored, 10);
}

TEST(WavMediaTest, AdpcmBlockHeadersOfBuilderAndSoxFilesPass) {
    for (const int channels : {1, 2}) {
        for (const std::uint32_t rate : {8000U, 22050U, 44100U}) {
            EXPECT_TRUE(testing::passesEveryLevel("wav", imaWav(static_cast<std::uint16_t>(channels), rate, 12)))
                << channels << " " << rate;
        }
        EXPECT_TRUE(testing::passesEveryLevel("wav", msAdpcmWav(static_cast<std::uint16_t>(channels), 9)));
    }
    EXPECT_TRUE(mediaIs("wav", imaWav(2, 44100, 12), LevelStatus::Passed,
                        "IMA ADPCM, 2 channels: 12 block headers valid"));
    for (const std::string_view name : {"sox_ima_adpcm.wav", "sox_ms_adpcm.wav"}) {
        EXPECT_TRUE(testing::passesEveryLevel("wav", audio::named(name).data())) << name;
    }
    EXPECT_TRUE(mediaIs("wav", audio::named("sox_ima_adpcm.wav").data(), LevelStatus::Passed,
                        "IMA ADPCM, 1 channel: 4 block headers valid"));
    EXPECT_TRUE(mediaIs("wav", audio::named("sox_ms_adpcm.wav").data(), LevelStatus::Passed,
                        "Microsoft ADPCM, 1 channel: 4 block headers valid"));
    // Coefficient pairs beyond the standard 7 make more predictors valid.
    EXPECT_TRUE(mediaIs("wav", msAdpcmWav(2, 12, 3), LevelStatus::Passed, "12 block headers valid"));
}

TEST(WavMediaTest, ALastBlockShorterThanTheOthersIsChecked) {
    const Bytes ima = imaWav(1, 8000, 4);
    // The last block holds a header and 96 bytes of samples, or only part of a header.
    EXPECT_TRUE(mediaIs("wav", withDataSize(ima, 3 * 256 + 100), LevelStatus::Passed, "4 block headers valid"));
    EXPECT_TRUE(mediaIs("wav", withDataSize(ima, 3 * 256 + 3), LevelStatus::Passed, "3 block headers valid"));
    const Bytes damaged = overwritten(withDataSize(ima, 3 * 256 + 100), payloadOf(ima, "data") + 3 * 256 + 2, {90});
    EXPECT_TRUE(mediaIs("wav", damaged, LevelStatus::Failed, "block 3, channel 1: step index 90"));
}

TEST(WavMediaTest, BadBlockHeadersFailWhereTheStructureLooksNoFurtherThanFmt) {
    // IMA: the step index of the second channel in the third block of a stereo file.
    const Bytes ima = imaWav(2, 22050, 6);
    const std::size_t data = payloadOf(ima, "data");
    const std::size_t blockAlign = loadLe16(ima, payloadOf(ima, "fmt ") + 12);
    const Bytes badStep = overwritten(ima, data + 2 * blockAlign + 4 + 2, {89});
    EXPECT_EQ(validated("wav", badStep).structural.status, LevelStatus::Passed);
    EXPECT_TRUE(mediaIs("wav", badStep, LevelStatus::Failed, "block 2, channel 2: step index 89"));
    EXPECT_EQ(mediaOf("wav", badStep).offset.value_or(0), data + 2 * blockAlign);
    EXPECT_EQ(validated("wav", badStep).status(), carving::ValidationStatus::Invalid);
    EXPECT_TRUE(mediaIs("wav", overwritten(ima, data + 2, {88}), LevelStatus::Passed)) << "88 is the last index";
    // The reserved byte after the step index.
    EXPECT_TRUE(mediaIs("wav", overwritten(ima, data + blockAlign + 3, {1}), LevelStatus::Failed,
                        "block 1, channel 1: step index"));
    EXPECT_TRUE(mediaIs("wav", overwritten(ima, data + blockAlign + 3, {1}), LevelStatus::Failed, "reserved byte 1"));

    // Microsoft ADPCM: a predictor equal to the number of coefficient pairs.
    const Bytes sox = audio::named("sox_ms_adpcm.wav").data();
    const std::size_t soxData = payloadOf(sox, "data");
    EXPECT_TRUE(mediaIs("wav", overwritten(sox, soxData + 256, {7}), LevelStatus::Failed,
                        "block 1, channel 1: predictor 7 of 7"));
    EXPECT_TRUE(mediaIs("wav", overwritten(sox, soxData + 256, {6}), LevelStatus::Passed));
    const Bytes extra = msAdpcmWav(2, 12, 3);
    EXPECT_TRUE(mediaIs("wav", overwritten(extra, payloadOf(extra, "data") + 4 * 512 + 1, {10}), LevelStatus::Failed,
                        "block 4, channel 2: predictor 10 of 10"));

    // fmt without the 7 standard coefficient pairs; blocks too small for their headers.
    const Bytes six = overwritten(sox, payloadOf(sox, "fmt ") + 20, {6, 0});
    EXPECT_EQ(validated("wav", six).structural.status, LevelStatus::Passed);
    EXPECT_TRUE(mediaIs("wav", six, LevelStatus::Failed, "fmt declares 6 coefficient pairs"));
    EXPECT_TRUE(mediaIs("wav", overwritten(ima, payloadOf(ima, "fmt ") + 12, {7, 0}), LevelStatus::Failed,
                        "blocks of 7 bytes for 2 channels"));
}

TEST(WavMediaTest, OtherCodingsAndMpegAudioAreNotDecoded) {
    const Bytes pcm = test::makeWav({});
    const Bytes mpeg = overwritten(pcm, payloadOf(pcm, "fmt "), {0x55, 0x00});
    const ValidationState state = validated("wav", mpeg);
    EXPECT_EQ(state.structural.status, LevelStatus::Passed) << testing::describe(state);
    EXPECT_TRUE(mediaIs("wav", mpeg, LevelStatus::Unsupported, "format 0x0055 is not decoded"));
    EXPECT_EQ(state.status(), carving::ValidationStatus::Valid);
    // Microsoft ADPCM wrapped in WAVE_FORMAT_EXTENSIBLE has no coefficient table.
    test::WavOptions options;
    options.extensible = true;
    const Bytes extensible = test::makeWav(options);
    EXPECT_TRUE(mediaIs("wav", overwritten(extensible, payloadOf(extensible, "fmt ") + 24, {0x02, 0x00}),
                        LevelStatus::Unsupported, "inside WAVE_FORMAT_EXTENSIBLE"));

    std::vector<Bytes> mp3s = {test::makeMp3({})};
    for (const audio::Sample& sample : audio::all()) {
        if (sample.format == "mp3") {
            mp3s.push_back(sample.data());
        }
    }
    for (const Bytes& file : mp3s) {
        const ValidationState mp3 = validated("mp3", file);
        EXPECT_EQ(mp3.structural.status, LevelStatus::Passed) << testing::describe(mp3);
        EXPECT_EQ(mp3.media.status, LevelStatus::Unsupported);
        EXPECT_NE(mp3.media.detail.find("not decoded"), std::string::npos) << mp3.media.detail;
        EXPECT_EQ(mp3.status(), carving::ValidationStatus::Valid);
    }
}

TEST(WavMediaTest, FilesCutShortAreTruncated) {
    for (const Bytes& file : {imaWav(1, 8000, 4), audio::named("sox_ms_adpcm.wav").data()}) {
        const std::size_t dataEnd = payloadOf(file, "data") + loadLe32(file, payloadOf(file, "data") - 4);
        for (std::size_t length = 0; length < dataEnd; ++length) {
            EXPECT_TRUE(mediaIs("wav", std::span(file).first(length), LevelStatus::Truncated)) << length;
        }
    }
}

TEST(WavMediaTest, DamagedFilesNeverBreakTheDecoder) {
    testing::fuzzMedia("wav", imaWav(2, 44100, 6), 400, 41);
    testing::fuzzMedia("wav", audio::named("sox_ms_adpcm.wav").data(), 400, 42);
    testing::fuzzMedia("wav", msAdpcmWav(2, 5, 2), 200, 43);
    testing::fuzzMedia("wav", test::makeWav({}), 100, 44);
    testing::fuzzMedia("mp3", test::makeMp3({}), 50, 45);
}

}  // namespace
}  // namespace recovery::validation
