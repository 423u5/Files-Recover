#pragma once

// The syntax of HEVC (ITU-T H.265) that can be read without decoding the
// coded pictures, for the media checks of MP4 video tracks (mp4_media.cpp):
//
//  * the HEVCDecoderConfigurationRecord of an hvcC box (ISO/IEC 14496-15
//    8.3.3) and the parameter sets in its arrays;
//  * video parameter sets (7.3.2.1) with profile, tier and level and HRD
//    parameters;
//  * sequence parameter sets (7.3.2.2): scaling lists, PCM, short-term
//    reference picture sets (inter-set prediction derived as 7-61 and 7-62
//    order them), long-term pictures, the VUI, and the range, multilayer,
//    3D and screen content extensions;
//  * picture parameter sets (7.3.2.3): tiles, deblocking, scaling lists, and
//    the range and screen content extensions;
//  * the slice segment header of every coded slice segment (7.3.6): the
//    reference picture sets of the slice, reference list modification,
//    prediction weights, entry points (which must lie inside the slice
//    segment data), the header extension and byte_alignment(). Slice data
//    is not decoded.
//
// As for AVC (avc_syntax.hpp), a header fails where the specification
// forbids what it holds and FFmpeg's decoder (6.1) rejects it too, and where
// the syntax cannot be followed; a VUI that does not read cleanly is
// accepted (FFmpeg retries VUIs of old encoders with another layout), and
// so is a PPS that ends early (FFmpeg only warns). NAL units of layers
// other than the base layer are not read (FFmpeg ignores them too).

#include "nal_units.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace recovery::validation::detail::hevc {

// nal_unit_type values.
inline constexpr std::uint32_t kIdrWithRadl = 19;
inline constexpr std::uint32_t kIdrNoLeading = 20;
inline constexpr std::uint32_t kVps = 32;
inline constexpr std::uint32_t kSps = 33;
inline constexpr std::uint32_t kPps = 34;

// A short-term reference picture set: negative POC deltas first (closest
// first), then positive ones (closest first).
struct ShortTermRps {
    std::uint32_t numNegative = 0;
    std::uint32_t numDelta = 0;
    std::array<std::int32_t, 32> deltaPoc{};
    std::array<bool, 32> used{};

    [[nodiscard]] std::uint32_t usedCount() const noexcept;
};

struct Vps {
    std::uint32_t id = 0;
    std::uint32_t maxSubLayers = 1;
};

struct Sps {
    std::uint32_t id = 0;
    std::uint32_t vpsId = 0;
    std::uint32_t maxSubLayersMinus1 = 0;
    std::uint32_t profileIdc = 0;
    std::uint32_t chromaFormatIdc = 1;
    bool separateColourPlane = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bitDepthLuma = 8;
    std::uint32_t bitDepthChroma = 8;
    std::uint32_t log2MaxPocLsb = 4;
    std::uint32_t log2MinCbSize = 3;
    std::uint32_t log2CtbSize = 4;
    std::uint32_t log2MinTbSize = 2;
    std::uint32_t log2DiffMaxMinCb = 0;
    bool sampleAdaptiveOffset = false;
    std::vector<ShortTermRps> shortTermSets;
    bool longTermRefsPresent = false;
    std::uint32_t numLongTermRefPicsSps = 0;
    std::array<bool, 33> usedByCurrPicLtSps{};
    bool temporalMvp = false;
    std::uint32_t motionVectorResolutionControlIdc = 0;
    // The VUI or what follows it did not read cleanly (accepted).
    bool vuiCut = false;

    [[nodiscard]] std::uint32_t chromaArrayType() const noexcept {
        return separateColourPlane ? 0U : chromaFormatIdc;
    }
    [[nodiscard]] std::uint32_t ctbWidth() const noexcept {
        return (width + (1U << log2CtbSize) - 1) >> log2CtbSize;
    }
    [[nodiscard]] std::uint32_t ctbHeight() const noexcept {
        return (height + (1U << log2CtbSize) - 1) >> log2CtbSize;
    }
};

struct Pps {
    std::uint32_t id = 0;
    std::uint32_t spsId = 0;
    bool dependentSliceSegmentsEnabled = false;
    bool outputFlagPresent = false;
    std::uint32_t numExtraSliceHeaderBits = 0;
    bool cabacInitPresent = false;
    std::array<std::uint32_t, 2> numRefIdxDefault = {1, 1};
    std::int32_t initQpMinus26 = 0;
    bool sliceChromaQpOffsetsPresent = false;
    bool weightedPred = false;
    bool weightedBipred = false;
    bool tilesEnabled = false;
    bool entropyCodingSync = false;
    std::uint32_t tileColumns = 1;
    std::uint32_t tileRows = 1;
    bool loopFilterAcrossSlices = false;
    bool deblockingControlPresent = false;
    bool deblockingOverrideEnabled = false;
    bool deblockingDisabled = false;
    bool listsModificationPresent = false;
    bool sliceHeaderExtensionPresent = false;
    bool chromaQpOffsetListEnabled = false;
    bool currPicRefEnabled = false;
    bool sliceActQpOffsetsPresent = false;
    // Multilayer or 3D extension data: its slices' headers are not read.
    bool extensionsUnread = false;
};

// The parameter sets a stream has delivered so far, by id. A VPS or SPS
// replaced by a different one takes the sets that refer to it along, as in
// FFmpeg.
struct ParameterSets {
    std::array<std::optional<Vps>, 16> vps;
    std::array<std::vector<std::uint8_t>, 16> vpsBytes;
    std::array<std::optional<Sps>, 16> sps;
    std::array<std::vector<std::uint8_t>, 16> spsBytes;
    std::array<std::optional<Pps>, 64> pps;
};

struct DecoderConfig {
    std::uint32_t profileIdc = 0;
    std::uint32_t lengthSize = 4;
    // The NAL units of its arrays, in order.
    std::vector<std::vector<std::uint8_t>> units;
};

struct ConfigRead {
    std::optional<DecoderConfig> config;
    std::string problem;
    // A configurationVersion other than 0 or 1 (FFmpeg takes early files'
    // version 0 records as hvcC too).
    bool unsupported = false;
};

// The payload of an hvcC box.
[[nodiscard]] ConfigRead readDecoderConfig(std::span<const std::uint8_t> payload);

// The NAL units of one track (one sample description), in decoding order.
class Stream {
public:
    static constexpr std::size_t kMaxParameterSet = 64 * 1024;
    static constexpr std::size_t kSliceHead = 16 * 1024;

    [[nodiscard]] std::optional<std::string> configure(const DecoderConfig& config);

    // How many of a NAL unit's bytes nalUnit() needs, by its first header byte and size.
    [[nodiscard]] static std::uint64_t headBytes(std::uint8_t header, std::uint64_t size) noexcept;
    // "VPS", "IDR slice segment", "prefix SEI", ... by the first header byte, for details.
    [[nodiscard]] static std::string kindOf(std::uint8_t header);

    // One NAL unit: its first bytes `head` and its size. What is wrong with it, if anything.
    [[nodiscard]] std::optional<std::string> nalUnit(std::span<const std::uint8_t> head, std::uint64_t size);

    [[nodiscard]] std::string summary() const;
    [[nodiscard]] std::uint64_t slices() const noexcept { return slices_; }
    [[nodiscard]] const std::optional<Sps>& firstSps() const noexcept { return firstSps_; }

private:
    [[nodiscard]] std::optional<std::string> vps(std::span<const std::uint8_t> unit);
    [[nodiscard]] std::optional<std::string> sps(std::span<const std::uint8_t> unit);
    [[nodiscard]] std::optional<std::string> pps(std::span<const std::uint8_t> unit);
    [[nodiscard]] std::optional<std::string> slice(std::span<const std::uint8_t> head, std::uint64_t size,
                                                   std::uint32_t type);

    ParameterSets sets_;
    std::optional<Sps> firstSps_;
    std::uint64_t vpsCount_ = 0;
    std::uint64_t spsCount_ = 0;
    std::uint64_t ppsCount_ = 0;
    std::uint64_t slices_ = 0;
    std::uint64_t unreadSlices_ = 0;
    std::uint64_t otherLayers_ = 0;
    std::uint64_t others_ = 0;
};

// "Main", "Main 10", "Range Extensions", ... by general_profile_idc.
[[nodiscard]] std::string profileName(std::uint32_t profileIdc);

}  // namespace recovery::validation::detail::hevc
