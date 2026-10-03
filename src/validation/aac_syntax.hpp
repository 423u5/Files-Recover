#pragma once

// The syntax of AAC (ISO/IEC 14496-3 1.6.2 and 4.4.2, ISO/IEC 13818-7) that
// can be read without the codec's Huffman tables, for the media checks of
// ADTS streams (aac_media.cpp) and MP4 sound tracks (mp4_media.cpp).
//
// raw_data_block(): the elements in order. Data stream (DSE), program config
// (PCE) and fill (FIL) elements are read completely. A channel element's
// individual channel streams are read up to their scale factor data:
// global_gain, ics_info (the reserved bit clear, max_sfb within the window's
// scale factor bands, prediction data only in AAC Main, with a reset group
// of 1 to 30, and LTP data only in AAC LTP), ms_mask_present (not the
// reserved 3) and section_data (no reserved codebook 12, sections that end
// exactly at max_sfb). A stream whose sections all use the zero codebook
// has neither scale factors nor spectral data, so it is read to its end
// (pulse data only with long windows, TNS filter orders within the
// profile's limit, no gain control data outside AAC SSR) and the block is
// read on. Coded scale factors and spectral data need the Huffman tables:
// reading stops there, as it does at a coupling channel element. The
// channel elements may not hold more channels than the channel
// configuration (a single CPE in a mono configuration is played as stereo
// by FFmpeg and accepted), and an LFE needs a configuration with one.
//
// AudioSpecificConfig: the audio object type, sampling frequency and channel
// configuration, explicit and backward-compatible SBR/PS signalling, and the
// GASpecificConfig of AAC Main, LC and LTP.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace recovery::validation::detail::aac {

// id_syn_ele.
inline constexpr std::uint32_t kSce = 0;
inline constexpr std::uint32_t kCpe = 1;
inline constexpr std::uint32_t kCce = 2;
inline constexpr std::uint32_t kLfe = 3;
inline constexpr std::uint32_t kDse = 4;
inline constexpr std::uint32_t kPce = 5;
inline constexpr std::uint32_t kFil = 6;
inline constexpr std::uint32_t kEnd = 7;

// Audio object types.
inline constexpr std::uint32_t kAacMain = 1;
inline constexpr std::uint32_t kAacLc = 2;
inline constexpr std::uint32_t kAacSsr = 3;
inline constexpr std::uint32_t kAacLtp = 4;
inline constexpr std::uint32_t kSbr = 5;
inline constexpr std::uint32_t kPs = 29;

// The sampling frequencies of sampling_frequency_index 0 to 12.
inline constexpr std::uint32_t kSamplingIndices = 13;
[[nodiscard]] std::uint32_t sampleRate(std::uint32_t samplingIndex) noexcept;
// The sampling_frequency_index whose tables a frequency uses: its own, or the
// nearest standard frequency's (ISO/IEC 14496-3 Table 4.82).
[[nodiscard]] std::uint32_t samplingIndexOf(std::uint32_t frequency) noexcept;

// What the raw data blocks of a stream are decoded with.
struct Config {
    // The core's audio object type: kAacMain, kAacLc or kAacLtp (kAacSsr
    // streams are not read).
    std::uint32_t objectType = kAacLc;
    // The core's sampling_frequency_index (0 to 12).
    std::uint32_t samplingIndex = 4;
    // channel_configuration. Only 1 to 7 limit the channel elements (0: a
    // program config element describes the channels).
    std::uint32_t channelConfig = 2;
};

// "AAC LC", "AAC Main", ...
[[nodiscard]] std::string objectTypeName(std::uint32_t objectType);
// "SCE", "CPE", ...
[[nodiscard]] std::string elementName(std::uint32_t id);

struct IcsInfo {
    // 0 ONLY_LONG, 1 LONG_START, 2 EIGHT_SHORT, 3 LONG_STOP.
    std::uint32_t windowSequence = 0;
    std::uint32_t maxSfb = 0;
    std::uint32_t windowGroups = 1;

    [[nodiscard]] bool shortWindows() const noexcept { return windowSequence == 2; }
};

// An element of a raw data block (ID_END is not one). Positions are bit
// offsets into the bytes the block was read from.
struct Element {
    std::uint32_t id = kSce;
    // The first bit after id_syn_ele.
    std::uint64_t start = 0;
    // How far the element was read; its end when it was read completely.
    std::uint64_t readTo = 0;
    // The bit after the element, when it was read completely.
    std::optional<std::uint64_t> end;
    // CPE: common_window, and the ics_info its streams share when it is set.
    bool commonWindow = false;
    IcsInfo commonInfo;
    // CPE: the second individual_channel_stream's first bit, known when the
    // first stream was read to its end.
    std::optional<std::uint64_t> secondStream;

    [[nodiscard]] bool isChannel() const noexcept { return id == kSce || id == kCpe || id == kLfe; }
};

enum class BlockStatus : std::uint8_t {
    // Read through ID_END; `end` is the bit after it.
    Complete,
    // Read up to data that needs the Huffman tables (coded scale factors) or
    // a coupling channel element: `detail` says which, `at` where.
    Stopped,
    // A syntax error, or the elements run past the end of the block:
    // `detail` says what, `at` where.
    Invalid,
};

struct Block {
    BlockStatus status = BlockStatus::Stopped;
    // The elements read, in order, including the one where reading stopped
    // or failed.
    std::vector<Element> elements;
    std::uint64_t end = 0;
    std::uint64_t at = 0;
    std::string detail;

    [[nodiscard]] std::size_t count(std::uint32_t id) const noexcept;
};

// Reads the raw_data_block() in bits [begin, end) of `bytes`. Both must be
// byte aligned, and `end` at most the bits of `bytes`; alignment inside the
// block (data stream and program config elements) is relative to `begin`.
[[nodiscard]] Block readRawDataBlock(std::span<const std::uint8_t> bytes, std::uint64_t begin, std::uint64_t end,
                                     const Config& config);

// Whether a CPE's second individual_channel_stream can start at bit
// `position`: its global_gain, ics_info (without a common window) and
// section_data read without error before bit `end`.
[[nodiscard]] bool secondStreamFits(std::span<const std::uint8_t> bytes, std::uint64_t position, std::uint64_t end,
                                    const Element& cpe, const Config& config);

struct Span {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

// When the bits from `position` are fill and data stream elements and an
// ID_END whose byte alignment (relative to `begin`) ends at `end`, the data
// stream elements' spans (from their first bit after id_syn_ele); nothing
// otherwise. Where a block may end when its last channel element was not
// read to its end.
[[nodiscard]] std::optional<std::vector<Span>> readTail(std::span<const std::uint8_t> bytes, std::uint64_t begin,
                                                        std::uint64_t position, std::uint64_t end);

struct AudioSpecificConfig {
    // As signalled: 5 for explicit SBR, 29 for explicit PS, the core's otherwise.
    std::uint32_t audioObjectType = 0;
    // The core: its object type, its sampling frequency index (from the
    // explicit frequency when one is given) and the channel configuration.
    Config core;
    std::uint32_t samplingFrequency = 0;
    bool sbr = false;
    bool ps = false;
};

struct AscRead {
    std::optional<AudioSpecificConfig> config;
    // Why there is none.
    std::string problem;
    // The configuration is valid but not read: another audio object type,
    // 960-sample frames, a reserved channel configuration.
    bool unsupported = false;
};

[[nodiscard]] AscRead readAudioSpecificConfig(std::span<const std::uint8_t> bytes);

// What an ADTS frame's crc_check covers, as libfdk-aac computes and checks
// it and as fdkaac's files bear out: CRC-16 (MPEG) over the 56 bits of the
// header, then, element by element of the raw data block, the first 192
// bits of each SCE and LFE and of each CPE and the first 128 bits of a CPE's
// second individual_channel_stream (each region from its first bit after
// id_syn_ele, with zeros in place of the bits an element too short to fill
// it does not have), and every bit of each data stream element. Fill
// elements are not covered.
class Crc16 {
public:
    static constexpr std::uint16_t kInitial = 0xFFFF;

    // `count` bits from bit `begin` of `bytes`, most significant first.
    static std::uint16_t bits(std::uint16_t crc, std::span<const std::uint8_t> bytes, std::uint64_t begin,
                              std::uint64_t count) noexcept;
    static std::uint16_t zeros(std::uint16_t crc, std::uint64_t count) noexcept;
    // A region of `size` bits from `begin` of which those before `end` are
    // data and the rest zeros.
    static std::uint16_t region(std::uint16_t crc, std::span<const std::uint8_t> bytes, std::uint64_t begin,
                                std::uint64_t end, std::uint64_t size) noexcept;

    // The register as polynomial arithmetic modulo the generator: crc after
    // n more zero bits is multiply(crc, power(n)).
    static std::uint16_t multiply(std::uint16_t a, std::uint16_t b) noexcept;
    // x^n modulo the generator.
    static std::uint16_t power(std::uint64_t n) noexcept;
    // One more bit of the message: x * crc + bit * x^16.
    static std::uint16_t timesX(std::uint16_t crc) noexcept {
        return static_cast<std::uint16_t>((crc << 1) ^ ((crc & 0x8000U) != 0 ? 0x8005U : 0U));
    }
    // x^16 modulo the generator: what a message bit adds.
    static constexpr std::uint16_t kX16 = 0x8005;
};

}  // namespace recovery::validation::detail::aac
