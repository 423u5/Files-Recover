#include "avc_syntax.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace recovery::validation::detail::avc {

namespace {

using Problem = std::optional<std::string>;

constexpr std::uint32_t kMaxRefFrames = 16;
// FFmpeg's MAX_MMCO_COUNT: a slice with more fails.
constexpr int kMaxMemoryOperations = 66;

std::string range(std::string_view field, std::int64_t value, std::int64_t low, std::int64_t high) {
    return std::string(field) + " " + std::to_string(value) + " (" + std::to_string(low) + " to " +
           std::to_string(high) + ")";
}

// What a field that failed a check really shows: the end of the syntax, an
// Exp-Golomb code too long to read, or the check's own detail.
std::string why(nal::Rbsp& rbsp, std::string_view what, std::string detail) {
    if (rbsp.overrun()) {
        return std::string(what) + " ends inside its fields";
    }
    if (rbsp.bits().malformed()) {
        return std::string(what) + " holds an Exp-Golomb code of more than 32 bits";
    }
    return detail;
}

bool highProfile(std::uint32_t profile) noexcept {
    switch (profile) {
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135:
    case 144:
        return true;
    default:
        return false;
    }
}

// scaling_list() (7.3.2.1.1.1).
Problem scalingList(nal::Rbsp& rbsp, std::string_view what, int size) {
    MsbBits& bits = rbsp.bits();
    int last = 8;
    int next = 8;
    for (int j = 0; j < size; ++j) {
        if (next != 0) {
            const std::int32_t delta = bits.se();
            if (delta < -128 || delta > 127) {
                return why(rbsp, what, range("delta_scale", delta, -128, 127));
            }
            next = (last + delta + 256) % 256;
        }
        last = next == 0 ? last : next;
    }
    return std::nullopt;
}

// hrd_parameters() (E.1.2).
Problem hrdParameters(nal::Rbsp& rbsp) {
    MsbBits& bits = rbsp.bits();
    const std::uint32_t cpbCountMinus1 = bits.ue();
    if (cpbCountMinus1 > 31) {
        return why(rbsp, "the VUI", range("cpb_cnt_minus1", cpbCountMinus1, 0, 31));
    }
    bits.skip(4 + 4);  // bit_rate_scale, cpb_size_scale
    for (std::uint32_t i = 0; i <= cpbCountMinus1 && !bits.malformed(); ++i) {
        static_cast<void>(bits.ue());  // bit_rate_value_minus1
        static_cast<void>(bits.ue());  // cpb_size_value_minus1
        bits.skip(1);                  // cbr_flag
    }
    bits.skip(5 + 5 + 5 + 5);  // the delay and offset lengths
    return std::nullopt;
}

// vui_parameters() (E.1.1). A VUI that ends early is accepted (FFmpeg
// accepts it); only what FFmpeg rejects in a VUI it could read fails.
Problem vuiParameters(nal::Rbsp& rbsp, Sps& sps) {
    MsbBits& bits = rbsp.bits();
    if (bits.flag()) {  // aspect_ratio_info_present_flag
        if (bits.read(8) == 255) {
            bits.skip(16 + 16);  // sar_width, sar_height
        }
    }
    if (bits.flag()) {  // overscan_info_present_flag
        bits.skip(1);
    }
    if (bits.flag()) {  // video_signal_type_present_flag
        bits.skip(3 + 1);
        if (bits.flag()) {  // colour_description_present_flag
            bits.skip(8 + 8 + 8);
        }
    }
    if (bits.flag()) {  // chroma_loc_info_present_flag
        static_cast<void>(bits.ue());
        static_cast<void>(bits.ue());
    }
    if (bits.flag()) {  // timing_info_present_flag
        bits.skip(32 + 32 + 1);
    }
    const bool nalHrd = bits.flag();
    if (nalHrd) {
        if (Problem problem = hrdParameters(rbsp); problem && !rbsp.overrun()) {
            return problem;
        }
    }
    const bool vclHrd = bits.flag();
    if (vclHrd) {
        if (Problem problem = hrdParameters(rbsp); problem && !rbsp.overrun()) {
            return problem;
        }
    }
    if (nalHrd || vclHrd) {
        bits.skip(1);  // low_delay_hrd_flag
    }
    bits.skip(1);       // pic_struct_present_flag
    if (bits.flag()) {  // bitstream_restriction_flag
        bits.skip(1);   // motion_vectors_over_pic_boundaries_flag
        for (int i = 0; i < 4; ++i) {
            static_cast<void>(bits.ue());  // max_bytes_per_pic_denom ... log2_max_mv_length_vertical
        }
        const std::uint32_t reorder = bits.ue();
        static_cast<void>(bits.ue());  // max_dec_frame_buffering
        if (reorder > 16 && !rbsp.overrun() && !bits.malformed()) {
            return range("max_num_reorder_frames", reorder, 0, 16);
        }
    }
    sps.vuiCut = rbsp.overrun() || bits.malformed();
    return std::nullopt;
}

Problem readSps(nal::Rbsp& rbsp, Sps& sps) {
    MsbBits& bits = rbsp.bits();
    constexpr std::string_view kWhat = "the SPS";
    sps.profileIdc = bits.read(8);
    sps.constraintFlags = bits.read(6);
    bits.skip(2);  // reserved_zero_2bits
    sps.levelIdc = bits.read(8);
    sps.id = bits.ue();
    if (sps.id > 31) {
        return why(rbsp, kWhat, range("seq_parameter_set_id", sps.id, 0, 31));
    }
    if (highProfile(sps.profileIdc)) {
        sps.chromaFormatIdc = bits.ue();
        if (sps.chromaFormatIdc > 3) {
            return why(rbsp, kWhat, range("chroma_format_idc", sps.chromaFormatIdc, 0, 3));
        }
        if (sps.chromaFormatIdc == 3) {
            sps.separateColourPlane = bits.flag();
        }
        const std::uint32_t lumaMinus8 = bits.ue();
        const std::uint32_t chromaMinus8 = bits.ue();
        if (lumaMinus8 > 6) {
            return why(rbsp, kWhat, range("bit_depth_luma_minus8", lumaMinus8, 0, 6));
        }
        if (chromaMinus8 > 6) {
            return why(rbsp, kWhat, range("bit_depth_chroma_minus8", chromaMinus8, 0, 6));
        }
        sps.bitDepthLuma = 8 + lumaMinus8;
        sps.bitDepthChroma = 8 + chromaMinus8;
        bits.skip(1);       // qpprime_y_zero_transform_bypass_flag
        if (bits.flag()) {  // seq_scaling_matrix_present_flag
            const int lists = sps.chromaFormatIdc != 3 ? 8 : 12;
            for (int i = 0; i < lists; ++i) {
                if (bits.flag()) {
                    if (Problem problem = scalingList(rbsp, kWhat, i < 6 ? 16 : 64)) {
                        return problem;
                    }
                }
            }
        }
    }
    const std::uint32_t frameNumMinus4 = bits.ue();
    if (frameNumMinus4 > 12) {
        return why(rbsp, kWhat, range("log2_max_frame_num_minus4", frameNumMinus4, 0, 12));
    }
    sps.log2MaxFrameNum = frameNumMinus4 + 4;
    sps.pocType = bits.ue();
    if (sps.pocType == 0) {
        const std::uint32_t lsbMinus4 = bits.ue();
        if (lsbMinus4 > 12) {
            return why(rbsp, kWhat, range("log2_max_pic_order_cnt_lsb_minus4", lsbMinus4, 0, 12));
        }
        sps.log2MaxPocLsb = lsbMinus4 + 4;
    } else if (sps.pocType == 1) {
        sps.deltaPicOrderAlwaysZero = bits.flag();
        static_cast<void>(bits.se());  // offset_for_non_ref_pic
        static_cast<void>(bits.se());  // offset_for_top_to_bottom_field
        const std::uint32_t cycle = bits.ue();
        if (cycle > 255) {
            return why(rbsp, kWhat, range("num_ref_frames_in_pic_order_cnt_cycle", cycle, 0, 255));
        }
        for (std::uint32_t i = 0; i < cycle && !bits.malformed(); ++i) {
            static_cast<void>(bits.se());  // offset_for_ref_frame
        }
    } else if (sps.pocType != 2) {
        return why(rbsp, kWhat, range("pic_order_cnt_type", sps.pocType, 0, 2));
    }
    const std::uint32_t refFrames = bits.ue();
    if (refFrames > kMaxRefFrames) {
        return why(rbsp, kWhat, range("max_num_ref_frames", refFrames, 0, kMaxRefFrames));
    }
    bits.skip(1);  // gaps_in_frame_num_value_allowed_flag
    const std::uint32_t widthMinus1 = bits.ue();
    const std::uint32_t heightMinus1 = bits.ue();
    sps.frameMbsOnly = bits.flag();
    if (!sps.frameMbsOnly) {
        sps.mbAdaptiveFrameField = bits.flag();
    }
    sps.direct8x8Inference = bits.flag();
    if (rbsp.overrun() || bits.malformed()) {
        return why(rbsp, kWhat, {});
    }
    sps.widthInMbs = widthMinus1 + 1;
    sps.heightInMapUnits = heightMinus1 + 1;
    // FFmpeg's bounds (av_image_check_size): (w + 128) * (h + 128) < INT_MAX / 8.
    const std::uint64_t width = std::uint64_t{16} * (std::uint64_t{widthMinus1} + 1);
    const std::uint64_t height = std::uint64_t{16} * (sps.frameMbsOnly ? 1U : 2U) * (std::uint64_t{heightMinus1} + 1);
    if (width >= 0x7FFFFFFF || height >= 0x7FFFFFFF || (width + 128) * (height + 128) >= 0x7FFFFFFF / 8) {
        return "a picture of " + std::to_string(width) + "x" + std::to_string(height) + " samples";
    }
    if (bits.flag()) {  // frame_cropping_flag
        std::array<std::uint64_t, 4> crop{};
        for (std::uint64_t& value : crop) {
            value = bits.ue();
        }
        const std::uint64_t stepX = sps.chromaArrayType() == 1 || sps.chromaArrayType() == 2 ? 2 : 1;
        const std::uint64_t stepY = (sps.frameMbsOnly ? 1U : 2U) * (sps.chromaArrayType() == 1 ? 2U : 1U);
        if (!rbsp.overrun() && !bits.malformed() &&
            ((crop[0] + crop[1]) * stepX >= width || (crop[2] + crop[3]) * stepY >= height)) {
            return "a cropping window that leaves no picture";
        }
    }
    const bool vui = bits.flag();
    if (rbsp.overrun() || bits.malformed()) {
        return why(rbsp, kWhat, {});
    }
    if (vui) {
        return vuiParameters(rbsp, sps);
    }
    return std::nullopt;
}

Problem readPps(nal::Rbsp& rbsp, const ParameterSets& sets, Pps& pps) {
    MsbBits& bits = rbsp.bits();
    constexpr std::string_view kWhat = "the PPS";
    pps.id = bits.ue();
    if (pps.id > 255) {
        return why(rbsp, kWhat, range("pic_parameter_set_id", pps.id, 0, 255));
    }
    pps.spsId = bits.ue();
    if (pps.spsId > 31) {
        return why(rbsp, kWhat, range("seq_parameter_set_id", pps.spsId, 0, 31));
    }
    if (!sets.sps[pps.spsId].has_value()) {
        return why(rbsp, kWhat, "it refers to SPS " + std::to_string(pps.spsId) + ", which has not been sent");
    }
    const Sps& sps = *sets.sps[pps.spsId];
    pps.entropyCodingMode = bits.flag();
    pps.bottomFieldPicOrderInFramePresent = bits.flag();
    const std::uint32_t groupsMinus1 = bits.ue();
    if (groupsMinus1 > 7) {
        return why(rbsp, kWhat, range("num_slice_groups_minus1", groupsMinus1, 0, 7));
    }
    pps.numSliceGroups = groupsMinus1 + 1;
    if (groupsMinus1 > 0) {
        pps.sliceGroupMapType = bits.ue();
        switch (pps.sliceGroupMapType) {
        case 0:
            for (std::uint32_t group = 0; group <= groupsMinus1; ++group) {
                static_cast<void>(bits.ue());  // run_length_minus1
            }
            break;
        case 2:
            for (std::uint32_t group = 0; group < groupsMinus1; ++group) {
                static_cast<void>(bits.ue());  // top_left
                static_cast<void>(bits.ue());  // bottom_right
            }
            break;
        case 3:
        case 4:
        case 5: {
            bits.skip(1);  // slice_group_change_direction_flag
            const std::uint32_t rateMinus1 = bits.ue();
            const std::uint64_t mapUnits = std::uint64_t{sps.widthInMbs} * sps.heightInMapUnits;
            if (rateMinus1 >= mapUnits) {
                return why(rbsp, kWhat, range("slice_group_change_rate_minus1", rateMinus1, 0, mapUnits - 1));
            }
            pps.sliceGroupChangeRate = rateMinus1 + 1;
            break;
        }
        case 6: {
            const std::uint32_t unitsMinus1 = bits.ue();
            const std::uint64_t mapUnits = std::uint64_t{sps.widthInMbs} * sps.heightInMapUnits;
            if (std::uint64_t{unitsMinus1} + 1 != mapUnits) {
                return why(rbsp, kWhat,
                           "pic_size_in_map_units_minus1 " + std::to_string(unitsMinus1) + " for " +
                               std::to_string(mapUnits) + " map units");
            }
            const std::uint64_t idBits = nal::ceilLog2(pps.numSliceGroups);
            if (idBits * mapUnits > bits.left()) {
                return std::string(kWhat) + " ends inside its fields";
            }
            bits.skip(idBits * mapUnits);  // slice_group_id
            break;
        }
        case 1:
            break;
        default:
            return why(rbsp, kWhat, range("slice_group_map_type", pps.sliceGroupMapType, 0, 6));
        }
    }
    for (std::uint32_t list = 0; list < 2; ++list) {
        const std::uint32_t minus1 = bits.ue();
        if (minus1 > 31) {
            const char* field =
                list == 0 ? "num_ref_idx_l0_default_active_minus1" : "num_ref_idx_l1_default_active_minus1";
            return why(rbsp, kWhat, range(field, minus1, 0, 31));
        }
        pps.numRefIdxDefault[list] = minus1 + 1;
    }
    pps.weightedPred = bits.flag();
    pps.weightedBipredIdc = bits.read(2);
    pps.picInitQpMinus26 = bits.se();
    static_cast<void>(bits.se());  // pic_init_qs_minus26
    const std::int32_t chromaOffset = bits.se();
    if (chromaOffset < -12 || chromaOffset > 12) {
        return why(rbsp, kWhat, range("chroma_qp_index_offset", chromaOffset, -12, 12));
    }
    pps.deblockingFilterControlPresent = bits.flag();
    bits.skip(1);  // constrained_intra_pred_flag
    pps.redundantPicCntPresent = bits.flag();
    if (rbsp.overrun() || bits.malformed()) {
        return why(rbsp, kWhat, {});
    }
    // FFmpeg ignores what follows in Baseline, Main and Extended streams
    // with constraint_set0, 1 or 2 (encoders wrote junk there).
    const bool ignoreRest = (sps.profileIdc == 66 || sps.profileIdc == 77 || sps.profileIdc == 88) &&
                            (sps.constraintFlags & 0x38U) != 0;
    if (rbsp.moreData() && !ignoreRest) {
        pps.transform8x8Mode = bits.flag();
        if (bits.flag()) {  // pic_scaling_matrix_present_flag
            const int lists = 6 + (pps.transform8x8Mode ? (sps.chromaFormatIdc != 3 ? 2 : 6) : 0);
            for (int i = 0; i < lists; ++i) {
                if (bits.flag()) {
                    if (Problem problem = scalingList(rbsp, kWhat, i < 6 ? 16 : 64)) {
                        return problem;
                    }
                }
            }
        }
        const std::int32_t secondOffset = bits.se();
        if (secondOffset < -12 || secondOffset > 12) {
            return why(rbsp, kWhat, range("second_chroma_qp_index_offset", secondOffset, -12, 12));
        }
        if (rbsp.overrun() || bits.malformed()) {
            return why(rbsp, kWhat, {});
        }
    }
    return std::nullopt;
}

// Slice types by slice_type % 5.
constexpr std::uint32_t kP = 0;
constexpr std::uint32_t kB = 1;
constexpr std::uint32_t kI = 2;
constexpr std::uint32_t kSp = 3;
constexpr std::uint32_t kSi = 4;

const char* sliceTypeName(std::uint32_t type) {
    static constexpr std::array<const char*, 5> kNames = {"P", "B", "I", "SP", "SI"};
    return kNames[type % 5];
}

}  // namespace

ConfigRead readDecoderConfig(std::span<const std::uint8_t> payload) {
    ConfigRead read;
    if (payload.size() < 7) {
        read.problem = "the avcC box holds " + std::to_string(payload.size()) + " bytes, fewer than its 7 fixed ones";
        return read;
    }
    if (payload[0] != 1) {
        read.unsupported = true;
        read.problem = "avcC configurationVersion " + std::to_string(payload[0]) + " (only 1 is defined)";
        return read;
    }
    DecoderConfig config;
    config.profileIdc = payload[1];
    config.levelIdc = payload[3];
    config.lengthSize = (payload[4] & 3U) + 1;
    if (config.lengthSize == 3) {
        read.problem = "avcC lengthSizeMinusOne 2 (NAL unit lengths of 3 bytes are not allowed)";
        return read;
    }
    std::size_t position = 5;
    for (int list = 0; list < 2; ++list) {
        if (position >= payload.size()) {
            read.problem = "the avcC box ends before its picture parameter sets";
            return read;
        }
        const std::size_t count = list == 0 ? (payload[position] & 0x1FU) : payload[position];
        ++position;
        for (std::size_t i = 0; i < count; ++i) {
            if (payload.size() - position < 2) {
                read.problem = "the avcC box ends inside its parameter sets";
                return read;
            }
            const std::size_t length = (std::size_t{payload[position]} << 8) | payload[position + 1];
            position += 2;
            if (length == 0 || payload.size() - position < length) {
                read.problem = length == 0 ? "the avcC box holds an empty parameter set"
                                           : "the avcC box ends inside its parameter sets";
                return read;
            }
            config.parameterSets.emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(position),
                                              payload.begin() + static_cast<std::ptrdiff_t>(position + length));
            position += length;
        }
    }
    // What follows (the chroma format and bit depths of High profiles, and
    // SPS extensions) is not needed; FFmpeg does not read it either.
    read.config = std::move(config);
    return read;
}

std::optional<std::string> Stream::configure(const DecoderConfig& config) {
    std::size_t index = 0;
    for (const std::vector<std::uint8_t>& unit : config.parameterSets) {
        ++index;
        nal::SequenceScan scan;
        scan.feed(unit, 0);
        const std::string where = "parameter set " + std::to_string(index) + " of the avcC box (" + kindOf(unit[0]) +
                                  "): ";
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
    switch (header & 0x1FU) {
    case kSps:
    case kPps:
        return std::min<std::uint64_t>(size, kMaxParameterSet + 1);
    case kSlice:
    case kIdrSlice:
        return std::min<std::uint64_t>(size, kSliceHead);
    default:
        return std::min<std::uint64_t>(size, 1);
    }
}

std::string Stream::kindOf(std::uint8_t header) {
    switch (header & 0x1FU) {
    case kSlice:
        return "slice";
    case kPartitionA:
        return "data partition A";
    case 3:
        return "data partition B";
    case kPartitionC:
        return "data partition C";
    case kIdrSlice:
        return "IDR slice";
    case 6:
        return "SEI";
    case kSps:
        return "SPS";
    case kPps:
        return "PPS";
    case 9:
        return "access unit delimiter";
    case 10:
        return "end of sequence";
    case 11:
        return "end of stream";
    case 12:
        return "filler data";
    case 13:
        return "SPS extension";
    case 14:
        return "prefix";
    case 15:
        return "subset SPS";
    case 20:
        return "slice extension";
    default:
        return "type " + std::to_string(header & 0x1FU);
    }
}

std::optional<std::string> Stream::nalUnit(std::span<const std::uint8_t> head, std::uint64_t size) {
    if (head.empty() || size == 0) {
        return "an empty NAL unit";
    }
    const std::uint8_t header = head[0];
    if ((header & 0x80U) != 0) {
        return "forbidden_zero_bit is set";
    }
    const std::uint32_t refIdc = (header >> 5) & 3U;
    const std::uint32_t type = header & 0x1FU;
    const bool complete = head.size() == size;
    switch (type) {
    case kSps:
        ++spsCount_;
        if (!complete) {
            return "an SPS of " + std::to_string(size) + " bytes (at most 64 KiB are read)";
        }
        return sps(head);
    case kPps:
        ++ppsCount_;
        if (!complete) {
            return "a PPS of " + std::to_string(size) + " bytes (at most 64 KiB are read)";
        }
        return pps(head);
    case kSlice:
    case kIdrSlice:
        ++slices_;
        return slice(head, complete, type, refIdc);
    default:
        if (type >= kPartitionA && type <= kPartitionC) {
            ++partitions_;
        } else {
            ++others_;
        }
        return std::nullopt;
    }
}

std::optional<std::string> Stream::sps(std::span<const std::uint8_t> unit) {
    nal::Rbsp rbsp(unit, 1, true);
    Sps parsed;
    if (Problem problem = readSps(rbsp, parsed)) {
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
    sets_.sps[parsed.id] = parsed;
    sets_.spsBytes[parsed.id] = std::move(bytes);
    if (!firstSps_.has_value()) {
        firstSps_ = parsed;
    }
    return std::nullopt;
}

std::optional<std::string> Stream::pps(std::span<const std::uint8_t> unit) {
    nal::Rbsp rbsp(unit, 1, true);
    Pps parsed;
    if (Problem problem = readPps(rbsp, sets_, parsed)) {
        return problem;
    }
    sets_.pps[parsed.id] = parsed;
    return std::nullopt;
}

std::optional<std::string> Stream::slice(std::span<const std::uint8_t> head, bool complete, std::uint32_t type,
                                         std::uint32_t refIdc) {
    nal::Rbsp rbsp(head, 1, complete);
    MsbBits& bits = rbsp.bits();
    constexpr std::string_view kWhat = "the slice header";
    const std::uint32_t firstMb = bits.ue();
    const std::uint32_t sliceType = bits.ue();
    if (sliceType > 9) {
        return why(rbsp, kWhat, range("slice_type", sliceType, 0, 9));
    }
    const std::uint32_t kind = sliceType % 5;
    if (type == kIdrSlice && kind != kI && kind != kSi) {
        return why(rbsp, kWhat, std::string("a slice of type ") + sliceTypeName(kind) + " in an IDR picture");
    }
    const std::uint32_t ppsId = bits.ue();
    if (ppsId > 255) {
        return why(rbsp, kWhat, range("pic_parameter_set_id", ppsId, 0, 255));
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
    if (sps.separateColourPlane) {
        bits.skip(2);  // colour_plane_id
    }
    bits.skip(sps.log2MaxFrameNum);  // frame_num
    bool field = false;
    if (!sps.frameMbsOnly) {
        field = bits.flag();
        if (field) {
            bits.skip(1);  // bottom_field_flag
        }
        if (!sps.direct8x8Inference && kind == kB) {
            return why(rbsp, kWhat, "a B slice of an interlaced stream whose SPS clears direct_8x8_inference_flag");
        }
    }
    const std::uint64_t frameMbs = std::uint64_t{sps.widthInMbs} * sps.frameHeightInMbs();
    const bool fieldOrMbaff = field || sps.mbAdaptiveFrameField;
    if ((std::uint64_t{firstMb} << (fieldOrMbaff ? 1 : 0)) >= frameMbs && !bits.malformed()) {
        return why(rbsp, kWhat,
                   "first_mb_in_slice " + std::to_string(firstMb) + " beyond the picture's " +
                       std::to_string(fieldOrMbaff ? frameMbs / 2 : frameMbs) + " macroblocks");
    }
    if (type == kIdrSlice) {
        static_cast<void>(bits.ue());  // idr_pic_id
    }
    if (sps.pocType == 0) {
        bits.skip(sps.log2MaxPocLsb);  // pic_order_cnt_lsb
        if (pps.bottomFieldPicOrderInFramePresent && !field) {
            static_cast<void>(bits.se());  // delta_pic_order_cnt_bottom
        }
    }
    if (sps.pocType == 1 && !sps.deltaPicOrderAlwaysZero) {
        static_cast<void>(bits.se());  // delta_pic_order_cnt[0]
        if (pps.bottomFieldPicOrderInFramePresent && !field) {
            static_cast<void>(bits.se());  // delta_pic_order_cnt[1]
        }
    }
    if (pps.redundantPicCntPresent) {
        static_cast<void>(bits.ue());  // redundant_pic_cnt
    }
    if (kind == kB) {
        bits.skip(1);  // direct_spatial_mv_pred_flag
    }
    std::array<std::uint32_t, 2> refs = pps.numRefIdxDefault;
    const std::uint32_t lists = kind == kB ? 2 : kind == kP || kind == kSp ? 1 : 0;
    if (lists > 0) {
        if (bits.flag()) {  // num_ref_idx_active_override_flag
            refs[0] = bits.ue() + 1;
            if (kind == kB) {
                refs[1] = bits.ue() + 1;
            }
        }
        const std::uint32_t maxRefs = field ? 32 : 16;
        for (std::uint32_t list = 0; list < lists; ++list) {
            if (refs[list] == 0 || refs[list] > maxRefs) {
                return why(rbsp, kWhat,
                           range(list == 0 ? "num_ref_idx_l0_active_minus1" : "num_ref_idx_l1_active_minus1",
                                 std::int64_t{refs[list]} - 1, 0, maxRefs - 1));
            }
        }
    }
    // ref_pic_list_modification()
    const std::uint64_t maxPicNum = std::uint64_t{1} << (sps.log2MaxFrameNum + (field ? 1 : 0));
    for (std::uint32_t list = 0; list < lists; ++list) {
        if (!bits.flag()) {
            continue;
        }
        for (std::uint32_t index = 0;; ++index) {
            const std::uint32_t operation = bits.ue();
            if (operation == 3 || bits.malformed()) {
                break;
            }
            if (index >= refs[list]) {
                return why(rbsp, kWhat, "more reference list modifications than the list's " +
                                            std::to_string(refs[list]) + " entries");
            }
            if (operation > 3) {
                return why(rbsp, kWhat, range("modification_of_pic_nums_idc", operation, 0, 3));
            }
            const std::uint32_t argument = bits.ue();
            if (operation < 2 && std::uint64_t{argument} + 1 > maxPicNum) {
                return why(rbsp, kWhat, range("abs_diff_pic_num_minus1", argument, 0, maxPicNum - 1));
            }
            if (operation == 2 && (field ? argument >> 1 : argument) > 31) {
                return why(rbsp, kWhat, range("long_term_pic_num", argument, 0, field ? 63 : 31));
            }
        }
    }
    // pred_weight_table()
    if ((pps.weightedPred && (kind == kP || kind == kSp)) || (pps.weightedBipredIdc == 1 && kind == kB)) {
        static_cast<void>(bits.ue());  // luma_log2_weight_denom (FFmpeg corrects values above 7)
        const bool chroma = sps.chromaArrayType() != 0;
        if (chroma) {
            static_cast<void>(bits.ue());  // chroma_log2_weight_denom
        }
        for (std::uint32_t list = 0; list < lists; ++list) {
            for (std::uint32_t i = 0; i < refs[list] && !bits.malformed(); ++i) {
                if (bits.flag()) {  // luma_weight_lX_flag
                    const std::int32_t weight = bits.se();
                    const std::int32_t offset = bits.se();
                    if (weight < -128 || weight > 127 || offset < -128 || offset > 127) {
                        return why(rbsp, kWhat, "a luma weight or offset outside -128 to 127");
                    }
                }
                if (chroma && bits.flag()) {
                    for (int component = 0; component < 2; ++component) {
                        const std::int32_t weight = bits.se();
                        const std::int32_t offset = bits.se();
                        if (weight < -128 || weight > 127 || offset < -128 || offset > 127) {
                            return why(rbsp, kWhat, "a chroma weight or offset outside -128 to 127");
                        }
                    }
                }
            }
        }
    }
    // dec_ref_pic_marking()
    if (refIdc != 0) {
        if (type == kIdrSlice) {
            bits.skip(2);  // no_output_of_prior_pics_flag, long_term_reference_flag
        } else if (bits.flag()) {  // adaptive_ref_pic_marking_mode_flag
            int operations = 0;
            for (;;) {
                const std::uint32_t operation = bits.ue();
                if (operation == 0 || bits.malformed()) {
                    break;
                }
                if (++operations > kMaxMemoryOperations) {
                    return why(rbsp, kWhat, "more than " + std::to_string(kMaxMemoryOperations) +
                                                " memory management control operations");
                }
                if (operation > 6) {
                    return why(rbsp, kWhat, range("memory_management_control_operation", operation, 0, 6));
                }
                if (operation == 1 || operation == 3) {
                    static_cast<void>(bits.ue());  // difference_of_pic_nums_minus1
                }
                if (operation == 2 || operation == 3 || operation == 4 || operation == 6) {
                    const std::uint32_t argument = bits.ue();
                    if (argument >= 32 ||
                        (argument >= 16 && !(operation == 4 && argument == 16) && !(operation == 2 && field))) {
                        return why(rbsp, kWhat,
                                   "the long-term index " + std::to_string(argument) +
                                       " of memory_management_control_operation " + std::to_string(operation));
                    }
                }
            }
        }
    }
    if (pps.entropyCodingMode && kind != kI && kind != kSi) {
        const std::uint32_t init = bits.ue();
        if (init > 2) {
            return why(rbsp, kWhat, range("cabac_init_idc", init, 0, 2));
        }
    }
    const std::int64_t qp = 26 + std::int64_t{pps.picInitQpMinus26} + bits.se();
    const std::int64_t lowestQp = -6 * (std::int64_t{sps.bitDepthLuma} - 8);
    if (qp < lowestQp || qp > 51) {
        return why(rbsp, kWhat, range("the slice QP", qp, lowestQp, 51));
    }
    if (kind == kSp || kind == kSi) {
        if (kind == kSp) {
            bits.skip(1);  // sp_for_switch_flag
        }
        static_cast<void>(bits.se());  // slice_qs_delta
    }
    if (pps.deblockingFilterControlPresent) {
        const std::uint32_t filter = bits.ue();
        if (filter > 2) {
            return why(rbsp, kWhat, range("disable_deblocking_filter_idc", filter, 0, 2));
        }
        if (filter != 1) {
            const std::int32_t alpha = bits.se();
            const std::int32_t beta = bits.se();
            if (alpha < -6 || alpha > 6) {
                return why(rbsp, kWhat, range("slice_alpha_c0_offset_div2", alpha, -6, 6));
            }
            if (beta < -6 || beta > 6) {
                return why(rbsp, kWhat, range("slice_beta_offset_div2", beta, -6, 6));
            }
        }
    }
    if (pps.numSliceGroups > 1 && pps.sliceGroupMapType >= 3 && pps.sliceGroupMapType <= 5) {
        // Ceil(Log2(PicSizeInMapUnits / SliceGroupChangeRate + 1)) bits.
        const std::uint64_t mapUnits = std::uint64_t{sps.widthInMbs} * sps.heightInMapUnits;
        unsigned cycleBits = 0;
        while (((std::uint64_t{1} << cycleBits) - 1) * pps.sliceGroupChangeRate < mapUnits) {
            ++cycleBits;
        }
        bits.skip(cycleBits);  // slice_group_change_cycle
    }
    if (rbsp.overrun() || bits.malformed()) {
        if (!rbsp.complete() && bits.overrun()) {
            return "the slice header is longer than " + std::to_string(kSliceHead / 1024) + " KiB";
        }
        return why(rbsp, "the slice header", {});
    }
    if (pps.entropyCodingMode) {
        ++cabacSlices_;
        while (!bits.byteAligned()) {
            if (!bits.flag()) {
                return "a cabac_alignment_one_bit of 0 after the slice header";
            }
        }
        if (rbsp.overrun()) {
            return "the slice header runs into the end of the NAL unit";
        }
    }
    return std::nullopt;
}

std::string Stream::summary() const {
    std::string text = std::to_string(spsCount_) + " SPS, " + std::to_string(ppsCount_) + " PPS, " +
                       std::to_string(slices_) + (slices_ == 1 ? " slice" : " slices") + " (headers read";
    if (cabacSlices_ > 0) {
        text += "; " + std::to_string(cabacSlices_) + " CABAC";
    }
    text += ")";
    if (partitions_ > 0) {
        text += ", " + std::to_string(partitions_) + " data partitions (not read)";
    }
    return text;
}

std::string profileName(std::uint32_t profileIdc) {
    switch (profileIdc) {
    case 66:
        return "Baseline";
    case 77:
        return "Main";
    case 88:
        return "Extended";
    case 100:
        return "High";
    case 110:
        return "High 10";
    case 122:
        return "High 4:2:2";
    case 244:
        return "High 4:4:4 Predictive";
    case 44:
        return "CAVLC 4:4:4 Intra";
    default:
        return "profile " + std::to_string(profileIdc);
    }
}

}  // namespace recovery::validation::detail::avc
