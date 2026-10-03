#pragma once

// The syntax of AVC (ITU-T H.264) that can be read without decoding the
// coded pictures, for the media checks of MP4 video tracks (mp4_media.cpp):
//
//  * the AVCDecoderConfigurationRecord of an avcC box (ISO/IEC 14496-15
//    5.3.3) and the parameter sets it carries;
//  * sequence parameter sets (7.3.2.1): every field up to and with the VUI;
//  * picture parameter sets (7.3.2.2), slice groups included;
//  * the slice header of every coded slice (7.3.3): reference list
//    modification, prediction weights, reference picture marking, and with
//    CABAC the cabac_alignment_one_bits after it. Slice data is not decoded.
//
// A header fails where the specification forbids what it holds and FFmpeg's
// decoder (6.1) rejects it too, and where the syntax cannot be followed (a
// field beyond the end of the NAL unit, a value that selects no branch).
// What FFmpeg logs and corrects is accepted: weight denominators above 7,
// unknown aspect ratio indexes, a VUI cut short (FFmpeg retries an SPS
// without the end of its VUI). Coding features FFmpeg lacks but the
// specification has (separate colour planes, different luma and chroma bit
// depths, slice groups) are read as the specification says.
//
// A NAL unit's bytes (avc::Stream::nalUnit) begin with the one-byte NAL unit
// header; the forbidden byte sequences are nal::SequenceScan's.

#include "nal_units.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace recovery::validation::detail::avc {

// nal_unit_type values.
inline constexpr std::uint32_t kSlice = 1;
inline constexpr std::uint32_t kPartitionA = 2;
inline constexpr std::uint32_t kPartitionC = 4;
inline constexpr std::uint32_t kIdrSlice = 5;
inline constexpr std::uint32_t kSps = 7;
inline constexpr std::uint32_t kPps = 8;

struct Sps {
    std::uint32_t id = 0;
    std::uint32_t profileIdc = 0;
    // constraint_set0_flag in bit 5 down to constraint_set5_flag in bit 0.
    std::uint32_t constraintFlags = 0;
    std::uint32_t levelIdc = 0;
    std::uint32_t chromaFormatIdc = 1;
    bool separateColourPlane = false;
    std::uint32_t bitDepthLuma = 8;
    std::uint32_t bitDepthChroma = 8;
    std::uint32_t log2MaxFrameNum = 4;
    std::uint32_t pocType = 0;
    std::uint32_t log2MaxPocLsb = 4;
    bool deltaPicOrderAlwaysZero = false;
    std::uint32_t widthInMbs = 1;
    std::uint32_t heightInMapUnits = 1;
    bool frameMbsOnly = true;
    bool mbAdaptiveFrameField = false;
    bool direct8x8Inference = true;
    // The VUI was present but ended early (accepted, as FFmpeg does).
    bool vuiCut = false;

    [[nodiscard]] std::uint32_t frameHeightInMbs() const noexcept {
        return (frameMbsOnly ? 1U : 2U) * heightInMapUnits;
    }
    [[nodiscard]] std::uint32_t chromaArrayType() const noexcept {
        return separateColourPlane ? 0U : chromaFormatIdc;
    }
};

struct Pps {
    std::uint32_t id = 0;
    std::uint32_t spsId = 0;
    bool entropyCodingMode = false;
    bool bottomFieldPicOrderInFramePresent = false;
    std::uint32_t numSliceGroups = 1;
    std::uint32_t sliceGroupMapType = 0;
    std::uint32_t sliceGroupChangeRate = 1;
    std::array<std::uint32_t, 2> numRefIdxDefault = {1, 1};
    bool weightedPred = false;
    std::uint32_t weightedBipredIdc = 0;
    std::int32_t picInitQpMinus26 = 0;
    bool deblockingFilterControlPresent = false;
    bool redundantPicCntPresent = false;
    bool transform8x8Mode = false;
};

// The parameter sets a stream has delivered so far, by id. An SPS that is
// replaced by a different one takes the PPSs that refer to it along (as in
// FFmpeg): they must be sent again.
struct ParameterSets {
    std::array<std::optional<Sps>, 32> sps;
    std::array<std::vector<std::uint8_t>, 32> spsBytes;
    std::array<std::optional<Pps>, 256> pps;
};

struct DecoderConfig {
    std::uint32_t profileIdc = 0;
    std::uint32_t levelIdc = 0;
    // The NAL unit length size: 1, 2 or 4.
    std::uint32_t lengthSize = 4;
    // The NAL units of its sequence and picture parameter sets (and sequence
    // parameter set extensions), in order.
    std::vector<std::vector<std::uint8_t>> parameterSets;
};

struct ConfigRead {
    std::optional<DecoderConfig> config;
    // Why there is none.
    std::string problem;
    // A configurationVersion other than 1: readers must not parse it.
    bool unsupported = false;
};

// The payload of an avcC box.
[[nodiscard]] ConfigRead readDecoderConfig(std::span<const std::uint8_t> payload);

// The NAL units of one track (one sample description), in decoding order.
class Stream {
public:
    // The most bytes of a parameter set NAL unit read; larger ones fail.
    static constexpr std::size_t kMaxParameterSet = 64 * 1024;
    // The bytes of a slice NAL unit read for its header.
    static constexpr std::size_t kSliceHead = 16 * 1024;

    // The parameter sets of the configuration record; what is wrong with them.
    [[nodiscard]] std::optional<std::string> configure(const DecoderConfig& config);

    // How many of a NAL unit's bytes nalUnit() needs, by its header byte and size.
    [[nodiscard]] static std::uint64_t headBytes(std::uint8_t header, std::uint64_t size) noexcept;
    // "SPS", "IDR slice", "SEI", ... by the header byte, for details.
    [[nodiscard]] static std::string kindOf(std::uint8_t header);

    // One NAL unit: its first bytes `head` (headBytes() of them, or all) and
    // its size. What is wrong with it, if anything.
    [[nodiscard]] std::optional<std::string> nalUnit(std::span<const std::uint8_t> head, std::uint64_t size);

    // "4 SPS, 4 PPS, 30 slices (headers read; CABAC 30)".
    [[nodiscard]] std::string summary() const;
    [[nodiscard]] std::uint64_t slices() const noexcept { return slices_; }
    // The profile and size of the first SPS, for summaries.
    [[nodiscard]] const std::optional<Sps>& firstSps() const noexcept { return firstSps_; }

private:
    [[nodiscard]] std::optional<std::string> sps(std::span<const std::uint8_t> unit);
    [[nodiscard]] std::optional<std::string> pps(std::span<const std::uint8_t> unit);
    [[nodiscard]] std::optional<std::string> slice(std::span<const std::uint8_t> head, bool complete,
                                                   std::uint32_t type, std::uint32_t refIdc);

    ParameterSets sets_;
    std::optional<Sps> firstSps_;
    std::uint64_t spsCount_ = 0;
    std::uint64_t ppsCount_ = 0;
    std::uint64_t slices_ = 0;
    std::uint64_t cabacSlices_ = 0;
    std::uint64_t partitions_ = 0;
    std::uint64_t others_ = 0;
};

// "Baseline", "Main", "High", ... by profile_idc.
[[nodiscard]] std::string profileName(std::uint32_t profileIdc);

}  // namespace recovery::validation::detail::avc
