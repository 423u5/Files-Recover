#include "hevc_syntax.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace recovery::validation::detail::hevc {

namespace {

using Problem = std::optional<std::string>;

// FFmpeg's bounds: HEVC_MAX_SUB_LAYERS, HEVC_MAX_DPB_SIZE, HEVC_MAX_SHORT_TERM_REF_PIC_SETS,
// HEVC_MAX_LONG_TERM_REF_PICS, HEVC_MAX_REFS, HEVC_MAX_PALETTE_PREDICTOR_SIZE.
constexpr std::uint32_t kMaxSubLayers = 7;
constexpr std::uint32_t kMaxDpbSize = 16;
constexpr std::uint32_t kMaxShortTermSets = 64;
constexpr std::uint32_t kMaxLongTermSps = 32;
constexpr std::uint32_t kMaxRefs = 16;
constexpr std::uint32_t kMaxPalettePredictor = 128;

// Slice types.
constexpr std::uint32_t kB = 0;
constexpr std::uint32_t kP = 1;
constexpr std::uint32_t kI = 2;

std::string range(std::string_view field, std::int64_t value, std::int64_t low, std::int64_t high) {
    return std::string(field) + " " + std::to_string(value) + " (" + std::to_string(low) + " to " +
           std::to_string(high) + ")";
}

std::string why(nal::Rbsp& rbsp, std::string_view what, std::string detail) {
    if (rbsp.overrun()) {
        return std::string(what) + " ends inside its fields";
    }
    if (rbsp.bits().malformed()) {
        return std::string(what) + " holds an Exp-Golomb code of more than 32 bits";
    }
    return detail;
}

bool isIrap(std::uint32_t type) noexcept {
    return type >= 16 && type <= 23;
}

bool isSlice(std::uint32_t type) noexcept {
    return type <= 9 || (type >= 16 && type <= 21);
}

// profile_tier_level(1, maxNumSubLayersMinus1) (7.3.3).
void profileTierLevel(MsbBits& bits, std::uint32_t maxSubLayersMinus1, std::uint32_t& profileIdc) {
    bits.skip(2 + 1);  // general_profile_space, general_tier_flag
    profileIdc = bits.read(5);
    const std::uint32_t compatibility = bits.read(32);
    for (unsigned i = 1; i < 32 && profileIdc == 0; ++i) {
        if (((compatibility >> (31 - i)) & 1U) != 0) {
            profileIdc = i;
        }
    }
    bits.skip(4 + 43 + 1);  // source and constraint flags, general_inbld_flag
    bits.skip(8);           // general_level_idc
    std::array<bool, 8> profilePresent{};
    std::array<bool, 8> levelPresent{};
    for (std::uint32_t i = 0; i < maxSubLayersMinus1; ++i) {
        profilePresent[i] = bits.flag();
        levelPresent[i] = bits.flag();
    }
    if (maxSubLayersMinus1 > 0) {
        bits.skip(std::uint64_t{2} * (8 - maxSubLayersMinus1));  // reserved_zero_2bits
    }
    for (std::uint32_t i = 0; i < maxSubLayersMinus1; ++i) {
        if (profilePresent[i]) {
            bits.skip(88);
        }
        if (levelPresent[i]) {
            bits.skip(8);
        }
    }
}

// hrd_parameters() (E.2.2).
Problem hrdParameters(nal::Rbsp& rbsp, std::string_view what, bool commonInfo, std::uint32_t maxSubLayersMinus1) {
    MsbBits& bits = rbsp.bits();
    bool nal = false;
    bool vcl = false;
    bool subPic = false;
    if (commonInfo) {
        nal = bits.flag();
        vcl = bits.flag();
        if (nal || vcl) {
            subPic = bits.flag();
            if (subPic) {
                bits.skip(8 + 5 + 1 + 5);
            }
            bits.skip(4 + 4);
            if (subPic) {
                bits.skip(4);
            }
            bits.skip(5 + 5 + 5);
        }
    }
    for (std::uint32_t i = 0; i <= maxSubLayersMinus1 && !bits.malformed(); ++i) {
        const bool fixedGeneral = bits.flag();
        const bool fixedWithinCvs = fixedGeneral || bits.flag();
        bool lowDelay = false;
        if (fixedWithinCvs) {
            static_cast<void>(bits.ue());  // elemental_duration_in_tc_minus1
        } else {
            lowDelay = bits.flag();
        }
        std::uint32_t cpbCountMinus1 = 0;
        if (!lowDelay) {
            cpbCountMinus1 = bits.ue();
            if (cpbCountMinus1 > 31) {
                return why(rbsp, what, range("cpb_cnt_minus1", cpbCountMinus1, 0, 31));
            }
        }
        const int layers = (nal ? 1 : 0) + (vcl ? 1 : 0);
        for (int layer = 0; layer < layers; ++layer) {
            for (std::uint32_t j = 0; j <= cpbCountMinus1 && !bits.malformed(); ++j) {
                static_cast<void>(bits.ue());  // bit_rate_value_minus1
                static_cast<void>(bits.ue());  // cpb_size_value_minus1
                if (subPic) {
                    static_cast<void>(bits.ue());  // cpb_size_du_value_minus1
                    static_cast<void>(bits.ue());  // bit_rate_du_value_minus1
                }
                bits.skip(1);  // cbr_flag
            }
        }
    }
    return std::nullopt;
}

// scaling_list_data() (7.3.4).
Problem scalingListData(nal::Rbsp& rbsp, std::string_view what) {
    MsbBits& bits = rbsp.bits();
    for (std::uint32_t sizeId = 0; sizeId < 4; ++sizeId) {
        for (std::uint32_t matrixId = 0; matrixId < 6; matrixId += sizeId == 3 ? 3 : 1) {
            if (!bits.flag()) {  // scaling_list_pred_mode_flag
                const std::uint64_t delta = std::uint64_t{bits.ue()} * (sizeId == 3 ? 3 : 1);
                if (delta > matrixId) {
                    return why(rbsp, what,
                               "scaling_list_pred_matrix_id_delta for list " + std::to_string(matrixId) +
                                   " of size " + std::to_string(sizeId) + " points before the first list");
                }
                continue;
            }
            const std::uint32_t coefficients = std::min<std::uint32_t>(64, 1U << (4 + (sizeId << 1)));
            if (sizeId > 1) {
                const std::int32_t dc = bits.se();
                if (dc < -7 || dc > 247) {
                    return why(rbsp, what, range("scaling_list_dc_coef_minus8", dc, -7, 247));
                }
            }
            for (std::uint32_t i = 0; i < coefficients && !bits.malformed(); ++i) {
                static_cast<void>(bits.se());  // scaling_list_delta_coef
            }
        }
    }
    return std::nullopt;
}

// st_ref_pic_set(index) (7.3.7) with the derivation of 7.4.8, as FFmpeg
// reads it. `sets` are the sets before it (in a slice header, all of the
// SPS's: index is their count).
Problem shortTermRps(nal::Rbsp& rbsp, std::string_view what, const std::vector<ShortTermRps>& sets,
                     std::uint32_t index, bool inSliceHeader, ShortTermRps& out) {
    MsbBits& bits = rbsp.bits();
    out = ShortTermRps{};
    const bool predicted = index != 0 && bits.flag();  // inter_ref_pic_set_prediction_flag
    if (predicted) {
        std::uint32_t reference = index - 1;
        if (inSliceHeader) {
            const std::uint32_t deltaIdxMinus1 = bits.ue();
            if (std::uint64_t{deltaIdxMinus1} + 1 > index) {
                return why(rbsp, what, range("delta_idx_minus1", deltaIdxMinus1, 0, index - 1));
            }
            reference = index - (deltaIdxMinus1 + 1);
        }
        const ShortTermRps& from = sets[reference];
        const bool negative = bits.flag();  // delta_rps_sign
        const std::uint32_t absMinus1 = bits.ue();
        if (absMinus1 > 32767) {
            return why(rbsp, what, range("abs_delta_rps_minus1", absMinus1, 0, 32767));
        }
        const std::int32_t deltaRps = (negative ? -1 : 1) * static_cast<std::int32_t>(absMinus1 + 1);
        std::array<std::pair<std::int32_t, bool>, 33> entries{};
        std::uint32_t count = 0;
        for (std::uint32_t j = 0; j <= from.numDelta; ++j) {
            const bool used = bits.flag();             // used_by_curr_pic_flag
            const bool useDelta = used || bits.flag();  // use_delta_flag (1 when absent)
            if (!useDelta) {
                continue;
            }
            if (count == 32) {
                return why(rbsp, what, "a short-term reference picture set of more than 32 pictures");
            }
            const std::int32_t delta = j < from.numDelta ? deltaRps + from.deltaPoc[j] : deltaRps;
            entries[count++] = {delta, used};
        }
        // Negative deltas closest first, then positive ones closest first.
        std::stable_sort(entries.begin(), entries.begin() + count,
                         [](const auto& left, const auto& right) { return left.first < right.first; });
        std::uint32_t negatives = 0;
        while (negatives < count && entries[negatives].first < 0) {
            ++negatives;
        }
        std::reverse(entries.begin(), entries.begin() + negatives);
        out.numNegative = negatives;
        out.numDelta = count;
        for (std::uint32_t i = 0; i < count; ++i) {
            out.deltaPoc[i] = entries[i].first;
            out.used[i] = entries[i].second;
        }
        return std::nullopt;
    }
    const std::uint32_t negatives = bits.ue();
    const std::uint32_t positives = bits.ue();
    if (negatives >= kMaxRefs || positives >= kMaxRefs) {
        return why(rbsp, what,
                   "num_negative_pics " + std::to_string(negatives) + " and num_positive_pics " +
                       std::to_string(positives) + " (each 0 to 15)");
    }
    out.numNegative = negatives;
    out.numDelta = negatives + positives;
    std::int32_t poc = 0;
    for (std::uint32_t i = 0; i < out.numDelta; ++i) {
        if (i == negatives) {
            poc = 0;
        }
        const std::uint32_t minus1 = bits.ue();
        if (minus1 > 32767) {
            return why(rbsp, what,
                       range(i < negatives ? "delta_poc_s0_minus1" : "delta_poc_s1_minus1", minus1, 0, 32767));
        }
        poc += (i < negatives ? -1 : 1) * static_cast<std::int32_t>(minus1 + 1);
        out.deltaPoc[i] = poc;
        out.used[i] = bits.flag();
    }
    return std::nullopt;
}

// vui_parameters() (E.2.1); the caller accepts a VUI that ends early.
Problem vuiParameters(nal::Rbsp& rbsp, std::uint32_t maxSubLayersMinus1) {
    MsbBits& bits = rbsp.bits();
    if (bits.flag()) {  // aspect_ratio_info_present_flag
        if (bits.read(8) == 255) {
            bits.skip(16 + 16);
        }
    }
    if (bits.flag()) {  // overscan_info_present_flag
        bits.skip(1);
    }
    if (bits.flag()) {  // video_signal_type_present_flag
        bits.skip(3 + 1);
        if (bits.flag()) {
            bits.skip(8 + 8 + 8);
        }
    }
    if (bits.flag()) {  // chroma_loc_info_present_flag
        static_cast<void>(bits.ue());
        static_cast<void>(bits.ue());
    }
    bits.skip(3);       // neutral_chroma_indication_flag, field_seq_flag, frame_field_info_present_flag
    if (bits.flag()) {  // default_display_window_flag
        for (int i = 0; i < 4; ++i) {
            static_cast<void>(bits.ue());
        }
    }
    if (bits.flag()) {  // vui_timing_info_present_flag
        bits.skip(32 + 32);
        if (bits.flag()) {  // vui_poc_proportional_to_timing_flag
            static_cast<void>(bits.ue());
        }
        if (bits.flag()) {  // vui_hrd_parameters_present_flag
            if (Problem problem = hrdParameters(rbsp, "the VUI", true, maxSubLayersMinus1)) {
                return problem;
            }
        }
    }
    if (bits.flag()) {  // bitstream_restriction_flag
        bits.skip(3);
        for (int i = 0; i < 5; ++i) {
            static_cast<void>(bits.ue());
        }
    }
    return std::nullopt;
}

Problem readVps(nal::Rbsp& rbsp, Vps& vps) {
    MsbBits& bits = rbsp.bits();
    constexpr std::string_view kWhat = "the VPS";
    vps.id = bits.read(4);
    if (bits.read(2) != 3) {
        return why(rbsp, kWhat, "vps_base_layer_internal_flag and vps_base_layer_available_flag are not both 1");
    }
    bits.skip(6);  // vps_max_layers_minus1
    const std::uint32_t subLayersMinus1 = bits.read(3);
    bits.skip(1);  // vps_temporal_id_nesting_flag
    if (bits.read(16) != 0xFFFF) {
        return why(rbsp, kWhat, "vps_reserved_0xffff_16bits is not 0xFFFF");
    }
    if (subLayersMinus1 + 1 > kMaxSubLayers) {
        return why(rbsp, kWhat, range("vps_max_sub_layers_minus1", subLayersMinus1, 0, kMaxSubLayers - 1));
    }
    vps.maxSubLayers = subLayersMinus1 + 1;
    std::uint32_t profile = 0;
    profileTierLevel(bits, subLayersMinus1, profile);
    if (rbsp.overrun()) {
        return why(rbsp, kWhat, {});
    }
    const bool orderingInfo = bits.flag();
    for (std::uint32_t i = orderingInfo ? 0 : subLayersMinus1; i <= subLayersMinus1; ++i) {
        const std::uint32_t bufferingMinus1 = bits.ue();
        static_cast<void>(bits.ue());  // vps_max_num_reorder_pics (FFmpeg only warns)
        static_cast<void>(bits.ue());  // vps_max_latency_increase_plus1
        if (bufferingMinus1 >= kMaxDpbSize) {
            return why(rbsp, kWhat, range("vps_max_dec_pic_buffering_minus1", bufferingMinus1, 0, kMaxDpbSize - 1));
        }
    }
    const std::uint32_t maxLayerId = bits.read(6);
    const std::uint32_t layerSetsMinus1 = bits.ue();
    if (layerSetsMinus1 > 1023 || std::uint64_t{layerSetsMinus1} * (maxLayerId + 1) > bits.left()) {
        return why(rbsp, kWhat, range("vps_num_layer_sets_minus1", layerSetsMinus1, 0, 1023));
    }
    bits.skip(std::uint64_t{layerSetsMinus1} * (maxLayerId + 1));  // layer_id_included_flag
    if (bits.flag()) {                                               // vps_timing_info_present_flag
        bits.skip(32 + 32);
        if (bits.flag()) {  // vps_poc_proportional_to_timing_flag
            static_cast<void>(bits.ue());
        }
        const std::uint32_t hrds = bits.ue();
        if (hrds > layerSetsMinus1 + 1) {
            return why(rbsp, kWhat, range("vps_num_hrd_parameters", hrds, 0, layerSetsMinus1 + 1));
        }
        for (std::uint32_t i = 0; i < hrds && !bits.malformed(); ++i) {
            static_cast<void>(bits.ue());  // hrd_layer_set_idx
            const bool common = i == 0 || bits.flag();
            if (Problem problem = hrdParameters(rbsp, kWhat, common, subLayersMinus1)) {
                return problem;
            }
        }
    }
    // vps_extension_flag and what follows are not read; FFmpeg accepts a VPS
    // that ends early.
    return std::nullopt;
}

// The SPS's extensions after its VUI (7.3.2.2.1).
Problem spsExtensions(nal::Rbsp& rbsp, Sps& sps) {
    MsbBits& bits = rbsp.bits();
    constexpr std::string_view kWhat = "the SPS";
    if (!bits.flag()) {  // sps_extension_present_flag
        return std::nullopt;
    }
    const bool rangeExtension = bits.flag();
    const bool multilayer = bits.flag();
    const bool threeD = bits.flag();
    const bool scc = bits.flag();
    bits.skip(4);  // sps_extension_4bits
    if (rangeExtension) {
        bits.skip(9);
    }
    if (multilayer) {
        bits.skip(1);  // inter_view_mv_vert_constraint_flag
    }
    if (threeD) {
        // sps_3d_extension(): for texture, then depth.
        bits.skip(2);                  // iv_di_mc_enabled_flag, iv_mv_scal_enabled_flag
        static_cast<void>(bits.ue());  // log2_ivmc_sub_pb_size_minus3
        bits.skip(4);                  // iv_res_pred, depth_ref, vsp_mc, dbbp enabled flags
        bits.skip(2 + 1);              // iv_di_mc, iv_mv_scal, tex_mc enabled flags
        static_cast<void>(bits.ue());  // log2_texmc_sub_pb_size_minus3
        bits.skip(5);                  // intra_contour ... skip_intra enabled flags
    }
    if (scc) {
        bits.skip(1);       // sps_curr_pic_ref_enabled_flag
        if (bits.flag()) {  // palette_mode_enabled_flag
            static_cast<void>(bits.ue());  // palette_max_size
            static_cast<void>(bits.ue());  // delta_palette_max_predictor_size
            if (bits.flag()) {             // sps_palette_predictor_initializers_present_flag
                const std::uint64_t initializers = std::uint64_t{bits.ue()} + 1;
                if (initializers > kMaxPalettePredictor) {
                    return why(rbsp, kWhat,
                               range("sps_num_palette_predictor_initializers_minus1", initializers - 1, 0,
                                     kMaxPalettePredictor - 1));
                }
                const std::uint64_t perComponent = sps.bitDepthLuma * initializers;
                bits.skip(sps.chromaFormatIdc == 0 ? perComponent
                                                   : perComponent + 2 * sps.bitDepthChroma * initializers);
            }
        }
        sps.motionVectorResolutionControlIdc = bits.read(2);
        bits.skip(1);  // intra_boundary_filtering_disabled_flag
    }
    return std::nullopt;
}

Problem readSps(nal::Rbsp& rbsp, const ParameterSets& sets, Sps& sps) {
    MsbBits& bits = rbsp.bits();
    constexpr std::string_view kWhat = "the SPS";
    sps.vpsId = bits.read(4);
    if (!sets.vps[sps.vpsId].has_value()) {
        return why(rbsp, kWhat, "it refers to VPS " + std::to_string(sps.vpsId) + ", which has not been sent");
    }
    sps.maxSubLayersMinus1 = bits.read(3);
    if (sps.maxSubLayersMinus1 + 1 > kMaxSubLayers) {
        return why(rbsp, kWhat, range("sps_max_sub_layers_minus1", sps.maxSubLayersMinus1, 0, kMaxSubLayers - 1));
    }
    bits.skip(1);  // sps_temporal_id_nesting_flag
    profileTierLevel(bits, sps.maxSubLayersMinus1, sps.profileIdc);
    if (rbsp.overrun()) {
        return why(rbsp, kWhat, {});
    }
    sps.id = bits.ue();
    if (sps.id > 15) {
        return why(rbsp, kWhat, range("sps_seq_parameter_set_id", sps.id, 0, 15));
    }
    sps.chromaFormatIdc = bits.ue();
    if (sps.chromaFormatIdc > 3) {
        return why(rbsp, kWhat, range("chroma_format_idc", sps.chromaFormatIdc, 0, 3));
    }
    if (sps.chromaFormatIdc == 3) {
        sps.separateColourPlane = bits.flag();
    }
    sps.width = bits.ue();
    sps.height = bits.ue();
    const std::uint64_t width = sps.width;
    const std::uint64_t height = sps.height;
    if (!bits.malformed() && (width == 0 || height == 0 || (width + 128) * (height + 128) >= 0x7FFFFFFF / 8)) {
        return why(rbsp, kWhat, "a picture of " + std::to_string(width) + "x" + std::to_string(height) + " samples");
    }
    if (bits.flag()) {  // conformance_window_flag (FFmpeg ignores a window that is too large)
        for (int i = 0; i < 4; ++i) {
            static_cast<void>(bits.ue());
        }
    }
    const std::uint32_t lumaMinus8 = bits.ue();
    const std::uint32_t chromaMinus8 = bits.ue();
    if (lumaMinus8 > 8) {
        return why(rbsp, kWhat, range("bit_depth_luma_minus8", lumaMinus8, 0, 8));
    }
    if (chromaMinus8 > 8) {
        return why(rbsp, kWhat, range("bit_depth_chroma_minus8", chromaMinus8, 0, 8));
    }
    sps.bitDepthLuma = lumaMinus8 + 8;
    sps.bitDepthChroma = chromaMinus8 + 8;
    const std::uint32_t pocLsbMinus4 = bits.ue();
    if (pocLsbMinus4 > 12) {
        return why(rbsp, kWhat, range("log2_max_pic_order_cnt_lsb_minus4", pocLsbMinus4, 0, 12));
    }
    sps.log2MaxPocLsb = pocLsbMinus4 + 4;
    const bool orderingInfo = bits.flag();
    for (std::uint32_t i = orderingInfo ? 0 : sps.maxSubLayersMinus1; i <= sps.maxSubLayersMinus1; ++i) {
        const std::uint32_t bufferingMinus1 = bits.ue();
        const std::uint32_t reorder = bits.ue();
        static_cast<void>(bits.ue());  // sps_max_latency_increase_plus1
        if (bufferingMinus1 >= kMaxDpbSize) {
            return why(rbsp, kWhat, range("sps_max_dec_pic_buffering_minus1", bufferingMinus1, 0, kMaxDpbSize - 1));
        }
        if (reorder > bufferingMinus1 && reorder > kMaxDpbSize - 1) {
            return why(rbsp, kWhat, range("sps_max_num_reorder_pics", reorder, 0, bufferingMinus1));
        }
    }
    const std::uint32_t minCbMinus3 = bits.ue();
    const std::uint32_t diffCb = bits.ue();
    const std::uint32_t minTbMinus2 = bits.ue();
    const std::uint32_t diffTb = bits.ue();
    if (minCbMinus3 > 27) {
        return why(rbsp, kWhat, range("log2_min_luma_coding_block_size_minus3", minCbMinus3, 0, 27));
    }
    if (diffCb > 30) {
        return why(rbsp, kWhat, range("log2_diff_max_min_luma_coding_block_size", diffCb, 0, 30));
    }
    sps.log2MinCbSize = minCbMinus3 + 3;
    sps.log2DiffMaxMinCb = diffCb;
    if (std::uint64_t{minTbMinus2} + 2 >= sps.log2MinCbSize) {
        return why(rbsp, kWhat,
                   "log2_min_luma_transform_block_size_minus2 " + std::to_string(minTbMinus2) +
                       ": transform blocks as large as the smallest coding block");
    }
    if (diffTb > 30) {
        return why(rbsp, kWhat, range("log2_diff_max_min_luma_transform_block_size", diffTb, 0, 30));
    }
    sps.log2MinTbSize = minTbMinus2 + 2;
    const std::uint32_t depthInter = bits.ue();
    const std::uint32_t depthIntra = bits.ue();
    if (bits.flag()) {      // scaling_list_enabled_flag
        if (bits.flag()) {  // sps_scaling_list_data_present_flag
            if (Problem problem = scalingListData(rbsp, kWhat)) {
                return problem;
            }
        }
    }
    bits.skip(1);  // amp_enabled_flag
    sps.sampleAdaptiveOffset = bits.flag();
    if (bits.flag()) {  // pcm_enabled_flag
        const std::uint32_t pcmLuma = bits.read(4) + 1;
        const std::uint32_t pcmChroma = bits.read(4) + 1;
        static_cast<void>(bits.ue());  // log2_min_pcm_luma_coding_block_size_minus3
        static_cast<void>(bits.ue());  // log2_diff_max_min_pcm_luma_coding_block_size
        if (std::max(pcmLuma, pcmChroma) > sps.bitDepthLuma) {
            return why(rbsp, kWhat,
                       "PCM bit depths " + std::to_string(pcmLuma) + " and " + std::to_string(pcmChroma) +
                           " above the bit depth " + std::to_string(sps.bitDepthLuma));
        }
        bits.skip(1);  // pcm_loop_filter_disabled_flag
    }
    const std::uint32_t shortTermSets = bits.ue();
    if (shortTermSets > kMaxShortTermSets) {
        return why(rbsp, kWhat, range("num_short_term_ref_pic_sets", shortTermSets, 0, kMaxShortTermSets));
    }
    sps.shortTermSets.reserve(shortTermSets);
    for (std::uint32_t i = 0; i < shortTermSets; ++i) {
        ShortTermRps set;
        if (Problem problem = shortTermRps(rbsp, kWhat, sps.shortTermSets, i, false, set)) {
            return problem;
        }
        sps.shortTermSets.push_back(set);
    }
    sps.longTermRefsPresent = bits.flag();
    if (sps.longTermRefsPresent) {
        sps.numLongTermRefPicsSps = bits.ue();
        if (sps.numLongTermRefPicsSps > kMaxLongTermSps) {
            return why(rbsp, kWhat, range("num_long_term_ref_pics_sps", sps.numLongTermRefPicsSps, 0, kMaxLongTermSps));
        }
        for (std::uint32_t i = 0; i < sps.numLongTermRefPicsSps; ++i) {
            bits.skip(sps.log2MaxPocLsb);  // lt_ref_pic_poc_lsb_sps
            sps.usedByCurrPicLtSps[i] = bits.flag();
        }
    }
    sps.temporalMvp = bits.flag();
    bits.skip(1);  // strong_intra_smoothing_enabled_flag
    const bool vui = bits.flag();
    if (rbsp.overrun() || bits.malformed()) {
        return why(rbsp, kWhat, {});
    }
    // FFmpeg's checks of the derived sizes.
    sps.log2CtbSize = sps.log2MinCbSize + sps.log2DiffMaxMinCb;
    if (sps.log2CtbSize > 6 || sps.log2CtbSize < 4) {
        return "coding tree blocks of 2^" + std::to_string(sps.log2CtbSize) + " samples (2^4 to 2^6)";
    }
    const std::uint32_t minCb = 1U << sps.log2MinCbSize;
    if (sps.width % minCb != 0 || sps.height % minCb != 0) {
        return "a coded size of " + std::to_string(sps.width) + "x" + std::to_string(sps.height) +
               " that is not a multiple of the smallest coding block (" + std::to_string(minCb) + ")";
    }
    const std::uint32_t maxDepth = sps.log2CtbSize - sps.log2MinTbSize;
    if (depthInter > maxDepth) {
        return range("max_transform_hierarchy_depth_inter", depthInter, 0, maxDepth);
    }
    if (depthIntra > maxDepth) {
        return range("max_transform_hierarchy_depth_intra", depthIntra, 0, maxDepth);
    }
    if (std::uint64_t{sps.log2MinTbSize} + diffTb > std::min<std::uint32_t>(sps.log2CtbSize, 5)) {
        return "transform blocks of 2^" + std::to_string(std::uint64_t{sps.log2MinTbSize} + diffTb) +
               " samples, larger than its coding tree blocks or 32";
    }
    if (vui) {
        // A VUI that does not read cleanly, and the extensions after it,
        // are accepted with the extensions taken as absent.
        const Problem problem = vuiParameters(rbsp, sps.maxSubLayersMinus1);
        if (problem.has_value() || rbsp.overrun() || bits.malformed() || spsExtensions(rbsp, sps).has_value() ||
            rbsp.overrun() || bits.malformed()) {
            sps.vuiCut = true;
            sps.motionVectorResolutionControlIdc = 0;
        }
        return std::nullopt;
    }
    if (Problem problem = spsExtensions(rbsp, sps)) {
        return problem;
    }
    if (rbsp.overrun() || bits.malformed()) {
        return why(rbsp, kWhat, {});
    }
    return std::nullopt;
}

// pps_range_extension() and pps_scc_extension() (7.3.2.3.2, 7.3.2.3.3).
Problem ppsExtensions(nal::Rbsp& rbsp, const Sps& sps, bool transformSkip, Pps& pps) {
    MsbBits& bits = rbsp.bits();
    constexpr std::string_view kWhat = "the PPS";
    if (!bits.flag()) {  // pps_extension_present_flag
        return std::nullopt;
    }
    const bool rangeExtension = bits.flag();
    const bool multilayer = bits.flag();
    const bool threeD = bits.flag();
    const bool scc = bits.flag();
    bits.skip(4);  // pps_extension_4bits
    if (rangeExtension) {
        if (transformSkip) {
            static_cast<void>(bits.ue());  // log2_max_transform_skip_block_size_minus2
        }
        if (bits.flag() && sps.chromaFormatIdc != 3) {  // cross_component_prediction_enabled_flag
            return why(rbsp, kWhat, "cross-component prediction without 4:4:4 chroma");
        }
        pps.chromaQpOffsetListEnabled = bits.flag();
        if (pps.chromaQpOffsetListEnabled) {
            static_cast<void>(bits.ue());  // diff_cu_chroma_qp_offset_depth
            const std::uint32_t lengthMinus1 = bits.ue();
            if (lengthMinus1 > 5) {
                return why(rbsp, kWhat, range("chroma_qp_offset_list_len_minus1", lengthMinus1, 0, 5));
            }
            for (std::uint32_t i = 0; i <= lengthMinus1; ++i) {
                static_cast<void>(bits.se());  // cb_qp_offset_list
                static_cast<void>(bits.se());  // cr_qp_offset_list
            }
        }
        const std::uint32_t saoLuma = bits.ue();
        const std::uint32_t saoChroma = bits.ue();
        const std::uint32_t maxLuma = sps.bitDepthLuma > 10 ? sps.bitDepthLuma - 10 : 0;
        const std::uint32_t maxChroma = sps.bitDepthChroma > 10 ? sps.bitDepthChroma - 10 : 0;
        if (saoLuma > maxLuma) {
            return why(rbsp, kWhat, range("log2_sao_offset_scale_luma", saoLuma, 0, maxLuma));
        }
        if (saoChroma > maxChroma) {
            return why(rbsp, kWhat, range("log2_sao_offset_scale_chroma", saoChroma, 0, maxChroma));
        }
    }
    if (multilayer || threeD) {
        pps.extensionsUnread = true;
        return std::nullopt;
    }
    if (scc) {
        pps.currPicRefEnabled = bits.flag();
        if (bits.flag()) {  // residual_adaptive_colour_transform_enabled_flag
            pps.sliceActQpOffsetsPresent = bits.flag();
            const std::int64_t y = std::int64_t{bits.se()} - 5;
            const std::int64_t cb = std::int64_t{bits.se()} - 5;
            const std::int64_t cr = std::int64_t{bits.se()} - 3;
            for (const std::int64_t offset : {y, cb, cr}) {
                if (offset <= -12 || offset >= 12) {
                    return why(rbsp, kWhat, "a colour transform QP offset outside -12 to 12");
                }
            }
        }
        if (bits.flag()) {  // pps_palette_predictor_initializers_present_flag
            const std::uint32_t initializers = bits.ue();
            if (initializers > kMaxPalettePredictor) {
                return why(rbsp, kWhat,
                           range("pps_num_palette_predictor_initializers", initializers, 0, kMaxPalettePredictor));
            }
            if (initializers > 0) {
                const bool monochrome = bits.flag();
                const std::uint32_t lumaDepth = bits.ue() + 8;
                if (lumaDepth != sps.bitDepthLuma) {
                    return why(rbsp, kWhat, "luma_bit_depth_entry_minus8 differs from the SPS's bit depth");
                }
                std::uint32_t chromaDepth = 0;
                if (!monochrome) {
                    chromaDepth = bits.ue() + 8;
                    if (chromaDepth != sps.bitDepthChroma) {
                        return why(rbsp, kWhat, "chroma_bit_depth_entry_minus8 differs from the SPS's bit depth");
                    }
                }
                bits.skip(std::uint64_t{initializers} * (lumaDepth + (monochrome ? 0 : 2 * chromaDepth)));
            }
        }
    }
    return std::nullopt;
}

Problem readPps(nal::Rbsp& rbsp, const ParameterSets& sets, Pps& pps) {
    MsbBits& bits = rbsp.bits();
    constexpr std::string_view kWhat = "the PPS";
    pps.id = bits.ue();
    if (pps.id > 63) {
        return why(rbsp, kWhat, range("pps_pic_parameter_set_id", pps.id, 0, 63));
    }
    pps.spsId = bits.ue();
    if (pps.spsId > 15) {
        return why(rbsp, kWhat, range("pps_seq_parameter_set_id", pps.spsId, 0, 15));
    }
    if (!sets.sps[pps.spsId].has_value()) {
        return why(rbsp, kWhat, "it refers to SPS " + std::to_string(pps.spsId) + ", which has not been sent");
    }
    const Sps& sps = *sets.sps[pps.spsId];
    pps.dependentSliceSegmentsEnabled = bits.flag();
    pps.outputFlagPresent = bits.flag();
    pps.numExtraSliceHeaderBits = bits.read(3);
    bits.skip(1);  // sign_data_hiding_enabled_flag
    pps.cabacInitPresent = bits.flag();
    for (std::uint32_t list = 0; list < 2; ++list) {
        const std::uint32_t minus1 = bits.ue();
        if (minus1 + 1 >= kMaxRefs) {
            const char* field =
                list == 0 ? "num_ref_idx_l0_default_active_minus1" : "num_ref_idx_l1_default_active_minus1";
            return why(rbsp, kWhat, range(field, minus1, 0, kMaxRefs - 2));
        }
        pps.numRefIdxDefault[list] = minus1 + 1;
    }
    pps.initQpMinus26 = bits.se();
    bits.skip(1);  // constrained_intra_pred_flag
    const bool transformSkip = bits.flag();
    if (bits.flag()) {  // cu_qp_delta_enabled_flag
        const std::uint32_t depth = bits.ue();
        if (depth > sps.log2DiffMaxMinCb) {
            return why(rbsp, kWhat, range("diff_cu_qp_delta_depth", depth, 0, sps.log2DiffMaxMinCb));
        }
    }
    const std::int32_t cbOffset = bits.se();
    if (cbOffset < -12 || cbOffset > 12) {
        return why(rbsp, kWhat, range("pps_cb_qp_offset", cbOffset, -12, 12));
    }
    const std::int32_t crOffset = bits.se();
    if (crOffset < -12 || crOffset > 12) {
        return why(rbsp, kWhat, range("pps_cr_qp_offset", crOffset, -12, 12));
    }
    pps.sliceChromaQpOffsetsPresent = bits.flag();
    pps.weightedPred = bits.flag();
    pps.weightedBipred = bits.flag();
    bits.skip(1);  // transquant_bypass_enabled_flag
    pps.tilesEnabled = bits.flag();
    pps.entropyCodingSync = bits.flag();
    if (pps.tilesEnabled) {
        const std::uint32_t columnsMinus1 = bits.ue();
        const std::uint32_t rowsMinus1 = bits.ue();
        if (columnsMinus1 >= sps.ctbWidth()) {
            return why(rbsp, kWhat, range("num_tile_columns_minus1", columnsMinus1, 0, sps.ctbWidth() - 1));
        }
        if (rowsMinus1 >= sps.ctbHeight()) {
            return why(rbsp, kWhat, range("num_tile_rows_minus1", rowsMinus1, 0, sps.ctbHeight() - 1));
        }
        pps.tileColumns = columnsMinus1 + 1;
        pps.tileRows = rowsMinus1 + 1;
        if (!bits.flag()) {  // uniform_spacing_flag
            std::uint64_t sum = 0;
            for (std::uint32_t i = 0; i < columnsMinus1; ++i) {
                sum += std::uint64_t{bits.ue()} + 1;
            }
            if (sum >= sps.ctbWidth()) {
                return why(rbsp, kWhat, "tile columns wider than the picture");
            }
            sum = 0;
            for (std::uint32_t i = 0; i < rowsMinus1; ++i) {
                sum += std::uint64_t{bits.ue()} + 1;
            }
            if (sum >= sps.ctbHeight()) {
                return why(rbsp, kWhat, "tile rows taller than the picture");
            }
        }
        bits.skip(1);  // loop_filter_across_tiles_enabled_flag
    }
    pps.loopFilterAcrossSlices = bits.flag();
    pps.deblockingControlPresent = bits.flag();
    if (pps.deblockingControlPresent) {
        pps.deblockingOverrideEnabled = bits.flag();
        pps.deblockingDisabled = bits.flag();
        if (!pps.deblockingDisabled) {
            const std::int32_t beta = bits.se();
            const std::int32_t tc = bits.se();
            if (beta < -6 || beta > 6) {
                return why(rbsp, kWhat, range("pps_beta_offset_div2", beta, -6, 6));
            }
            if (tc < -6 || tc > 6) {
                return why(rbsp, kWhat, range("pps_tc_offset_div2", tc, -6, 6));
            }
        }
    }
    if (bits.flag()) {  // pps_scaling_list_data_present_flag
        if (Problem problem = scalingListData(rbsp, kWhat)) {
            return problem;
        }
    }
    pps.listsModificationPresent = bits.flag();
    const std::uint32_t mergeLevelMinus2 = bits.ue();
    if (mergeLevelMinus2 > sps.log2CtbSize) {
        return why(rbsp, kWhat, range("log2_parallel_merge_level_minus2", mergeLevelMinus2, 0, sps.log2CtbSize));
    }
    pps.sliceHeaderExtensionPresent = bits.flag();
    if (rbsp.overrun() || bits.malformed()) {
        // FFmpeg only warns about a PPS that ends early.
        return std::nullopt;
    }
    if (Problem problem = ppsExtensions(rbsp, sps, transformSkip, pps); problem && !rbsp.overrun()) {
        return problem;
    }
    return std::nullopt;
}

}  // namespace

std::uint32_t ShortTermRps::usedCount() const noexcept {
    return static_cast<std::uint32_t>(std::count(used.begin(), used.begin() + numDelta, true));
}

ConfigRead readDecoderConfig(std::span<const std::uint8_t> payload) {
    ConfigRead read;
    constexpr std::size_t kFixed = 23;
    if (payload.size() < kFixed) {
        read.problem = "the hvcC box holds " + std::to_string(payload.size()) + " bytes, fewer than its 23 fixed ones";
        return read;
    }
    if (payload[0] > 1) {
        read.unsupported = true;
        read.problem = "hvcC configurationVersion " + std::to_string(payload[0]) + " (only 1 is defined)";
        return read;
    }
    DecoderConfig config;
    config.profileIdc = payload[1] & 0x1FU;
    config.lengthSize = (payload[21] & 3U) + 1;
    if (config.lengthSize == 3) {
        read.problem = "hvcC lengthSizeMinusOne 2 (NAL unit lengths of 3 bytes are not allowed)";
        return read;
    }
    const std::size_t arrays = payload[22];
    std::size_t position = kFixed;
    for (std::size_t array = 0; array < arrays; ++array) {
        if (payload.size() - position < 3) {
            read.problem = "the hvcC box ends inside its NAL unit arrays";
            return read;
        }
        const std::size_t count = (std::size_t{payload[position + 1]} << 8) | payload[position + 2];
        position += 3;
        for (std::size_t i = 0; i < count; ++i) {
            if (payload.size() - position < 2) {
                read.problem = "the hvcC box ends inside its NAL unit arrays";
                return read;
            }
            const std::size_t length = (std::size_t{payload[position]} << 8) | payload[position + 1];
            position += 2;
            if (length == 0 || payload.size() - position < length) {
                read.problem = length == 0 ? "the hvcC box holds an empty NAL unit"
                                           : "the hvcC box ends inside its NAL unit arrays";
                return read;
            }
            config.units.emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(position),
                                      payload.begin() + static_cast<std::ptrdiff_t>(position + length));
            position += length;
        }
    }
    read.config = std::move(config);
    return read;
}

std::optional<std::string> Stream::configure(const DecoderConfig& config) {
    std::size_t index = 0;
    for (const std::vector<std::uint8_t>& unit : config.units) {
        ++index;
        nal::SequenceScan scan;
        scan.feed(unit, 0);
        const std::string where = "NAL unit " + std::to_string(index) + " of the hvcC box (" + kindOf(unit[0]) + "): ";
        if (scan.violation().has_value()) {
            return where + scan.violation()->detail;
        }
        if (std::optional<std::string> problem = nalUnit(unit, unit.size())) {
            return where + *problem;
        }
    }
    return std::nullopt;
}

std::uint64_t Stream::headBytes(std::uint8_t header, std::uint64_t size) noexcept {
    const std::uint32_t type = (header >> 1) & 0x3FU;
    if (type == kVps || type == kSps || type == kPps) {
        return std::min<std::uint64_t>(size, kMaxParameterSet + 1);
    }
    if (isSlice(type)) {
        return std::min<std::uint64_t>(size, kSliceHead);
    }
    return std::min<std::uint64_t>(size, 2);
}

std::string Stream::kindOf(std::uint8_t header) {
    const std::uint32_t type = (header >> 1) & 0x3FU;
    if (type <= 9) {
        return "slice segment";
    }
    if (type >= 16 && type <= 18) {
        return "BLA slice segment";
    }
    if (type == kIdrWithRadl || type == kIdrNoLeading) {
        return "IDR slice segment";
    }
    switch (type) {
    case 21:
        return "CRA slice segment";
    case kVps:
        return "VPS";
    case kSps:
        return "SPS";
    case kPps:
        return "PPS";
    case 35:
        return "access unit delimiter";
    case 36:
        return "end of sequence";
    case 37:
        return "end of bitstream";
    case 38:
        return "filler data";
    case 39:
        return "prefix SEI";
    case 40:
        return "suffix SEI";
    default:
        return "type " + std::to_string(type);
    }
}

std::optional<std::string> Stream::nalUnit(std::span<const std::uint8_t> head, std::uint64_t size) {
    if (head.size() < 2 || size < 2) {
        return "a NAL unit of " + std::to_string(size) + (size == 1 ? " byte" : " bytes") +
               ", shorter than its header";
    }
    if ((head[0] & 0x80U) != 0) {
        return "forbidden_zero_bit is set";
    }
    const std::uint32_t type = (head[0] >> 1) & 0x3FU;
    const std::uint32_t layer = ((head[0] & 1U) << 5) | (head[1] >> 3);
    if ((head[1] & 7U) == 0) {
        return "nuh_temporal_id_plus1 is 0";
    }
    if (layer != 0) {
        ++otherLayers_;
        return std::nullopt;
    }
    const bool complete = head.size() == size;
    const auto whole = [&](const char* name) -> std::optional<std::string> {
        if (!complete) {
            return std::string("a ") + name + " of " + std::to_string(size) + " bytes (at most 64 KiB are read)";
        }
        return std::nullopt;
    };
    switch (type) {
    case kVps:
        ++vpsCount_;
        if (std::optional<std::string> problem = whole("VPS")) {
            return problem;
        }
        return vps(head);
    case kSps:
        ++spsCount_;
        if (std::optional<std::string> problem = whole("SPS")) {
            return problem;
        }
        return sps(head);
    case kPps:
        ++ppsCount_;
        if (std::optional<std::string> problem = whole("PPS")) {
            return problem;
        }
        return pps(head);
    default:
        if (isSlice(type)) {
            ++slices_;
            return slice(head, size, type);
        }
        ++others_;
        return std::nullopt;
    }
}

std::optional<std::string> Stream::vps(std::span<const std::uint8_t> unit) {
    nal::Rbsp rbsp(unit, 2, true);
    Vps parsed;
    if (Problem problem = readVps(rbsp, parsed)) {
        return problem;
    }
    std::vector<std::uint8_t> bytes(unit.begin(), unit.end());
    if (sets_.vps[parsed.id].has_value() && sets_.vpsBytes[parsed.id] != bytes) {
        for (std::size_t id = 0; id < sets_.sps.size(); ++id) {
            if (sets_.sps[id].has_value() && sets_.sps[id]->vpsId == parsed.id) {
                sets_.sps[id].reset();
                for (std::optional<Pps>& pps : sets_.pps) {
                    if (pps.has_value() && pps->spsId == id) {
                        pps.reset();
                    }
                }
            }
        }
    }
    sets_.vps[parsed.id] = parsed;
    sets_.vpsBytes[parsed.id] = std::move(bytes);
    return std::nullopt;
}

std::optional<std::string> Stream::sps(std::span<const std::uint8_t> unit) {
    nal::Rbsp rbsp(unit, 2, true);
    Sps parsed;
    if (Problem problem = readSps(rbsp, sets_, parsed)) {
        return problem;
    }
    std::vector<std::uint8_t> bytes(unit.begin(), unit.end());
    if (sets_.sps[parsed.id].has_value() && sets_.spsBytes[parsed.id] != bytes) {
        for (std::optional<Pps>& pps : sets_.pps) {
            if (pps.has_value() && pps->spsId == parsed.id) {
                pps.reset();
            }
        }
    }
    if (!firstSps_.has_value()) {
        firstSps_ = parsed;
    }
    const std::uint32_t id = parsed.id;
    sets_.sps[id] = std::move(parsed);
    sets_.spsBytes[id] = std::move(bytes);
    return std::nullopt;
}

std::optional<std::string> Stream::pps(std::span<const std::uint8_t> unit) {
    nal::Rbsp rbsp(unit, 2, true);
    Pps parsed;
    if (Problem problem = readPps(rbsp, sets_, parsed)) {
        return problem;
    }
    sets_.pps[parsed.id] = parsed;
    return std::nullopt;
}

std::optional<std::string> Stream::slice(std::span<const std::uint8_t> head, std::uint64_t size,
                                         std::uint32_t type) {
    const bool complete = head.size() == size;
    nal::Rbsp rbsp(head, 2, complete);
    MsbBits& bits = rbsp.bits();
    constexpr std::string_view kWhat = "the slice segment header";
    const bool firstInPicture = bits.flag();
    if (isIrap(type)) {
        bits.skip(1);  // no_output_of_prior_pics_flag
    }
    const std::uint32_t ppsId = bits.ue();
    if (ppsId > 63) {
        return why(rbsp, kWhat, range("slice_pic_parameter_set_id", ppsId, 0, 63));
    }
    if (!sets_.pps[ppsId].has_value()) {
        return why(rbsp, kWhat, "it refers to PPS " + std::to_string(ppsId) + ", which has not been sent");
    }
    const Pps& pps = *sets_.pps[ppsId];
    if (!sets_.sps[pps.spsId].has_value()) {
        return "PPS " + std::to_string(ppsId) + " refers to SPS " + std::to_string(pps.spsId) +
               ", which has not been sent";
    }
    const Sps& sps = *sets_.sps[pps.spsId];
    if (pps.extensionsUnread) {
        ++unreadSlices_;
        return std::nullopt;
    }
    bool dependent = false;
    std::uint64_t address = 0;
    const std::uint64_t pictureCtbs = std::uint64_t{sps.ctbWidth()} * sps.ctbHeight();
    if (!firstInPicture) {
        if (pps.dependentSliceSegmentsEnabled) {
            dependent = bits.flag();
        }
        address = bits.read(nal::ceilLog2(pictureCtbs));
        if (address >= pictureCtbs) {
            return why(rbsp, kWhat, range("slice_segment_address", static_cast<std::int64_t>(address), 0,
                                          static_cast<std::int64_t>(pictureCtbs) - 1));
        }
        if (dependent && address == 0) {
            return why(rbsp, kWhat, "a dependent slice segment at the start of the picture");
        }
    }
    if (!dependent) {
        bits.skip(pps.numExtraSliceHeaderBits);  // slice_reserved_flag
        const std::uint32_t sliceType = bits.ue();
        if (sliceType > 2) {
            return why(rbsp, kWhat, range("slice_type", sliceType, 0, 2));
        }
        if (isIrap(type) && sliceType != kI && !pps.currPicRefEnabled) {
            return why(rbsp, kWhat, "a P or B slice in an IRAP picture");
        }
        if (pps.outputFlagPresent) {
            bits.skip(1);  // pic_output_flag
        }
        if (sps.separateColourPlane) {
            bits.skip(2);  // colour_plane_id
        }
        std::uint32_t pictures = 0;  // NumPicTotalCurr
        bool temporalMvp = false;
        if (type != kIdrWithRadl && type != kIdrNoLeading) {
            bits.skip(sps.log2MaxPocLsb);  // slice_pic_order_cnt_lsb
            const auto sets = static_cast<std::uint32_t>(sps.shortTermSets.size());
            if (!bits.flag()) {  // short_term_ref_pic_set_sps_flag
                ShortTermRps set;
                if (Problem problem = shortTermRps(rbsp, kWhat, sps.shortTermSets, sets, true, set)) {
                    return problem;
                }
                pictures += set.usedCount();
            } else {
                if (sets == 0) {
                    return why(rbsp, kWhat, "it takes a short-term reference picture set from an SPS that has none");
                }
                const std::uint32_t index = bits.read(nal::ceilLog2(sets));
                if (index >= sets) {
                    return why(rbsp, kWhat, range("short_term_ref_pic_set_idx", index, 0, sets - 1));
                }
                pictures += sps.shortTermSets[index].usedCount();
            }
            if (sps.longTermRefsPresent) {
                std::uint32_t fromSps = 0;
                if (sps.numLongTermRefPicsSps > 0) {
                    fromSps = bits.ue();
                }
                const std::uint32_t inHeader = bits.ue();
                if (fromSps > sps.numLongTermRefPicsSps) {
                    return why(rbsp, kWhat, range("num_long_term_sps", fromSps, 0, sps.numLongTermRefPicsSps));
                }
                if (std::uint64_t{fromSps} + inHeader > 32) {
                    return why(rbsp, kWhat, "more than 32 long-term reference pictures");
                }
                for (std::uint32_t i = 0; i < fromSps + inHeader && !bits.malformed(); ++i) {
                    bool used = false;
                    if (i < fromSps) {
                        std::uint32_t index = 0;
                        if (sps.numLongTermRefPicsSps > 1) {
                            index = bits.read(nal::ceilLog2(sps.numLongTermRefPicsSps));
                        }
                        if (index >= sps.numLongTermRefPicsSps) {
                            return why(rbsp, kWhat, range("lt_idx_sps", index, 0, sps.numLongTermRefPicsSps - 1));
                        }
                        used = sps.usedByCurrPicLtSps[index];
                    } else {
                        bits.skip(sps.log2MaxPocLsb);  // poc_lsb_lt
                        used = bits.flag();             // used_by_curr_pic_lt_flag
                    }
                    if (bits.flag()) {  // delta_poc_msb_present_flag
                        static_cast<void>(bits.ue());
                    }
                    pictures += used ? 1 : 0;
                }
            }
            if (sps.temporalMvp) {
                temporalMvp = bits.flag();
            }
        }
        if (pps.currPicRefEnabled) {
            ++pictures;
        }
        bool saoLuma = false;
        bool saoChroma = false;
        if (sps.sampleAdaptiveOffset) {
            saoLuma = bits.flag();
            if (sps.chromaArrayType() != 0) {
                saoChroma = bits.flag();
            }
        }
        if (sliceType == kP || sliceType == kB) {
            std::array<std::uint32_t, 2> refs = {pps.numRefIdxDefault[0],
                                                 sliceType == kB ? pps.numRefIdxDefault[1] : 0};
            if (bits.flag()) {  // num_ref_idx_active_override_flag
                refs[0] = bits.ue() + 1;
                if (sliceType == kB) {
                    refs[1] = bits.ue() + 1;
                }
            }
            if (refs[0] >= kMaxRefs || refs[1] >= kMaxRefs) {
                return why(rbsp, kWhat,
                           range(refs[0] >= kMaxRefs ? "num_ref_idx_l0_active_minus1" : "num_ref_idx_l1_active_minus1",
                                 std::int64_t{std::max(refs[0], refs[1])} - 1, 0, kMaxRefs - 2));
            }
            if (pictures == 0 && !bits.malformed() && !rbsp.overrun()) {
                return "a P or B slice without reference pictures";
            }
            if (pps.listsModificationPresent && pictures > 1) {
                const unsigned entryBits = nal::ceilLog2(pictures);
                for (std::uint32_t list = 0; list < (sliceType == kB ? 2U : 1U); ++list) {
                    if (!bits.flag()) {  // ref_pic_list_modification_flag_lX
                        continue;
                    }
                    for (std::uint32_t i = 0; i < refs[list]; ++i) {
                        const std::uint32_t entry = bits.read(entryBits);
                        if (entry >= pictures) {
                            return why(rbsp, kWhat, range(list == 0 ? "list_entry_l0" : "list_entry_l1", entry, 0,
                                                          pictures - 1));
                        }
                    }
                }
            }
            if (sliceType == kB) {
                bits.skip(1);  // mvd_l1_zero_flag
            }
            if (pps.cabacInitPresent) {
                bits.skip(1);  // cabac_init_flag
            }
            if (temporalMvp) {
                const std::uint32_t list = sliceType == kB && !bits.flag() ? 1 : 0;  // collocated_from_l0_flag
                if (refs[list] > 1) {
                    const std::uint32_t index = bits.ue();
                    if (index >= refs[list]) {
                        return why(rbsp, kWhat, range("collocated_ref_idx", index, 0, refs[list] - 1));
                    }
                }
            }
            if ((pps.weightedPred && sliceType == kP) || (pps.weightedBipred && sliceType == kB)) {
                const std::uint32_t lumaDenominator = bits.ue();
                if (lumaDenominator > 7) {
                    return why(rbsp, kWhat, range("luma_log2_weight_denom", lumaDenominator, 0, 7));
                }
                const bool chroma = sps.chromaArrayType() != 0;
                if (chroma) {
                    const std::int64_t chromaDenominator = std::int64_t{lumaDenominator} + bits.se();
                    if (chromaDenominator < 0 || chromaDenominator > 7) {
                        return why(rbsp, kWhat, range("ChromaLog2WeightDenom", chromaDenominator, 0, 7));
                    }
                }
                for (std::uint32_t list = 0; list < (sliceType == kB ? 2U : 1U); ++list) {
                    std::array<bool, kMaxRefs> lumaFlags{};
                    std::array<bool, kMaxRefs> chromaFlags{};
                    for (std::uint32_t i = 0; i < refs[list]; ++i) {
                        lumaFlags[i] = bits.flag();
                    }
                    if (chroma) {
                        for (std::uint32_t i = 0; i < refs[list]; ++i) {
                            chromaFlags[i] = bits.flag();
                        }
                    }
                    for (std::uint32_t i = 0; i < refs[list] && !bits.malformed(); ++i) {
                        if (lumaFlags[i]) {
                            const std::int32_t weight = bits.se();
                            static_cast<void>(bits.se());  // luma_offset_lX
                            if (weight < -128 || weight > 127) {
                                return why(rbsp, kWhat, range("delta_luma_weight", weight, -128, 127));
                            }
                        }
                        if (chromaFlags[i]) {
                            for (int component = 0; component < 2; ++component) {
                                const std::int32_t weight = bits.se();
                                const std::int32_t offset = bits.se();
                                if (weight < -128 || weight > 127) {
                                    return why(rbsp, kWhat, range("delta_chroma_weight", weight, -128, 127));
                                }
                                if (offset < -(1 << 17) || offset > (1 << 17)) {
                                    return why(rbsp, kWhat, range("delta_chroma_offset", offset, -(1 << 17), 1 << 17));
                                }
                            }
                        }
                    }
                }
            }
            const std::uint32_t mergeCandidates = bits.ue();
            if (mergeCandidates > 4) {
                return why(rbsp, kWhat, range("five_minus_max_num_merge_cand", mergeCandidates, 0, 4));
            }
            if (sps.motionVectorResolutionControlIdc == 2) {
                bits.skip(1);  // use_integer_mv_flag
            }
        }
        const std::int64_t qp = 26 + std::int64_t{pps.initQpMinus26} + bits.se();
        if (pps.sliceChromaQpOffsetsPresent) {
            const std::int32_t cb = bits.se();
            const std::int32_t cr = bits.se();
            if (cb < -12 || cb > 12 || cr < -12 || cr > 12) {
                return why(rbsp, kWhat, "slice_cb_qp_offset or slice_cr_qp_offset outside -12 to 12");
            }
        }
        if (pps.sliceActQpOffsetsPresent) {
            for (int i = 0; i < 3; ++i) {
                static_cast<void>(bits.se());
            }
        }
        if (pps.chromaQpOffsetListEnabled) {
            bits.skip(1);  // cu_chroma_qp_offset_enabled_flag
        }
        bool deblockingDisabled = false;
        if (pps.deblockingControlPresent) {
            const bool overridden = pps.deblockingOverrideEnabled && bits.flag();
            deblockingDisabled = pps.deblockingDisabled;
            if (overridden) {
                deblockingDisabled = bits.flag();
                if (!deblockingDisabled) {
                    const std::int32_t beta = bits.se();
                    const std::int32_t tc = bits.se();
                    if (beta < -6 || beta > 6 || tc < -6 || tc > 6) {
                        return why(rbsp, kWhat, "slice_beta_offset_div2 or slice_tc_offset_div2 outside -6 to 6");
                    }
                }
            }
        }
        if (pps.loopFilterAcrossSlices && (saoLuma || saoChroma || !deblockingDisabled)) {
            bits.skip(1);  // slice_loop_filter_across_slices_enabled_flag
        }
        const std::int64_t lowestQp = -6 * (std::int64_t{sps.bitDepthLuma} - 8);
        if ((qp < lowestQp || qp > 51) && !rbsp.overrun() && !bits.malformed()) {
            return range("SliceQpY", qp, lowestQp, 51);
        }
    }
    std::uint64_t entryBytes = 0;
    if (pps.tilesEnabled || pps.entropyCodingSync) {
        const std::uint32_t entries = bits.ue();
        const std::uint64_t ctbRows = sps.ctbHeight();
        const std::uint64_t maxEntries = pps.tilesEnabled && pps.entropyCodingSync
                                             ? std::uint64_t{pps.tileColumns} * ctbRows - 1
                                         : pps.tilesEnabled ? std::uint64_t{pps.tileColumns} * pps.tileRows - 1
                                                            : ctbRows - 1;
        if (entries > maxEntries) {
            return why(rbsp, kWhat,
                       range("num_entry_point_offsets", entries, 0, static_cast<std::int64_t>(maxEntries)));
        }
        if (entries > 0) {
            const std::uint32_t lengthMinus1 = bits.ue();
            if (lengthMinus1 > 31) {
                return why(rbsp, kWhat, range("offset_len_minus1", lengthMinus1, 0, 31));
            }
            for (std::uint32_t i = 0; i < entries && !bits.overrun(); ++i) {
                entryBytes += std::uint64_t{bits.read(lengthMinus1 + 1)} + 1;
            }
        }
    }
    if (pps.sliceHeaderExtensionPresent) {
        const std::uint32_t length = bits.ue();
        if (length > 256) {
            return why(rbsp, kWhat, range("slice_segment_header_extension_length", length, 0, 256));
        }
        bits.skip(std::uint64_t{8} * length);
    }
    if (rbsp.overrun() || bits.malformed()) {
        if (!complete && bits.overrun()) {
            return "the slice segment header is longer than " + std::to_string(kSliceHead / 1024) + " KiB";
        }
        return why(rbsp, kWhat, {});
    }
    // byte_alignment()
    if (!bits.flag()) {
        return "alignment_bit_equal_to_one is 0";
    }
    while (!bits.byteAligned()) {
        if (bits.flag()) {
            return "an alignment_bit_equal_to_zero of 1";
        }
    }
    if (rbsp.overrun()) {
        return "the slice segment header runs into the end of the NAL unit";
    }
    const std::uint64_t dataStart = rbsp.unitOffset(bits.position() / 8);
    if (entryBytes > 0 && entryBytes >= size - std::min(size, dataStart)) {
        return "entry points " + std::to_string(entryBytes) +
               " bytes into slice segment data of " + std::to_string(size - std::min(size, dataStart)) + " bytes";
    }
    return std::nullopt;
}

std::string Stream::summary() const {
    std::string text = std::to_string(vpsCount_) + " VPS, " + std::to_string(spsCount_) + " SPS, " +
                       std::to_string(ppsCount_) + " PPS, " + std::to_string(slices_) +
                       (slices_ == 1 ? " slice segment" : " slice segments") + " (headers read";
    if (unreadSlices_ > 0) {
        text += " but " + std::to_string(unreadSlices_) + " with multilayer or 3D extensions";
    }
    text += ")";
    if (otherLayers_ > 0) {
        text += ", " + std::to_string(otherLayers_) + " NAL units of other layers (not read)";
    }
    return text;
}

std::string profileName(std::uint32_t profileIdc) {
    switch (profileIdc) {
    case 1:
        return "Main";
    case 2:
        return "Main 10";
    case 3:
        return "Main Still Picture";
    case 4:
        return "Range Extensions";
    case 5:
        return "High Throughput";
    case 9:
        return "Screen Content Coding";
    default:
        return "profile " + std::to_string(profileIdc);
    }
}

}  // namespace recovery::validation::detail::hevc
