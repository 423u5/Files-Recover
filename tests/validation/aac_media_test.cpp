// AAC media validation of ADTS streams: builder streams (channel streams of
// the zero codebook, read through ID_END), the files of faac, fdkaac and
// FFmpeg, fdkaac's CRC-protected files (mono, stereo, 5.1, silence, clicks,
// 8 kHz) whose CRCs must match and fail once damaged, raw data blocks
// written here bit by bit that break one rule each, truncation and fuzzing.

#include "validation/aac_media_vectors.hpp"
#include "validation/media_test_helpers.hpp"

#include "formats/format_test_helpers.hpp"
#include "recovery/byte_order.hpp"
#include "support/audio_builders.hpp"
#include "support/audio_samples.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::validation {
namespace {

using testing::BitWriter;
using testing::Bytes;
using testing::mediaIs;
using testing::mediaOf;
using testing::validated;
namespace vectors = test::aac_vectors;

constexpr std::uint32_t kLc = 1;    // ADTS profile: object type - 1
constexpr std::uint32_t kMain = 0;
constexpr std::uint32_t k44100 = 4;  // sampling_frequency_index

// An ADTS stream of `frames` frames, each holding `block` (no CRC).
Bytes adtsStream(const Bytes& block, std::uint32_t profile = kLc, std::uint32_t channels = 2, int frames = 6,
                 std::uint32_t samplingIndex = k44100) {
    Bytes out;
    for (int i = 0; i < frames; ++i) {
        BitWriter header;
        header.put(0xFFF, 12);
        header.put(0, 1);  // MPEG-4
        header.put(0, 2);  // layer
        header.put(1, 1);  // protection_absent
        header.put(profile, 2);
        header.put(samplingIndex, 4);
        header.put(0, 1);
        header.put(channels, 3);
        header.put(0, 4);
        header.put(static_cast<std::uint32_t>(7 + block.size()), 13);
        header.put(0x7FF, 11);
        header.put(0, 2);
        const Bytes bytes = header.bytes();
        out.insert(out.end(), bytes.begin(), bytes.end());
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
}

// A long-window ics_info: max_sfb, no prediction.
void longInfo(BitWriter& bits, std::uint32_t maxSfb) {
    bits.put(0, 1);  // ics_reserved_bit
    bits.put(0, 2);  // ONLY_LONG_SEQUENCE
    bits.put(0, 1);  // window_shape
    bits.put(maxSfb, 6);
    bits.put(0, 1);  // predictor_data_present
}

// One section of `length` long-window bands with `codebook`.
void longSection(BitWriter& bits, std::uint32_t codebook, std::uint32_t length) {
    bits.put(codebook, 4);
    for (; length >= 31; length -= 31) {
        bits.put(31, 5);
    }
    bits.put(length, 5);
}

// The end of a stream without coded bands: no pulse, TNS or gain control data.
void noTools(BitWriter& bits) {
    bits.put(0, 3);
}

// An individual channel stream of `maxSfb` bands of the zero codebook.
void silentStream(BitWriter& bits, std::uint32_t maxSfb, bool ownInfo = true) {
    bits.put(100, 8);  // global_gain
    if (ownInfo) {
        longInfo(bits, maxSfb);
    }
    if (maxSfb > 0) {
        longSection(bits, 0, maxSfb);
    }
    noTools(bits);
}

void end(BitWriter& bits) {
    bits.put(7, 3);
}

// A CPE without a common window, both streams silent; then ID_END.
Bytes silentCpe(std::uint32_t maxSfb = 10) {
    BitWriter bits;
    bits.put(1, 3);
    bits.put(0, 4);
    bits.put(0, 1);  // common_window
    silentStream(bits, maxSfb);
    silentStream(bits, maxSfb);
    end(bits);
    return bits.bytes();
}

// A block of one SCE written by `stream` (after element_instance_tag), then ID_END.
Bytes sce(const std::function<void(BitWriter&)>& stream) {
    BitWriter bits;
    bits.put(0, 3);
    bits.put(0, 4);
    stream(bits);
    end(bits);
    return bits.bytes();
}

std::size_t vectorFrames(const Bytes& file) {
    std::size_t frames = 0;
    for (std::size_t position = 0; position + 7 <= file.size(); ++frames) {
        position += ((loadU8(file, position + 3) & 3U) << 11) | (std::size_t{loadU8(file, position + 4)} << 3) |
                    (loadU8(file, position + 5) >> 5);
    }
    return frames;
}

// Offsets of the frames of an ADTS stream without tags.
std::vector<std::size_t> frameOffsets(const Bytes& file) {
    std::vector<std::size_t> offsets;
    for (std::size_t position = 0; position + 7 <= file.size();) {
        offsets.push_back(position);
        position += ((loadU8(file, position + 3) & 3U) << 11) | (std::size_t{loadU8(file, position + 4)} << 3) |
                    (loadU8(file, position + 5) >> 5);
    }
    return offsets;
}

Bytes flipped(Bytes file, std::size_t bit) {
    file[bit / 8] ^= static_cast<std::byte>(0x80U >> (bit % 8));
    return file;
}

TEST(AacMediaTest, BuilderStreamsAreReadThroughIdEnd) {
    for (const int channels : {1, 2}) {
        for (const int profile : {0, 1, 3}) {
            for (const std::uint32_t rate : {8000U, 22050U, 44100U, 48000U, 96000U}) {
                test::AacOptions options;
                options.channels = static_cast<std::uint8_t>(channels);
                options.profile = static_cast<std::uint8_t>(profile);
                options.sampleRate = rate;
                options.mpeg2 = rate == 22050;
                const Bytes file = test::makeAdts(options);
                EXPECT_TRUE(testing::passesEveryLevel("aac", file, Coverage::Partial))
                    << channels << " " << profile << " " << rate;
                EXPECT_TRUE(
                    mediaIs("aac", file, LevelStatus::Passed, "read through ID_END in 20, up to coded data in 0"));
            }
        }
    }
    test::AacOptions tagged;
    tagged.id3v2 = 3;
    tagged.id3v1 = true;
    EXPECT_TRUE(testing::passesEveryLevel("aac", test::makeAdts(tagged), Coverage::Partial));
}

TEST(AacMediaTest, StreamsOfFaacFdkaacAndFfmpegPass) {
    int checked = 0;
    for (const test::audio_samples::Sample& sample : test::audio_samples::all()) {
        if (sample.format == "aac") {
            EXPECT_TRUE(testing::passesEveryLevel("aac", sample.data(), Coverage::Partial)) << sample.name;
            ++checked;
        }
    }
    EXPECT_GE(checked, 3);
}

TEST(AacMediaTest, CrcsOfFdkaacFilesMatch) {
    for (const vectors::Vector& vector : vectors::all()) {
        const Bytes file = vector.data();
        const ValidationState state = validated("aac", file);
        EXPECT_EQ(state.structural.status, LevelStatus::Passed) << vector.name << testing::describe(state);
        EXPECT_EQ(state.media.status, LevelStatus::Passed) << vector.name << testing::describe(state);
        EXPECT_EQ(state.media.coverage, Coverage::Partial);
        const std::size_t frames = vectorFrames(file);
        if (vector.name == "fdk_crc_surround.aac") {
            // 5.1: the CRC regions of channel elements after the first one read in part are not found.
            EXPECT_NE(state.media.detail.find("not checked in"), std::string::npos) << state.media.detail;
        } else {
            EXPECT_NE(state.media.detail.find("CRC matches in " + std::to_string(frames)), std::string::npos)
                << vector.name << ": " << state.media.detail;
            EXPECT_EQ(state.media.detail.find("not checked"), std::string::npos) << state.media.detail;
        }
    }
    // Digital silence: channel streams of the zero codebook are read to their ends.
    EXPECT_TRUE(mediaIs("aac", vectors::named("fdk_crc_silence.aac").data(), LevelStatus::Passed,
                        "read through ID_END in 2"));
}

TEST(AacMediaTest, DamageInsideTheCrcRegionsFails) {
    // Mono and stereo: the CRC regions of every frame are found, also where the channel elements hold coded
    // data. (In 5.1 only frames read through ID_END are checked.) No damaged copy may pass by a chance match.
    for (const vectors::Vector& vector : vectors::all()) {
        if (vector.name == "fdk_crc_surround.aac") {
            continue;
        }
        const Bytes file = vector.data();
        const std::vector<std::size_t> frames = frameOffsets(file);
        for (std::size_t frame = 0; frame < frames.size(); ++frame) {
            const std::size_t first = frames[frame] * 8;
            const std::size_t next = frame + 1 < frames.size() ? frames[frame + 1] : file.size();
            // Bits of the stored CRC, and the element_instance_tag of the first channel element (no syntax rule
            // reads it): the CRC alone tells.
            for (const std::size_t bit : {first + 56, first + 63, first + 71, first + 72 + 3 + 1}) {
                const LevelResult media = mediaOf("aac", flipped(file, bit));
                EXPECT_EQ(media.status, LevelStatus::Failed)
                    << vector.name << " frame " << frame << " bit " << bit - first << ": " << media.detail;
                EXPECT_EQ(media.offset.value_or(0), frames[frame]) << vector.name << " " << media.detail;
                EXPECT_NE(media.detail.find("the CRC does not match"), std::string::npos) << media.detail;
            }
            // A bit 40 bits into the first channel element: the CRC or a syntax rule, inside that frame.
            const LevelResult media = mediaOf("aac", flipped(file, first + 72 + 3 + 40));
            EXPECT_EQ(media.status, LevelStatus::Failed) << vector.name << " frame " << frame << ": " << media.detail;
            EXPECT_GE(media.offset.value_or(0), frames[frame]) << vector.name << " " << media.detail;
            EXPECT_LT(media.offset.value_or(0), next) << vector.name << " " << media.detail;
        }
    }
}

TEST(AacMediaTest, RawDataBlocksWrittenBitByBit) {
    // The zero codebook in every band: read through ID_END, in mono and stereo, LC and Main.
    EXPECT_TRUE(testing::passesEveryLevel("aac", adtsStream(silentCpe(), kLc, 2), Coverage::Partial));
    EXPECT_TRUE(testing::passesEveryLevel("aac", adtsStream(sce([](BitWriter& b) { silentStream(b, 40); }), kLc, 1),
                                          Coverage::Partial));
    // Coded bands: read up to their scale factors.
    EXPECT_TRUE(mediaIs("aac", adtsStream(sce([](BitWriter& b) {
                                              b.put(100, 8);
                                              longInfo(b, 3);
                                              longSection(b, 5, 3);
                                              b.put(0x5A5A, 16);
                                          }),
                                          kLc, 1),
                        LevelStatus::Passed, "read through ID_END in 0, up to coded data in 6"));

    const auto fails = [](const Bytes& block, std::string_view detail, std::uint32_t profile = kLc,
                          std::uint32_t channels = 1) {
        const Bytes stream = adtsStream(block, profile, channels);
        const ValidationState state = validated("aac", stream);
        EXPECT_EQ(state.structural.status, LevelStatus::Passed) << testing::describe(state);
        return mediaIs("aac", stream, LevelStatus::Failed, detail);
    };
    EXPECT_TRUE(fails(sce([](BitWriter& b) {
                          b.put(100, 8);
                          b.put(1, 1);  // ics_reserved_bit
                          b.put(0, 10);
                      }),
                      "ics_reserved_bit is set"));
    EXPECT_TRUE(fails(sce([](BitWriter& b) { silentStream(b, 50); }), "max_sfb 50 beyond the 49 bands"));
    EXPECT_TRUE(fails(sce([](BitWriter& b) {
                          b.put(100, 8);
                          longInfo(b, 4);
                          longSection(b, 12, 4);
                      }),
                      "reserved codebook 12"));
    EXPECT_TRUE(fails(sce([](BitWriter& b) {
                          b.put(100, 8);
                          longInfo(b, 4);
                          longSection(b, 0, 3);
                          longSection(b, 0, 2);
                      }),
                      "sections run to band 5, beyond max_sfb 4"));
    // Prediction: AAC Main only, with a reset group of 1 to 30.
    const auto predicted = [](std::uint32_t resetGroup) {
        return sce([resetGroup](BitWriter& b) {
            b.put(100, 8);
            b.put(0, 4);    // reserved bit, ONLY_LONG, window shape
            b.put(2, 6);    // max_sfb
            b.put(1, 1);    // predictor_data_present
            b.put(1, 1);    // predictor_reset
            b.put(resetGroup, 5);
            b.put(3, 2);    // prediction_used for the 2 bands
            longSection(b, 0, 2);
            noTools(b);
        });
    };
    EXPECT_TRUE(fails(predicted(5), "prediction data in AAC LC"));
    EXPECT_TRUE(mediaIs("aac", adtsStream(predicted(5), kMain, 1), LevelStatus::Passed, "read through ID_END in 6"));
    EXPECT_TRUE(fails(predicted(31), "predictor reset group 31", kMain));
    // CPE: ms_mask_present 3 is reserved.
    {
        BitWriter b;
        b.put(1, 3);
        b.put(0, 4);
        b.put(1, 1);  // common_window
        longInfo(b, 2);
        b.put(3, 2);  // ms_mask_present
        b.put(0, 30);
        EXPECT_TRUE(fails(b.bytes(), "ms_mask_present is the reserved 3", kLc, 2));
    }
    // Pulse data needs long windows; TNS orders within the profile's limit; no gain control in AAC LC.
    EXPECT_TRUE(fails(sce([](BitWriter& b) {
                          b.put(100, 8);
                          b.put(0, 1);
                          b.put(2, 2);  // EIGHT_SHORT_SEQUENCE
                          b.put(0, 1);
                          b.put(0, 4);  // max_sfb
                          b.put(0x7F, 7);
                          b.put(1, 1);  // pulse_data_present
                          b.put(0, 20);
                      }),
                      "pulse data in a stream of short windows"));
    const auto tns = [](std::uint32_t order) {
        return sce([order](BitWriter& b) {
            b.put(100, 8);
            longInfo(b, 0);
            b.put(0, 1);      // pulse
            b.put(1, 1);      // tns_data_present
            b.put(1, 2);      // n_filt
            b.put(0, 1);      // coef_res
            b.put(20, 6);     // length
            b.put(order, 5);  // order
            b.put(0, 2);      // direction, coef_compress
            b.put(0, 3 * order);
            b.put(0, 1);  // gain control
        });
    };
    EXPECT_TRUE(mediaIs("aac", adtsStream(tns(12), kLc, 1), LevelStatus::Passed, "read through ID_END in 6"));
    EXPECT_TRUE(fails(tns(13), "TNS filter order 13 above 12"));
    EXPECT_TRUE(mediaIs("aac", adtsStream(tns(20), kMain, 1), LevelStatus::Passed));
    EXPECT_TRUE(fails(sce([](BitWriter& b) {
                          b.put(100, 8);
                          longInfo(b, 0);
                          b.put(1, 3);  // gain_control_data_present
                      }),
                      "gain control data in AAC LC"));
    // Channel elements beyond the configuration.
    {
        BitWriter b;
        b.put(3, 3);  // ID_LFE
        b.put(0, 4);
        silentStream(b, 0);
        end(b);
        EXPECT_TRUE(fails(b.bytes(), "an LFE element in channel configuration 2", kLc, 2));
    }
    {
        Bytes two = silentCpe(2);
        BitWriter b;
        b.put(1, 3);
        b.put(0, 4);
        b.put(0, 1);
        silentStream(b, 2);
        silentStream(b, 2);
        b.put(1, 3);
        b.put(1, 4);
        b.put(0, 1);
        silentStream(b, 2);
        silentStream(b, 2);
        end(b);
        EXPECT_TRUE(fails(b.bytes(), "channel elements for more than the 2 channels", kLc, 2));
        // A lone CPE in a mono configuration plays as stereo in FFmpeg.
        EXPECT_TRUE(mediaIs("aac", adtsStream(two, kLc, 1), LevelStatus::Passed));
    }
    // ID_END must end the frame; the elements may not run past it.
    {
        Bytes padded = silentCpe();
        padded.resize(padded.size() + 3);
        EXPECT_TRUE(fails(padded, "ID_END ends the raw data block 3 bytes before the end of the frame", kLc, 2));
        BitWriter b;
        b.put(1, 3);
        b.put(0, 4);
        b.put(0, 1);
        silentStream(b, 10);
        EXPECT_TRUE(fails(b.bytes(), "the elements run past the end of the block", kLc, 2));
    }
    // Fill, data stream and program config elements are read whole; a coupling channel element stops the reading.
    {
        BitWriter b;
        b.put(6, 3);  // FIL of 2 bytes
        b.put(2, 4);
        b.put(0x1A5, 16);
        b.put(4, 3);  // DSE, byte aligned, 3 bytes
        b.put(0, 4);
        b.put(1, 1);
        b.put(3, 8);
        while (b.size() % 8 != 0) {
            b.put(0, 1);
        }
        b.put(0xABCDEF, 24);
        b.put(5, 3);  // PCE: one front CPE, no comment
        b.put(0, 4);
        b.put(1, 2);
        b.put(k44100, 4);
        b.put(1, 4);
        b.put(0, 4 + 4 + 2 + 3 + 4);
        b.put(0, 3);  // no mixdowns
        b.put(1, 1);  // front element: CPE, tag 0
        b.put(0, 4);
        while (b.size() % 8 != 0) {
            b.put(0, 1);
        }
        b.put(0, 8);
        b.put(1, 3);  // the CPE
        b.put(0, 4);
        b.put(0, 1);
        silentStream(b, 1);
        silentStream(b, 1);
        end(b);
        EXPECT_TRUE(mediaIs("aac", adtsStream(b.bytes(), kLc, 2), LevelStatus::Passed, "read through ID_END in 6"));
        BitWriter c;
        c.put(2, 3);  // CCE
        c.put(0, 40);
        EXPECT_TRUE(mediaIs("aac", adtsStream(c.bytes(), kLc, 2), LevelStatus::Passed, "up to coded data in 6"));
    }
}

TEST(AacMediaTest, ShortWindowsAndTheirGroups) {
    const auto shortBlock = [](std::uint32_t maxSfb, std::uint32_t grouping, std::uint32_t sectionsPerGroup) {
        return sce([=](BitWriter& b) {
            b.put(100, 8);
            b.put(0, 1);
            b.put(2, 2);  // EIGHT_SHORT_SEQUENCE
            b.put(0, 1);
            b.put(maxSfb, 4);
            b.put(grouping, 7);
            std::uint32_t groups = 1;
            for (int bit = 6; bit >= 0; --bit) {
                groups += ((grouping >> bit) & 1U) == 0 ? 1U : 0U;
            }
            for (std::uint32_t g = 0; g < groups; ++g) {
                std::uint32_t left = maxSfb;
                for (std::uint32_t s = 0; s < sectionsPerGroup && left > 0; ++s) {
                    const std::uint32_t length = s + 1 == sectionsPerGroup ? left : 1;
                    b.put(0, 4);
                    std::uint32_t rest = length;
                    for (; rest >= 7; rest -= 7) {
                        b.put(7, 3);
                    }
                    b.put(rest, 3);
                    left -= length;
                }
            }
            noTools(b);
        });
    };
    // 14 bands of short windows at 44.1 kHz; groups of 1 to 8 windows.
    EXPECT_TRUE(mediaIs("aac", adtsStream(shortBlock(14, 0x7F, 2), kLc, 1), LevelStatus::Passed,
                        "read through ID_END in 6"));
    EXPECT_TRUE(mediaIs("aac", adtsStream(shortBlock(9, 0x00, 3), kLc, 1), LevelStatus::Passed,
                        "read through ID_END in 6"));
    EXPECT_TRUE(mediaIs("aac", adtsStream(shortBlock(15, 0x55, 1), kLc, 1), LevelStatus::Failed,
                        "max_sfb 15 beyond the 14 bands of a short window"));
    // 8 kHz has 15 short-window bands.
    EXPECT_TRUE(mediaIs("aac", adtsStream(shortBlock(15, 0x55, 1), kLc, 1, 6, 11), LevelStatus::Passed));
}

TEST(AacMediaTest, StreamsCutShortAreTruncated) {
    const Bytes file = test::makeAdts({});
    const std::vector<std::size_t> frames = frameOffsets(file);
    for (std::size_t length = frames[4] + 1; length < file.size(); length += 7) {
        const LevelResult media = mediaOf("aac", std::span(file).first(length));
        EXPECT_TRUE(media.status == LevelStatus::Truncated || media.status == LevelStatus::Passed) << length;
    }
    EXPECT_TRUE(mediaIs("aac", std::span(file).first(frames[10] + 20), LevelStatus::Truncated,
                        "the data ends inside frame 11"));
}

TEST(AacMediaTest, DamagedFilesNeverBreakTheDecoder) {
    testing::fuzzMedia("aac", test::makeAdts({}), 300, 51);
    testing::fuzzMedia("aac", vectors::named("fdk_crc_mono.aac").data(), 300, 52);
    testing::fuzzMedia("aac", vectors::named("fdk_crc_stereo.aac").data(), 300, 53);
    testing::fuzzMedia("aac", vectors::named("fdk_crc_clicks.aac").data(), 200, 54);
    testing::fuzzMedia("aac", vectors::named("fdk_crc_surround.aac").data(), 100, 55);
}

}  // namespace
}  // namespace recovery::validation
