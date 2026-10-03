// Media validation of MP4 and M4A files: the samples of every audio and
// video track. Files of FFmpeg, GPAC and Media Foundation, AVC and HEVC
// vectors of libx264 and libx265 (CABAC, B pictures, weighted prediction,
// interlacing, slices, 4:2:2, 4:4:4, 10 bits, lossless coding, wavefront
// entry points, open GOPs, temporal sub-layers, in-band parameter sets),
// the builders' files, parameter sets and slice headers written here bit by
// bit that break one rule each, damage inside samples that the structure
// cannot see (zero-filled runs, flipped header bits), truncation and fuzzing.

#include "validation/media_test_helpers.hpp"
#include "validation/video_media_vectors.hpp"

#include "carving/content_reader.hpp"
#include "formats/mp4_parser.hpp"
#include "recovery/byte_order.hpp"
#include "support/audio_builders.hpp"
#include "support/audio_samples.hpp"
#include "support/mp4_builders.hpp"
#include "support/mp4_samples.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recovery::validation {
namespace {

using testing::BitWriter;
using testing::Bytes;
using testing::mediaIs;
using testing::mediaOf;
using testing::validated;
namespace mp4 = formats::mp4;
namespace samples = test::mp4_samples;
namespace vectors = test::video_vectors;

void append(Bytes& out, const Bytes& more) {
    out.insert(out.end(), more.begin(), more.end());
}

void putBe(Bytes& out, std::uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
    }
}

// A NAL unit: its header bytes, then `rbsp` with emulation prevention bytes inserted.
Bytes nalUnit(std::initializer_list<int> header, const Bytes& rbsp) {
    Bytes out;
    for (const int value : header) {
        out.push_back(static_cast<std::byte>(value));
    }
    int zeros = 0;
    for (const std::byte value : rbsp) {
        if (zeros >= 2 && std::to_integer<int>(value) <= 3) {
            out.push_back(std::byte{3});
            zeros = 0;
        }
        out.push_back(value);
        zeros = value == std::byte{0} ? zeros + 1 : 0;
    }
    return out;
}

// A sample: NAL units, each after a four-byte length.
Bytes sample(std::initializer_list<Bytes> units) {
    Bytes out;
    for (const Bytes& unit : units) {
        putBe(out, unit.size(), 4);
        append(out, unit);
    }
    return out;
}

Bytes videoFile(bool hevc, const Bytes& config, std::vector<Bytes> data) {
    test::Mp4TrackOptions track;
    track.kind = test::Mp4TrackKind::Video;
    track.hevc = hevc;
    track.samplesPerChunk = 2;
    track.sampleData = std::move(data);
    track.codecConfig = config;
    test::Mp4Options options;
    options.tracks = {track};
    return test::makeMp4(options).bytes;
}

// ---------------------------------------------------------------------------
// AVC written field by field: a Baseline (or CABAC Main) stream of 2x2
// macroblocks with POC type 2.
// ---------------------------------------------------------------------------

struct AvcSps {
    std::uint32_t profile = 66;
    std::uint32_t id = 0;
    std::uint32_t frameNumMinus4 = 0;
    std::uint32_t pocType = 2;
    std::uint32_t refFrames = 1;
    std::uint32_t widthMinus1 = 1;
    std::uint32_t heightMinus1 = 1;
};

Bytes avcSps(const AvcSps& sps = {}) {
    BitWriter b;
    b.put(sps.profile, 8);
    b.put(0xC0, 8);  // constraint_set0_flag, constraint_set1_flag
    b.put(10, 8);    // level_idc
    b.ue(sps.id);
    b.ue(sps.frameNumMinus4);
    b.ue(sps.pocType);
    if (sps.pocType == 0) {
        b.ue(0);
    }
    b.ue(sps.refFrames);
    b.flag(false);  // gaps_in_frame_num_value_allowed_flag
    b.ue(sps.widthMinus1);
    b.ue(sps.heightMinus1);
    b.flag(true);   // frame_mbs_only_flag
    b.flag(true);   // direct_8x8_inference_flag
    b.flag(false);  // frame_cropping_flag
    b.flag(false);  // vui_parameters_present_flag
    b.trailingBits();
    return nalUnit({0x67}, b.bytes());
}

struct AvcPps {
    std::uint32_t id = 0;
    std::uint32_t spsId = 0;
    bool cabac = false;
    bool weighted = false;
    std::int32_t chromaOffset = 0;
};

Bytes avcPps(const AvcPps& pps = {}) {
    BitWriter b;
    b.ue(pps.id);
    b.ue(pps.spsId);
    b.flag(pps.cabac);
    b.flag(false);  // bottom_field_pic_order_in_frame_present_flag
    b.ue(0);        // num_slice_groups_minus1
    b.ue(0);        // num_ref_idx_l0_default_active_minus1
    b.ue(0);        // num_ref_idx_l1_default_active_minus1
    b.flag(pps.weighted);
    b.put(0, 2);  // weighted_bipred_idc
    b.se(0);      // pic_init_qp_minus26
    b.se(0);      // pic_init_qs_minus26
    b.se(pps.chromaOffset);
    b.flag(true);   // deblocking_filter_control_present_flag
    b.flag(false);  // constrained_intra_pred_flag
    b.flag(false);  // redundant_pic_cnt_present_flag
    b.trailingBits();
    return nalUnit({0x68}, b.bytes());
}

struct AvcSlice {
    bool idr = true;
    std::uint32_t refIdc = 3;
    std::uint32_t firstMb = 0;
    std::uint32_t sliceType = 7;
    std::uint32_t ppsId = 0;
    std::uint32_t frameNum = 0;
    std::optional<std::uint32_t> refsOverride;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> modifications;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> operations;
    std::int32_t lumaWeight = 0;
    std::uint32_t cabacInit = 0;
    std::int32_t qpDelta = 0;
    std::uint32_t deblocking = 0;
    std::int32_t alpha = 0;
    bool alignmentOnes = true;
};

AvcSlice pSlice(std::uint32_t frameNum) {
    AvcSlice slice;
    slice.idr = false;
    slice.refIdc = 2;
    slice.sliceType = 5;
    slice.frameNum = frameNum;
    return slice;
}

Bytes avcSlice(const AvcSlice& slice, const AvcPps& pps = {}) {
    BitWriter b;
    b.ue(slice.firstMb);
    b.ue(slice.sliceType);
    b.ue(slice.ppsId);
    b.put(slice.frameNum, 4);
    if (slice.idr) {
        b.ue(0);  // idr_pic_id
    }
    const std::uint32_t kind = slice.sliceType % 5;
    const bool predicted = kind == 0 || kind == 3;
    if (predicted) {
        b.flag(slice.refsOverride.has_value());
        if (slice.refsOverride.has_value()) {
            b.ue(*slice.refsOverride);
        }
        b.flag(!slice.modifications.empty());
        for (const auto& [operation, argument] : slice.modifications) {
            b.ue(operation);
            b.ue(argument);
        }
        if (!slice.modifications.empty()) {
            b.ue(3);
        }
        if (pps.weighted) {
            b.ue(0);  // luma_log2_weight_denom
            b.ue(0);  // chroma_log2_weight_denom
            for (std::uint32_t i = 0; i <= slice.refsOverride.value_or(0); ++i) {
                b.flag(true);
                b.se(slice.lumaWeight);
                b.se(0);
                b.flag(false);
            }
        }
    }
    if (slice.refIdc != 0) {
        if (slice.idr) {
            b.put(0, 2);
        } else {
            b.flag(!slice.operations.empty());
            for (const auto& [operation, argument] : slice.operations) {
                b.ue(operation);
                if (operation == 1 || operation == 3) {
                    b.ue(0);
                }
                if (operation == 2 || operation == 3 || operation == 4 || operation == 6) {
                    b.ue(argument);
                }
            }
            if (!slice.operations.empty()) {
                b.ue(0);
            }
        }
    }
    if (pps.cabac && kind != 2 && kind != 4) {
        b.ue(slice.cabacInit);
    }
    b.se(slice.qpDelta);
    b.ue(slice.deblocking);
    if (slice.deblocking != 1) {
        b.se(slice.alpha);
        b.se(0);
    }
    if (pps.cabac) {
        if (b.size() % 8 == 0) {
            ADD_FAILURE() << "the slice header ends on a byte boundary: no cabac_alignment_one_bit to write";
        }
        while (b.size() % 8 != 0) {
            b.flag(slice.alignmentOnes);
        }
    }
    b.put(0xA55AC3, 24);  // slice data
    b.trailingBits();
    return nalUnit({static_cast<int>((slice.refIdc << 5) | (slice.idr ? 5U : 1U))}, b.bytes());
}

Bytes avcC(const std::vector<Bytes>& sps, const std::vector<Bytes>& pps, int version = 1, int lengthSizeMinusOne = 3) {
    Bytes out = {static_cast<std::byte>(version), std::byte{66}, std::byte{0xC0}, std::byte{10},
                 static_cast<std::byte>(0xFC | lengthSizeMinusOne), static_cast<std::byte>(0xE0 | sps.size())};
    for (const Bytes& unit : sps) {
        putBe(out, unit.size(), 2);
        append(out, unit);
    }
    out.push_back(static_cast<std::byte>(pps.size()));
    for (const Bytes& unit : pps) {
        putBe(out, unit.size(), 2);
        append(out, unit);
    }
    return out;
}

// An AVC file: the configuration's SPS and PPS, an IDR picture, then P pictures.
Bytes avcFile(const AvcPps& pps, std::vector<AvcSlice> slices, const AvcSps& sps = {}) {
    std::vector<Bytes> data;
    for (const AvcSlice& slice : slices) {
        data.push_back(sample({avcSlice(slice, pps)}));
    }
    return videoFile(false, avcC({avcSps(sps)}, {avcPps(pps)}), std::move(data));
}

::testing::AssertionResult avcFails(const AvcSlice& slice, std::string_view detail, const AvcPps& pps = {}) {
    return mediaIs("mp4", avcFile(pps, {AvcSlice{}, slice}), LevelStatus::Failed, detail);
}

// ---------------------------------------------------------------------------
// HEVC written field by field: Main, 8x8 to 32x32 coding blocks, one
// short-term reference picture set in the SPS (one picture back, used).
// ---------------------------------------------------------------------------

void profileTierLevel(BitWriter& b) {
    b.put(0, 2);           // general_profile_space
    b.flag(false);         // general_tier_flag
    b.put(1, 5);           // general_profile_idc: Main
    b.put(0x60000000, 32);  // general_profile_compatibility_flag[1], [2]
    b.put(0x9, 4);          // progressive, interlaced, non-packed, frame-only
    b.put(0, 32);
    b.put(0, 11);  // the 43 constraint bits
    b.flag(false);  // general_inbld_flag
    b.put(30, 8);   // general_level_idc
}

struct HevcVps {
    std::uint32_t baseFlags = 3;
    std::uint32_t reserved = 0xFFFF;
};

Bytes hevcVps(const HevcVps& vps = {}) {
    BitWriter b;
    b.put(0, 4);  // vps_video_parameter_set_id
    b.put(vps.baseFlags, 2);
    b.put(0, 6);  // vps_max_layers_minus1
    b.put(0, 3);  // vps_max_sub_layers_minus1
    b.flag(true);  // vps_temporal_id_nesting_flag
    b.put(vps.reserved, 16);
    profileTierLevel(b);
    b.flag(true);  // vps_sub_layer_ordering_info_present_flag
    b.ue(1);
    b.ue(0);
    b.ue(0);
    b.put(0, 6);    // vps_max_layer_id
    b.ue(0);        // vps_num_layer_sets_minus1
    b.flag(false);  // vps_timing_info_present_flag
    b.flag(false);  // vps_extension_flag
    b.trailingBits();
    return nalUnit({0x40, 0x01}, b.bytes());
}

struct HevcSps {
    std::uint32_t vpsId = 0;
    std::uint32_t subLayersMinus1 = 0;
    std::uint32_t width = 64;
    std::uint32_t height = 64;
    std::uint32_t diffCb = 2;  // 32x32 coding tree blocks
    bool used = true;          // the SPS's reference picture set is used
};

Bytes hevcSps(const HevcSps& sps = {}) {
    BitWriter b;
    b.put(sps.vpsId, 4);
    b.put(sps.subLayersMinus1, 3);
    b.flag(true);  // sps_temporal_id_nesting_flag
    profileTierLevel(b);
    b.ue(0);  // sps_seq_parameter_set_id
    b.ue(1);  // chroma_format_idc
    b.ue(sps.width);
    b.ue(sps.height);
    b.flag(false);  // conformance_window_flag
    b.ue(0);
    b.ue(0);        // bit depths
    b.ue(4);        // log2_max_pic_order_cnt_lsb_minus4
    b.flag(true);   // sps_sub_layer_ordering_info_present_flag
    for (std::uint32_t i = 0; i <= std::min<std::uint32_t>(sps.subLayersMinus1, 6); ++i) {
        b.ue(1);
        b.ue(0);
        b.ue(0);
    }
    b.ue(0);  // log2_min_luma_coding_block_size_minus3
    b.ue(sps.diffCb);
    b.ue(0);        // log2_min_luma_transform_block_size_minus2
    b.ue(3);        // log2_diff_max_min_luma_transform_block_size
    b.ue(1);        // max_transform_hierarchy_depth_inter
    b.ue(1);        // max_transform_hierarchy_depth_intra
    b.flag(false);  // scaling_list_enabled_flag
    b.flag(false);  // amp_enabled_flag
    b.flag(true);   // sample_adaptive_offset_enabled_flag
    b.flag(false);  // pcm_enabled_flag
    b.ue(1);        // num_short_term_ref_pic_sets
    b.ue(1);        // num_negative_pics
    b.ue(0);        // num_positive_pics
    b.ue(0);        // delta_poc_s0_minus1
    b.flag(sps.used);
    b.flag(false);  // long_term_ref_pics_present_flag
    b.flag(true);   // sps_temporal_mvp_enabled_flag
    b.flag(false);  // strong_intra_smoothing_enabled_flag
    b.flag(false);  // vui_parameters_present_flag
    b.flag(false);  // sps_extension_present_flag
    b.trailingBits();
    return nalUnit({0x42, 0x01}, b.bytes());
}

struct HevcPps {
    std::uint32_t spsId = 0;
    bool dependentSlices = false;
    std::int32_t cbOffset = 0;
    bool tiles = false;
    std::uint32_t tileColumnsMinus1 = 0;
    bool wavefronts = false;
};

Bytes hevcPps(const HevcPps& pps = {}) {
    BitWriter b;
    b.ue(0);  // pps_pic_parameter_set_id
    b.ue(pps.spsId);
    b.flag(pps.dependentSlices);
    b.flag(false);  // output_flag_present_flag
    b.put(0, 3);    // num_extra_slice_header_bits
    b.flag(false);  // sign_data_hiding_enabled_flag
    b.flag(false);  // cabac_init_present_flag
    b.ue(0);
    b.ue(0);        // num_ref_idx_lX_default_active_minus1
    b.se(0);        // init_qp_minus26
    b.flag(false);  // constrained_intra_pred_flag
    b.flag(false);  // transform_skip_enabled_flag
    b.flag(false);  // cu_qp_delta_enabled_flag
    b.se(pps.cbOffset);
    b.se(0);        // pps_cr_qp_offset
    b.flag(false);  // pps_slice_chroma_qp_offsets_present_flag
    b.flag(false);  // weighted_pred_flag
    b.flag(false);  // weighted_bipred_flag
    b.flag(false);  // transquant_bypass_enabled_flag
    b.flag(pps.tiles);
    b.flag(pps.wavefronts);
    if (pps.tiles) {
        b.ue(pps.tileColumnsMinus1);
        b.ue(0);        // num_tile_rows_minus1
        b.flag(true);   // uniform_spacing_flag
        b.flag(false);  // loop_filter_across_tiles_enabled_flag
    }
    b.flag(false);  // pps_loop_filter_across_slices_enabled_flag
    b.flag(false);  // deblocking_filter_control_present_flag
    b.flag(false);  // pps_scaling_list_data_present_flag
    b.flag(false);  // lists_modification_present_flag
    b.ue(0);        // log2_parallel_merge_level_minus2
    b.flag(false);  // slice_segment_header_extension_present_flag
    b.flag(false);  // pps_extension_present_flag
    b.trailingBits();
    return nalUnit({0x44, 0x01}, b.bytes());
}

struct HevcSlice {
    std::uint32_t type = 19;  // IDR_W_RADL
    std::uint32_t temporalIdPlus1 = 1;
    bool first = true;
    bool dependent = false;
    std::uint32_t address = 0;
    std::uint32_t addressBits = 2;
    std::uint32_t ppsId = 0;
    std::uint32_t sliceType = 2;
    // P slices: the reference picture set of the slice header (num_negative_pics), or the SPS's.
    std::optional<std::uint32_t> negatives;
    std::optional<std::uint32_t> refsOverride;
    std::optional<std::uint32_t> collocated;
    std::uint32_t mergeCandidates = 0;
    std::int32_t qpDelta = 0;
    std::vector<std::uint32_t> entryPoints;
    bool alignmentOne = true;
};

HevcSlice trailing(bool first = true) {
    HevcSlice slice;
    slice.type = 1;  // TRAIL_R
    slice.sliceType = 1;
    slice.first = first;
    return slice;
}

Bytes hevcSlice(const HevcSlice& slice, const HevcPps& pps = {}) {
    BitWriter b;
    b.flag(slice.first);
    if (slice.type >= 16 && slice.type <= 23) {
        b.flag(false);  // no_output_of_prior_pics_flag
    }
    b.ue(slice.ppsId);
    if (!slice.first) {
        if (pps.dependentSlices) {
            b.flag(slice.dependent);
        }
        b.put(slice.address, slice.addressBits);
    }
    if (!slice.dependent) {
        b.ue(slice.sliceType);
        bool temporalMvp = false;
        if (slice.type != 19 && slice.type != 20) {
            b.put(1, 8);  // slice_pic_order_cnt_lsb
            b.flag(!slice.negatives.has_value());  // short_term_ref_pic_set_sps_flag
            if (slice.negatives.has_value()) {
                b.flag(false);  // inter_ref_pic_set_prediction_flag
                b.ue(*slice.negatives);
                b.ue(0);
                for (std::uint32_t i = 0; i < *slice.negatives; ++i) {
                    b.ue(0);
                    b.flag(true);
                }
            }
            temporalMvp = slice.collocated.has_value();
            b.flag(temporalMvp);  // slice_temporal_mvp_enabled_flag
        }
        b.flag(false);  // slice_sao_luma_flag
        b.flag(false);  // slice_sao_chroma_flag
        if (slice.sliceType != 2) {
            b.flag(slice.refsOverride.has_value());
            if (slice.refsOverride.has_value()) {
                b.ue(*slice.refsOverride);
            }
            if (temporalMvp && slice.refsOverride.value_or(0) > 0) {
                b.ue(*slice.collocated);
            }
            b.ue(slice.mergeCandidates);
        }
        b.se(slice.qpDelta);
    }
    if (pps.tiles || pps.wavefronts) {
        b.ue(static_cast<std::uint32_t>(slice.entryPoints.size()));
        if (!slice.entryPoints.empty()) {
            b.ue(15);  // offset_len_minus1
            for (const std::uint32_t entry : slice.entryPoints) {
                b.put(entry - 1, 16);
            }
        }
    }
    b.flag(slice.alignmentOne);
    while (b.size() % 8 != 0) {
        b.flag(false);
    }
    b.put(0xA55AC3, 24);  // slice segment data
    b.trailingBits();
    return nalUnit({static_cast<int>(slice.type << 1), static_cast<int>(slice.temporalIdPlus1)}, b.bytes());
}

Bytes hvcC(const std::vector<Bytes>& units, int version = 1) {
    Bytes out = {static_cast<std::byte>(version), std::byte{0x01}, std::byte{0x60}, std::byte{0}, std::byte{0},
                 std::byte{0}, std::byte{0x90}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                 std::byte{0}, std::byte{30}, std::byte{0xF0}, std::byte{0}, std::byte{0xFC}, std::byte{0xFD},
                 std::byte{0xF8}, std::byte{0xF8}, std::byte{0}, std::byte{0}, std::byte{0x0F},
                 static_cast<std::byte>(units.size())};
    for (const Bytes& unit : units) {
        out.push_back(static_cast<std::byte>(0x80 | ((std::to_integer<int>(unit[0]) >> 1) & 0x3F)));
        putBe(out, 1, 2);
        putBe(out, unit.size(), 2);
        append(out, unit);
    }
    return out;
}

Bytes hevcFile(std::vector<HevcSlice> slices, const HevcPps& pps = {}, const HevcSps& sps = {},
               const HevcVps& vps = {}) {
    std::vector<Bytes> data;
    for (const HevcSlice& slice : slices) {
        data.push_back(sample({hevcSlice(slice, pps)}));
    }
    return videoFile(true, hvcC({hevcVps(vps), hevcSps(sps), hevcPps(pps)}), std::move(data));
}

::testing::AssertionResult hevcFails(const HevcSlice& slice, std::string_view detail, const HevcPps& pps = {},
                                     const HevcSps& sps = {}) {
    return mediaIs("mp4", hevcFile({HevcSlice{}, slice}, pps, sps), LevelStatus::Failed, detail);
}

// The (offset, size) of every sample of the file's first track.
std::vector<std::pair<std::uint64_t, std::uint32_t>> firstTrackSamples(const Bytes& file) {
    carving::MemoryContentReader content(file);
    Result<mp4::Mp4File> parsed = mp4::parseFile(content);
    std::vector<std::pair<std::uint64_t, std::uint32_t>> out;
    if (!parsed.ok() || !parsed->movie.has_value() || parsed->movie->tracks.empty()) {
        ADD_FAILURE() << "no track";
        return out;
    }
    mp4::forEachSample(parsed->movie->tracks.front(), [&](std::uint32_t, std::uint64_t offset, std::uint32_t size) {
        out.emplace_back(offset, size);
    });
    return out;
}

// The (offset, size) of the NAL units of a sample with four-byte lengths.
std::vector<std::pair<std::uint64_t, std::uint64_t>> nalUnits(const Bytes& file, std::uint64_t offset,
                                                              std::uint32_t size) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    for (std::uint64_t position = offset; position + 4 <= offset + size;) {
        const std::uint64_t length = loadBe32(file, static_cast<std::size_t>(position));
        out.emplace_back(position + 4, length);
        position += 4 + length;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Files of independent writers
// ---------------------------------------------------------------------------

TEST(Mp4MediaTest, FilesOfFfmpegGpacAndMediaFoundationPass) {
    for (const samples::Sample& sample : samples::all()) {
        const Bytes data = sample.data();
        const bool video = std::any_of(sample.tracks.begin(), sample.tracks.end(),
                                       [](const samples::Track& track) { return track.kind == "video"; });
        const ValidationState state = validated(video ? "mp4" : "m4a", data);
        EXPECT_EQ(state.structural.status, LevelStatus::Passed) << sample.name << "\n" << testing::describe(state);
        if (sample.name == "ffmpeg_mpeg4.mp4") {
            EXPECT_EQ(state.media.status, LevelStatus::Unsupported) << testing::describe(state);
            EXPECT_NE(state.media.detail.find("MPEG-4 Part 2 video is not decoded"), std::string::npos);
            continue;
        }
        EXPECT_EQ(state.media.status, LevelStatus::Passed) << sample.name << "\n" << testing::describe(state);
        EXPECT_EQ(state.media.coverage, Coverage::Partial) << sample.name;
        EXPECT_EQ(state.status(), carving::ValidationStatus::Valid) << sample.name;
    }
    EXPECT_TRUE(mediaIs("mp4", samples::named("ffmpeg_hevc.mp4").data(), LevelStatus::Passed,
                        "track 1 (hvc1, HEVC Main, 64x48): 5 samples, 1 VPS, 1 SPS, 1 PPS, 5 slice segments"));
    EXPECT_TRUE(mediaIs("mp4", samples::named("mediafoundation_h264_aac.mp4").data(), LevelStatus::Passed,
                        "(avc1, AVC Main, 96x64): 5 samples, 1 SPS, 1 PPS, 5 slices (headers read; 5 CABAC)"));
    EXPECT_TRUE(mediaIs("mp4", samples::named("ffmpeg_subtitles.mp4").data(), LevelStatus::Passed,
                        "track 2 ('sbtl'): not audio or video"));
}

TEST(Mp4MediaTest, M4aFilesOfFaacFdkaacFfmpegAndMediaFoundationPass) {
    int checked = 0;
    for (const test::audio_samples::Sample& sample : test::audio_samples::all()) {
        if (sample.format != "m4a") {
            continue;
        }
        ++checked;
        const Bytes data = sample.data();
        if (sample.name == "ffmpeg_alac.m4a") {
            EXPECT_TRUE(mediaIs("m4a", data, LevelStatus::Unsupported, "'alac' sound is not decoded"));
            continue;
        }
        EXPECT_TRUE(testing::passesEveryLevel("m4a", data, Coverage::Partial)) << sample.name;
    }
    EXPECT_GE(checked, 8);
    EXPECT_TRUE(mediaIs("m4a", test::audio_samples::named("fdkaac.m4a").data(), LevelStatus::Passed,
                        "track 1 (mp4a, AAC LC, 22050 Hz, 1 channel): 8 samples, raw data blocks read through ID_END"));
}

TEST(Mp4MediaTest, AvcAndHevcOfLibx264AndLibx265Pass) {
    for (const vectors::Vector& vector : vectors::all()) {
        const Bytes data = vector.data();
        EXPECT_TRUE(testing::passesEveryLevel("mp4", data, Coverage::Partial)) << vector.name;
    }
    const auto passes = [](std::string_view name, std::string_view detail) {
        return mediaIs("mp4", vectors::named(name).data(), LevelStatus::Passed, detail);
    };
    EXPECT_TRUE(passes("avc_slices.mp4", "40 slices (headers read; 40 CABAC)"));
    EXPECT_TRUE(passes("avc_cavlc_baseline.mp4", "(avc1, AVC Baseline, 128x96): 10 samples"));
    EXPECT_TRUE(passes("avc_444_10bit.mp4", "AVC High 4:4:4 Predictive"));
    EXPECT_TRUE(passes("avc_422_10bit.mp4", "AVC High 4:2:2"));
    // Parameter sets inside the samples too.
    EXPECT_TRUE(passes("avc_avc3.mp4", "(avc3, AVC High, 128x96): 10 samples, 3 SPS, 3 PPS"));
    EXPECT_TRUE(passes("hevc_hev1.mp4", "3 VPS, 3 SPS, 3 PPS"));
    EXPECT_TRUE(passes("hevc_slices.mp4", "30 slice segments"));
    EXPECT_TRUE(passes("hevc_444.mp4", "HEVC Range Extensions"));
    EXPECT_TRUE(passes("hevc_main10.mp4", "HEVC Main 10"));
}

// ---------------------------------------------------------------------------
// Builder files
// ---------------------------------------------------------------------------

TEST(Mp4MediaTest, BuilderAudioPassesAndPatternVideoFails) {
    // The audio builders write AAC frames of silence: read through ID_END.
    EXPECT_TRUE(testing::passesEveryLevel("m4a", test::makeM4a({}), Coverage::Partial));
    test::Mp4Options audio;
    audio.tracks = {test::Mp4TrackOptions{test::Mp4TrackKind::Audio}, test::Mp4TrackOptions{test::Mp4TrackKind::Text}};
    audio.tracks[0].channels = 1;
    EXPECT_TRUE(mediaIs("m4a", test::makeMp4(audio).bytes, LevelStatus::Passed,
                        "raw data blocks read through ID_END in 12, up to coded data in 0"));
    // The MP4 builder's video samples are deterministic bytes framed as NAL
    // units: their structure is sound, their slices are not.
    for (const bool hevc : {false, true}) {
        test::Mp4Options options;
        options.tracks[0].hevc = hevc;
        const Bytes file = test::makeMp4(options).bytes;
        const ValidationState state = validated("mp4", file);
        EXPECT_EQ(state.structural.status, LevelStatus::Passed) << testing::describe(state);
        EXPECT_EQ(state.media.status, LevelStatus::Failed) << testing::describe(state);
        EXPECT_NE(state.media.detail.find(hevc ? "track 1 (hvc1, HEVC" : "track 1 (avc1, AVC"), std::string::npos)
            << state.media.detail;
        EXPECT_EQ(state.status(), carving::ValidationStatus::Invalid);
    }
}

// ---------------------------------------------------------------------------
// AVC rules
// ---------------------------------------------------------------------------

TEST(Mp4MediaTest, AvcWrittenBitByBitPasses) {
    EXPECT_TRUE(mediaIs("mp4", avcFile({}, {AvcSlice{}, pSlice(1), pSlice(2)}), LevelStatus::Passed,
                        "(avc1, AVC Baseline, 64x48): 3 samples, 1 SPS, 1 PPS, 3 slices (headers read)"));
    AvcPps cabac;
    cabac.cabac = true;
    EXPECT_TRUE(mediaIs("mp4", avcFile(cabac, {AvcSlice{}, pSlice(1), pSlice(2)}), LevelStatus::Passed,
                        "3 slices (headers read; 3 CABAC)"));
    // Reference list modification, weights and reference picture marking within their ranges.
    AvcSlice rich = pSlice(1);
    rich.refsOverride = 1;
    rich.modifications = {{0, 0}, {1, 2}};
    rich.operations = {{1, 0}, {3, 15}, {4, 16}, {2, 3}, {6, 2}, {5, 0}};
    rich.lumaWeight = -128;
    AvcPps weighted;
    weighted.weighted = true;
    EXPECT_TRUE(mediaIs("mp4", avcFile(weighted, {AvcSlice{}, rich}), LevelStatus::Passed, "2 slices"));
    // A slice that starts at the last macroblock.
    AvcSlice last;
    last.firstMb = 3;
    EXPECT_TRUE(mediaIs("mp4", avcFile({}, {last}), LevelStatus::Passed));
}

TEST(Mp4MediaTest, AvcSliceHeadersBreakingOneRuleFail) {
    AvcSlice slice = pSlice(1);
    slice.sliceType = 10;
    EXPECT_TRUE(avcFails(slice, "slice_type 10 (0 to 9)"));
    slice = pSlice(1);
    slice.ppsId = 1;
    EXPECT_TRUE(avcFails(slice, "refers to PPS 1, which has not been sent"));
    slice = pSlice(1);
    slice.firstMb = 4;
    EXPECT_TRUE(avcFails(slice, "first_mb_in_slice 4 beyond the picture's 4 macroblocks"));
    AvcSlice idr;
    idr.sliceType = 5;
    EXPECT_TRUE(avcFails(idr, "a slice of type P in an IDR picture"));
    slice = pSlice(1);
    slice.refsOverride = 16;
    EXPECT_TRUE(avcFails(slice, "num_ref_idx_l0_active_minus1 16 (0 to 15)"));
    slice = pSlice(1);
    slice.modifications = {{4, 0}};
    EXPECT_TRUE(avcFails(slice, "modification_of_pic_nums_idc 4 (0 to 3)"));
    slice = pSlice(1);
    slice.modifications = {{0, 0}, {0, 0}};
    EXPECT_TRUE(avcFails(slice, "more reference list modifications than the list's 1 entries"));
    slice = pSlice(1);
    slice.modifications = {{0, 16}};
    EXPECT_TRUE(avcFails(slice, "abs_diff_pic_num_minus1 16 (0 to 15)"));
    slice = pSlice(1);
    slice.operations = {{7, 0}};
    EXPECT_TRUE(avcFails(slice, "memory_management_control_operation 7 (0 to 6)"));
    slice = pSlice(1);
    slice.operations = {{6, 16}};
    EXPECT_TRUE(avcFails(slice, "the long-term index 16 of memory_management_control_operation 6"));
    slice = pSlice(1);
    slice.operations.assign(67, {1, 0});
    EXPECT_TRUE(avcFails(slice, "more than 66 memory management control operations"));
    slice = pSlice(1);
    slice.qpDelta = 26;
    EXPECT_TRUE(avcFails(slice, "the slice QP 52 (0 to 51)"));
    slice = pSlice(1);
    slice.deblocking = 3;
    EXPECT_TRUE(avcFails(slice, "disable_deblocking_filter_idc 3 (0 to 2)"));
    slice = pSlice(1);
    slice.alpha = 7;
    EXPECT_TRUE(avcFails(slice, "slice_alpha_c0_offset_div2 7 (-6 to 6)"));
    AvcPps weighted;
    weighted.weighted = true;
    slice = pSlice(1);
    slice.lumaWeight = 128;
    EXPECT_TRUE(avcFails(slice, "a luma weight or offset outside -128 to 127", weighted));
    AvcPps cabac;
    cabac.cabac = true;
    slice = pSlice(1);
    slice.cabacInit = 3;
    EXPECT_TRUE(avcFails(slice, "cabac_init_idc 3 (0 to 2)", cabac));
    slice = pSlice(1);
    slice.alignmentOnes = false;
    EXPECT_TRUE(avcFails(slice, "a cabac_alignment_one_bit of 0 after the slice header", cabac));
    // A slice header cut short by the end of its NAL unit.
    EXPECT_TRUE(mediaIs("mp4",
                        videoFile(false, avcC({avcSps()}, {avcPps()}),
                                  {sample({avcSlice(AvcSlice{})}), sample({nalUnit({0x41}, {std::byte{0x9A}})})}),
                        LevelStatus::Failed, "sample 2: NAL unit 1 (slice): the slice header ends inside its fields"));
}

TEST(Mp4MediaTest, AvcParameterSetsBreakingOneRuleFail) {
    const auto configFails = [](const Bytes& config, std::string_view detail) {
        return mediaIs("mp4", videoFile(false, config, {sample({avcSlice(AvcSlice{})})}), LevelStatus::Failed, detail);
    };
    AvcSps sps;
    sps.frameNumMinus4 = 13;
    EXPECT_TRUE(configFails(avcC({avcSps(sps)}, {avcPps()}),
                            "parameter set 1 of the avcC box (SPS): log2_max_frame_num_minus4 13 (0 to 12)"));
    sps = {};
    sps.pocType = 3;
    EXPECT_TRUE(configFails(avcC({avcSps(sps)}, {avcPps()}), "pic_order_cnt_type 3 (0 to 2)"));
    sps = {};
    sps.refFrames = 17;
    EXPECT_TRUE(configFails(avcC({avcSps(sps)}, {avcPps()}), "max_num_ref_frames 17 (0 to 16)"));
    sps = {};
    sps.id = 32;
    EXPECT_TRUE(configFails(avcC({avcSps(sps)}, {avcPps()}), "seq_parameter_set_id 32 (0 to 31)"));
    AvcPps pps;
    pps.spsId = 1;
    EXPECT_TRUE(configFails(avcC({avcSps()}, {avcPps(pps)}),
                            "parameter set 2 of the avcC box (PPS): it refers to SPS 1, which has not been sent"));
    pps = {};
    pps.chromaOffset = 13;
    EXPECT_TRUE(configFails(avcC({avcSps()}, {avcPps(pps)}), "chroma_qp_index_offset 13 (-12 to 12)"));
    // An SPS cut short inside its fields; NAL unit lengths of 3 bytes; no avcC at all.
    Bytes cut = avcSps();
    cut.resize(4);
    EXPECT_TRUE(configFails(avcC({cut}, {avcPps()}), "the SPS ends inside its fields"));
    EXPECT_TRUE(configFails(avcC({avcSps()}, {avcPps()}, 1, 2), "lengthSizeMinusOne 2"));
    Bytes noConfig = videoFile(false, avcC({avcSps()}, {avcPps()}), {sample({avcSlice(AvcSlice{})})});
    constexpr std::string_view kConfigBox = "avcC";
    const auto box = std::search(noConfig.begin(), noConfig.end(), kConfigBox.begin(), kConfigBox.end(),
                                 [](std::byte a, char b) { return std::to_integer<char>(a) == b; });
    ASSERT_NE(box, noConfig.end());
    *box = std::byte{'x'};
    EXPECT_TRUE(mediaIs("mp4", noConfig, LevelStatus::Failed, "the sample description holds no 'avcC' box"));
    // A configuration record of an unknown version is not read.
    EXPECT_TRUE(mediaIs("mp4", videoFile(false, avcC({avcSps()}, {avcPps()}, 2), {sample({avcSlice(AvcSlice{})})}),
                        LevelStatus::Unsupported, "avcC configurationVersion 2"));
}

TEST(Mp4MediaTest, AvcParameterSetsInsideSamples) {
    // Sent again unchanged: the PPS stays. Changed: the PPS that refers to it must follow.
    AvcSps changed;
    changed.refFrames = 2;
    const Bytes config = avcC({avcSps()}, {avcPps()});
    EXPECT_TRUE(mediaIs("mp4",
                        videoFile(false, config,
                                  {sample({avcSlice(AvcSlice{})}), sample({avcSps(), avcSlice(pSlice(1))})}),
                        LevelStatus::Passed, "2 SPS, 1 PPS, 2 slices"));
    EXPECT_TRUE(mediaIs("mp4",
                        videoFile(false, config,
                                  {sample({avcSlice(AvcSlice{})}), sample({avcSps(changed), avcSlice(pSlice(1))})}),
                        LevelStatus::Failed,
                        "sample 2: NAL unit 2 (slice): it refers to PPS 0, which has not been sent"));
    EXPECT_TRUE(mediaIs("mp4",
                        videoFile(false, config,
                                  {sample({avcSlice(AvcSlice{})}),
                                   sample({avcSps(changed), avcPps(), avcSlice(pSlice(1))})}),
                        LevelStatus::Passed, "2 SPS, 2 PPS, 2 slices"));
    // A configuration without parameter sets ('avc3'): they come with the first sample.
    EXPECT_TRUE(mediaIs("mp4", videoFile(false, avcC({}, {}), {sample({avcSps(), avcPps(), avcSlice(AvcSlice{})})}),
                        LevelStatus::Passed, "1 SPS, 1 PPS, 1 slice"));
    EXPECT_TRUE(mediaIs("mp4", videoFile(false, avcC({}, {}), {sample({avcSlice(AvcSlice{})})}), LevelStatus::Failed,
                        "refers to PPS 0, which has not been sent"));
}

TEST(Mp4MediaTest, ForbiddenByteSequencesInsideNalUnitsFail) {
    const Bytes slice = avcSlice(AvcSlice{});
    const auto with = [&](std::initializer_list<int> bytes, bool atEnd) {
        Bytes unit = slice;
        Bytes inserted;
        for (const int value : bytes) {
            inserted.push_back(static_cast<std::byte>(value));
        }
        unit.insert(atEnd ? unit.end() : unit.end() - 2, inserted.begin(), inserted.end());
        return videoFile(false, avcC({avcSps()}, {avcPps()}), {sample({unit})});
    };
    EXPECT_TRUE(mediaIs("mp4", with({0, 0, 1}, false), LevelStatus::Failed,
                        "NAL unit 1 (IDR slice): the byte sequence 00 00 01 inside the NAL unit"));
    EXPECT_TRUE(mediaIs("mp4", with({0, 0, 2}, false), LevelStatus::Failed, "the byte sequence 00 00 02"));
    EXPECT_TRUE(mediaIs("mp4", with({0, 0, 3, 7}, false), LevelStatus::Failed, "the byte sequence 00 00 03 07"));
    EXPECT_TRUE(mediaIs("mp4", with({0, 0, 0, 0, 0, 0x55}, false), LevelStatus::Failed,
                        "a run of 5 zero bytes inside the NAL unit"));
    // Zero bytes that end the unit are padding (FFmpeg drops them), and so
    // is a final emulation prevention byte (cabac_zero_words).
    EXPECT_TRUE(mediaIs("mp4", with({0, 0, 0, 0}, true), LevelStatus::Passed));
    EXPECT_TRUE(mediaIs("mp4", with({0, 0, 3}, true), LevelStatus::Passed));
    // The offset is where the sequence starts.
    const Bytes file = with({0, 0, 1}, false);
    const LevelResult media = mediaOf("mp4", file);
    const auto samplesOf = firstTrackSamples(file);
    ASSERT_EQ(samplesOf.size(), 1U);
    EXPECT_EQ(media.offset.value_or(0), samplesOf[0].first + samplesOf[0].second - 2 - 3);
}

// ---------------------------------------------------------------------------
// HEVC rules
// ---------------------------------------------------------------------------

TEST(Mp4MediaTest, HevcWrittenBitByBitPasses) {
    EXPECT_TRUE(mediaIs("mp4", hevcFile({HevcSlice{}, trailing(), trailing()}), LevelStatus::Passed,
                        "(hvc1, HEVC Main, 64x48): 3 samples, 1 VPS, 1 SPS, 1 PPS, 3 slice segments"));
    // A reference picture set of the slice header; two references with a collocated one.
    HevcSlice own = trailing();
    own.negatives = 2;
    own.refsOverride = 1;
    own.collocated = 1;
    EXPECT_TRUE(mediaIs("mp4", hevcFile({HevcSlice{}, own}), LevelStatus::Passed, "2 slice segments"));
    // A second slice segment, and a dependent one.
    HevcPps dependent;
    dependent.dependentSlices = true;
    HevcSlice second;
    second.first = false;
    second.address = 2;
    HevcSlice third = second;
    third.dependent = true;
    third.address = 3;
    EXPECT_TRUE(mediaIs("mp4", hevcFile({HevcSlice{}, second, third}, dependent), LevelStatus::Passed,
                        "3 slice segments"));
    // Wavefront entry points inside the slice segment data (two rows of coding tree blocks).
    HevcPps wavefronts;
    wavefronts.wavefronts = true;
    HevcSlice entries;
    entries.entryPoints = {2};
    EXPECT_TRUE(mediaIs("mp4", hevcFile({entries}, wavefronts), LevelStatus::Passed));
    // An hvcC of version 0 (early writers) is read too.
    EXPECT_TRUE(mediaIs("mp4",
                        videoFile(true, hvcC({hevcVps(), hevcSps(), hevcPps()}, 0), {sample({hevcSlice(HevcSlice{})})}),
                        LevelStatus::Passed));
}

TEST(Mp4MediaTest, HevcSliceSegmentHeadersBreakingOneRuleFail) {
    HevcSlice slice = trailing();
    slice.sliceType = 3;
    EXPECT_TRUE(hevcFails(slice, "slice_type 3 (0 to 2)"));
    HevcSlice idr;
    idr.sliceType = 1;
    EXPECT_TRUE(hevcFails(idr, "a P or B slice in an IRAP picture"));
    slice = trailing();
    slice.ppsId = 1;
    EXPECT_TRUE(hevcFails(slice, "refers to PPS 1, which has not been sent"));
    slice = trailing();
    slice.temporalIdPlus1 = 0;
    EXPECT_TRUE(hevcFails(slice, "nuh_temporal_id_plus1 is 0"));
    // Three coding tree blocks (96x32): slice_segment_address 3 lies beyond them.
    HevcSps wide;
    wide.width = 96;
    wide.height = 32;
    HevcSlice beyond = HevcSlice{};
    beyond.first = false;
    beyond.address = 3;
    EXPECT_TRUE(hevcFails(beyond, "slice_segment_address 3 (0 to 2)", {}, wide));
    HevcPps dependent;
    dependent.dependentSlices = true;
    HevcSlice atStart;
    atStart.first = false;
    atStart.dependent = true;
    atStart.address = 0;
    EXPECT_TRUE(hevcFails(atStart, "a dependent slice segment at the start of the picture", dependent));
    slice = trailing();
    slice.refsOverride = 15;
    EXPECT_TRUE(hevcFails(slice, "num_ref_idx_l0_active_minus1 15 (0 to 14)"));
    slice = trailing();
    slice.negatives = 16;
    EXPECT_TRUE(hevcFails(slice, "num_negative_pics 16 and num_positive_pics 0 (each 0 to 15)"));
    HevcSps unused;
    unused.used = false;
    EXPECT_TRUE(hevcFails(trailing(), "a P or B slice without reference pictures", {}, unused));
    slice = trailing();
    slice.negatives = 2;
    slice.refsOverride = 1;
    slice.collocated = 2;
    EXPECT_TRUE(hevcFails(slice, "collocated_ref_idx 2 (0 to 1)"));
    slice = trailing();
    slice.mergeCandidates = 5;
    EXPECT_TRUE(hevcFails(slice, "five_minus_max_num_merge_cand 5 (0 to 4)"));
    slice = trailing();
    slice.qpDelta = 26;
    EXPECT_TRUE(hevcFails(slice, "SliceQpY 52 (0 to 51)"));
    slice = trailing();
    slice.alignmentOne = false;
    EXPECT_TRUE(hevcFails(slice, "alignment_bit_equal_to_one is 0"));
    HevcPps wavefronts;
    wavefronts.wavefronts = true;
    HevcSlice entries;
    entries.entryPoints = {4};
    EXPECT_TRUE(hevcFails(entries, "entry points 4 bytes into slice segment data of 4 bytes", wavefronts));
    entries.entryPoints = {1, 1};
    EXPECT_TRUE(hevcFails(entries, "num_entry_point_offsets 2 (0 to 1)", wavefronts));
}

TEST(Mp4MediaTest, HevcParameterSetsBreakingOneRuleFail) {
    const auto configFails = [](const std::vector<Bytes>& units, std::string_view detail) {
        return mediaIs("mp4", videoFile(true, hvcC(units), {sample({hevcSlice(HevcSlice{})})}), LevelStatus::Failed,
                       detail);
    };
    HevcVps vps;
    vps.reserved = 0xFFFE;
    EXPECT_TRUE(configFails({hevcVps(vps), hevcSps(), hevcPps()},
                            "NAL unit 1 of the hvcC box (VPS): vps_reserved_0xffff_16bits is not 0xFFFF"));
    vps = {};
    vps.baseFlags = 1;
    EXPECT_TRUE(configFails({hevcVps(vps), hevcSps(), hevcPps()}, "vps_base_layer_internal_flag"));
    HevcSps sps;
    sps.vpsId = 1;
    EXPECT_TRUE(configFails({hevcVps(), hevcSps(sps), hevcPps()},
                            "NAL unit 2 of the hvcC box (SPS): it refers to VPS 1, which has not been sent"));
    sps = {};
    sps.subLayersMinus1 = 7;
    EXPECT_TRUE(configFails({hevcVps(), hevcSps(sps), hevcPps()}, "sps_max_sub_layers_minus1 7 (0 to 6)"));
    sps = {};
    sps.diffCb = 4;
    EXPECT_TRUE(configFails({hevcVps(), hevcSps(sps), hevcPps()}, "coding tree blocks of 2^7 samples (2^4 to 2^6)"));
    sps = {};
    sps.width = 100;
    EXPECT_TRUE(configFails({hevcVps(), hevcSps(sps), hevcPps()},
                            "a coded size of 100x64 that is not a multiple of the smallest coding block (8)"));
    HevcPps pps;
    pps.spsId = 1;
    EXPECT_TRUE(configFails({hevcVps(), hevcSps(), hevcPps(pps)}, "it refers to SPS 1, which has not been sent"));
    pps = {};
    pps.cbOffset = 13;
    EXPECT_TRUE(configFails({hevcVps(), hevcSps(), hevcPps(pps)}, "pps_cb_qp_offset 13 (-12 to 12)"));
    pps = {};
    pps.tiles = true;
    pps.tileColumnsMinus1 = 2;
    EXPECT_TRUE(configFails({hevcVps(), hevcSps(), hevcPps(pps)}, "num_tile_columns_minus1 2 (0 to 1)"));
    // A PPS before its SPS; a configuration record of an unknown version.
    EXPECT_TRUE(configFails({hevcVps(), hevcPps(), hevcSps()}, "it refers to SPS 0, which has not been sent"));
    EXPECT_TRUE(mediaIs("mp4",
                        videoFile(true, hvcC({hevcVps(), hevcSps(), hevcPps()}, 2), {sample({hevcSlice(HevcSlice{})})}),
                        LevelStatus::Unsupported, "hvcC configurationVersion 2"));
}

TEST(Mp4MediaTest, HevcParameterSetsInsideSamples) {
    // A changed SPS takes the PPS that refers to it along; sent again with
    // its VPS and PPS, the stream goes on.
    HevcSps changed;
    changed.width = 32;
    const Bytes config = hvcC({hevcVps(), hevcSps(), hevcPps()});
    EXPECT_TRUE(mediaIs("mp4",
                        videoFile(true, config,
                                  {sample({hevcSlice(HevcSlice{})}),
                                   sample({hevcSps(changed), hevcSlice(HevcSlice{})})}),
                        LevelStatus::Failed,
                        "sample 2: NAL unit 2 (IDR slice segment): it refers to PPS 0, which has not been sent"));
    EXPECT_TRUE(mediaIs("mp4",
                        videoFile(true, config,
                                  {sample({hevcSlice(HevcSlice{})}),
                                   sample({hevcVps(), hevcSps(changed), hevcPps(), hevcSlice(HevcSlice{})})}),
                        LevelStatus::Passed, "2 VPS, 2 SPS, 2 PPS, 2 slice segments"));
}

// ---------------------------------------------------------------------------
// Audio tracks
// ---------------------------------------------------------------------------

Bytes m4aWith(const Bytes& audioSpecificConfig, std::uint8_t channels = 2) {
    test::Mp4TrackOptions track;
    track.kind = test::Mp4TrackKind::Audio;
    track.channels = channels;
    track.codecConfig = audioSpecificConfig;
    test::Mp4Options options;
    options.majorBrand = "M4A ";
    options.compatibleBrands = {"M4A ", "isom"};
    options.tracks = {track};
    return test::makeMp4(options).bytes;
}

Bytes ascOf(std::uint32_t objectType, std::uint32_t samplingIndex, std::uint32_t channelConfig) {
    BitWriter b;
    b.put(objectType, 5);
    b.put(samplingIndex, 4);
    b.put(channelConfig, 4);
    b.put(0, 3);
    return b.bytes();
}

TEST(Mp4MediaTest, AacTracksFollowTheirAudioSpecificConfig) {
    EXPECT_TRUE(mediaIs("m4a", m4aWith(ascOf(2, 4, 2)), LevelStatus::Passed, "(mp4a, AAC LC, 44100 Hz, 2 channels)"));
    // HE-AAC signalled explicitly: the core is AAC LC at half the rate.
    BitWriter sbr;
    sbr.put(5, 5);
    sbr.put(7, 4);  // 22050 Hz core
    sbr.put(2, 4);
    sbr.put(4, 4);  // 44100 Hz extension
    sbr.put(2, 5);  // AAC LC core
    sbr.put(0, 3);
    EXPECT_TRUE(mediaIs("m4a", m4aWith(sbr.bytes()), LevelStatus::Passed, "HE-AAC (SBR), 22050 Hz"));
    // A mono configuration with stereo frames plays as stereo (FFmpeg); two
    // CPEs in it do not fit. Reserved channel configurations fail; object
    // types other than AAC Main, LC and LTP are not read.
    EXPECT_TRUE(mediaIs("m4a", m4aWith(ascOf(2, 4, 1)), LevelStatus::Passed));
    EXPECT_TRUE(mediaIs("m4a", m4aWith(ascOf(2, 4, 8)), LevelStatus::Failed, "the reserved channel configuration 8"));
    EXPECT_TRUE(mediaIs("m4a", m4aWith(ascOf(23, 4, 2)), LevelStatus::Unsupported, "audio object type 23 is not read"));
    // A stereo configuration whose frames are mono.
    EXPECT_TRUE(mediaIs("m4a", m4aWith(ascOf(2, 4, 2), 1), LevelStatus::Passed));
    // A damaged frame: ics_reserved_bit set in the first sample.
    Bytes file = test::makeM4a({});
    const auto first = firstTrackSamples(file);
    ASSERT_FALSE(first.empty());
    // ID_CPE (3 bits), tag (4), common_window (1), global_gain (8): then ics_reserved_bit.
    file[static_cast<std::size_t>(first[0].first) + 2] |= std::byte{0x80};
    EXPECT_TRUE(mediaIs("m4a", file, LevelStatus::Failed, "track 1 (mp4a, AAC LC, 44100 Hz, 2 channels), sample 1: "
                                                          "CPE: ics_reserved_bit is set"));
}

TEST(Mp4MediaTest, OtherCodecsAreNotDecoded) {
    const auto patched = [](std::string_view from, std::string_view to) {
        Bytes file = test::makeM4a({});
        const auto at = std::search(file.begin(), file.end(), from.begin(), from.end(),
                                    [](std::byte a, char b) { return std::to_integer<char>(a) == b; });
        EXPECT_NE(at, file.end());
        for (std::size_t i = 0; i < to.size() && at != file.end(); ++i) {
            *(at + static_cast<std::ptrdiff_t>(i)) = static_cast<std::byte>(to[i]);
        }
        return file;
    };
    EXPECT_TRUE(mediaIs("m4a", patched("mp4a", "sowt"), LevelStatus::NotApplicable, "PCM sound: nothing is coded"));
    EXPECT_TRUE(mediaIs("m4a", patched("mp4a", "ac-3"), LevelStatus::Unsupported, "'ac-3' sound is not decoded"));
    // Without an esds box: AAC LC of the sample description's channels and rate, as FFmpeg plays it.
    EXPECT_TRUE(mediaIs("m4a", patched("esds", "xsds"), LevelStatus::Passed,
                        "AAC LC without AudioSpecificConfig, 44100 Hz, 2 channels"));
}

// ---------------------------------------------------------------------------
// Damage the structure cannot see, truncation, fuzzing
// ---------------------------------------------------------------------------

TEST(Mp4MediaTest, ZeroFilledRunsInsideVideoSamplesFail) {
    for (const std::string_view name : {"avc_cabac_bframes.mp4", "hevc_wpp.mp4", "avc_cavlc_baseline.mp4"}) {
        Bytes file = vectors::named(name).data();
        const auto samplesOf = firstTrackSamples(file);
        ASSERT_GE(samplesOf.size(), 3U);
        // The largest NAL unit of the third sample: 16 bytes in its middle zeroed.
        const auto units = nalUnits(file, samplesOf[2].first, samplesOf[2].second);
        const auto largest = std::max_element(units.begin(), units.end(),
                                              [](const auto& a, const auto& b) { return a.second < b.second; });
        ASSERT_GT(largest->second, 64U) << name;
        const std::uint64_t at = largest->first + largest->second / 2;
        std::fill_n(file.begin() + static_cast<std::ptrdiff_t>(at), 16, std::byte{0});
        const ValidationState state = validated("mp4", file);
        EXPECT_EQ(state.structural.status, LevelStatus::Passed) << name << testing::describe(state);
        EXPECT_EQ(state.media.status, LevelStatus::Failed) << name << testing::describe(state);
        EXPECT_NE(state.media.detail.find("sample 3"), std::string::npos) << state.media.detail;
        EXPECT_NE(state.media.detail.find("zero bytes inside the NAL unit"), std::string::npos) << state.media.detail;
        // The run starts at `at`, or up to two bytes before it when data bytes there were zero already.
        EXPECT_LE(state.media.offset.value_or(0), at);
        EXPECT_GE(state.media.offset.value_or(0) + 2, at);
    }
}

TEST(Mp4MediaTest, FlippedSliceHeaderBitsAreOftenCaught) {
    // Every bit of the first four bytes after each slice's NAL unit header.
    // About half of them are fields any value of which is valid (frame_num,
    // the POC LSBs, SAO and output flags, a QP delta still in range): a flip
    // there turns one valid header into another and cannot be told without
    // decoding the picture. The rest fails (2026-10-03: 157 of 320 in the
    // AVC file, 151 of 320 in the HEVC one).
    for (const std::string_view name : {"avc_cabac_bframes.mp4", "hevc_wpp.mp4"}) {
        const Bytes file = vectors::named(name).data();
        const bool hevc = name.starts_with("hevc");
        int tried = 0;
        int caught = 0;
        for (const auto& [offset, size] : firstTrackSamples(file)) {
            for (const auto& [unit, length] : nalUnits(file, offset, size)) {
                const auto header = std::to_integer<std::uint32_t>(file[static_cast<std::size_t>(unit)]);
                const bool slice = hevc ? ((header >> 1) & 0x3F) <= 21 : (header & 0x1F) == 1 || (header & 0x1F) == 5;
                const std::uint64_t first = unit + (hevc ? 2 : 1);
                if (!slice || length < (hevc ? 6U : 5U)) {
                    continue;
                }
                for (std::uint64_t bit = 0; bit < 32; ++bit) {
                    Bytes damaged = file;
                    damaged[static_cast<std::size_t>(first + bit / 8)] ^= static_cast<std::byte>(0x80U >> (bit % 8));
                    ++tried;
                    caught += mediaOf("mp4", damaged).status == LevelStatus::Failed ? 1 : 0;
                }
            }
        }
        RecordProperty(std::string(name) + " caught", std::to_string(caught) + " of " + std::to_string(tried));
        std::printf("%s: %d of %d flipped slice header bits caught\n", std::string(name).c_str(), caught, tried);
        EXPECT_GE(caught * 5, tried * 2) << name;
    }
}

TEST(Mp4MediaTest, FilesCutShortAreTruncated) {
    // moov first: the samples after the cut are missing.
    const Bytes file = samples::named("ffmpeg_faststart.mp4").data();
    const auto samplesOf = firstTrackSamples(file);
    ASSERT_GE(samplesOf.size(), 4U);
    const std::span<const std::byte> cut = std::span(file).first(static_cast<std::size_t>(samplesOf[3].first + 10));
    const LevelResult media = mediaOf("mp4", cut);
    EXPECT_EQ(media.status, LevelStatus::Truncated) << testing::describe(media);
    EXPECT_NE(media.detail.find("the data ends inside sample 4 of 5"), std::string::npos) << media.detail;
    // moov last: no movie in the data.
    const Bytes last = samples::named("ffmpeg_h264_aac.mp4").data();
    EXPECT_TRUE(mediaIs("mp4", std::span(last).first(last.size() / 2), LevelStatus::Truncated,
                        "no movie (moov) to find the samples by"));
}

TEST(Mp4MediaTest, DamagedFilesNeverBreakTheDecoder) {
    testing::fuzzMedia("mp4", samples::named("ffmpeg_h264_aac.mp4").data(), 200, 61);
    testing::fuzzMedia("mp4", vectors::named("hevc_wpp.mp4").data(), 200, 62);
    testing::fuzzMedia("mp4", vectors::named("avc_weighted.mp4").data(), 150, 63);
    testing::fuzzMedia("mp4", vectors::named("hevc_hev1.mp4").data(), 150, 64);
    testing::fuzzMedia("m4a", test::audio_samples::named("mediafoundation_aac.m4a").data(), 150, 65);
    testing::fuzzMedia("mp4", hevcFile({HevcSlice{}, trailing(), trailing()}), 150, 66);
}

}  // namespace
}  // namespace recovery::validation
