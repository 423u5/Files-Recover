#include "aac_syntax.hpp"

#include "bit_reader.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace recovery::validation::detail::aac {

namespace {

// Scale factor bands of a long window and of a short window, and the bands
// AAC Main prediction covers, by sampling frequency index (ISO/IEC 14496-3
// 4.5.4 and 4.6.6).
constexpr std::array<std::uint32_t, kSamplingIndices> kLongBands = {41, 41, 47, 49, 49, 51, 47,
                                                                    47, 43, 43, 43, 40, 40};
constexpr std::array<std::uint32_t, kSamplingIndices> kShortBands = {12, 12, 12, 14, 14, 14, 15,
                                                                     15, 15, 15, 15, 15, 15};
constexpr std::array<std::uint32_t, kSamplingIndices> kPredictionBands = {33, 33, 38, 40, 40, 40, 41,
                                                                          41, 37, 37, 37, 34, 34};
constexpr std::array<std::uint32_t, kSamplingIndices> kSampleRates = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                                      22050, 16000, 12000, 11025, 8000,  7350};
constexpr std::uint32_t kMaxLtpLongBands = 40;
constexpr std::uint32_t kMaxTnsOrderShort = 7;
constexpr std::uint32_t kMaxTnsOrderLong = 12;
constexpr std::uint32_t kMaxTnsOrderMain = 20;
// Channels of channel configurations 0 to 7 (0: described by a program config element).
constexpr std::array<std::uint32_t, 8> kConfigChannels = {0, 1, 2, 3, 4, 5, 6, 8};
constexpr std::uint32_t kSyncExtension = 0x2B7;
constexpr std::uint32_t kSyncPs = 0x548;

enum class Step : std::uint8_t { Ok, Stopped, Invalid };

// Reads elements of a raw data block from bits [begin, end).
class Reader {
public:
    Reader(std::span<const std::uint8_t> bytes, std::uint64_t begin, std::uint64_t end, const Config& config)
        : bits_(bytes.first(static_cast<std::size_t>(end / 8))), begin_(begin), config_(config) {
        bits_.seek(begin);
    }

    [[nodiscard]] MsbBits& bits() noexcept { return bits_; }
    [[nodiscard]] std::uint64_t at() const noexcept { return at_; }
    [[nodiscard]] std::string& detail() noexcept { return detail_; }

    Step icsInfo(IcsInfo& info, bool commonWindow);
    Step sectionData(const IcsInfo& info, bool& allZero);
    // individual_channel_stream(); `common` is the CPE's shared ics_info
    // (nullptr: the stream has its own). Ok: read to its end.
    Step stream(const IcsInfo* common);
    Step channelElement(Element& element);
    Step dataElement();
    Step programElement();
    Step fillElement();

private:
    Step ltpData(const IcsInfo& info);
    Step tnsData(const IcsInfo& info);
    void alignToByte() {
        const std::uint64_t offset = (bits_.position() - begin_) % 8;
        if (offset != 0) {
            bits_.skip(8 - offset);
        }
    }
    Step invalid(std::uint64_t at, std::string detail) {
        at_ = at;
        detail_ = bits_.overrun() ? "the elements run past the end of the block" : std::move(detail);
        return Step::Invalid;
    }
    Step stopped(std::uint64_t at, std::string detail) {
        at_ = at;
        detail_ = std::move(detail);
        return Step::Stopped;
    }
    // Ok, or Invalid when a read went past the end of the block.
    Step checked(std::uint64_t at) {
        return bits_.overrun() ? invalid(at, {}) : Step::Ok;
    }

    MsbBits bits_;
    std::uint64_t begin_;
    const Config& config_;
    std::uint64_t at_ = 0;
    std::string detail_;
};

Step Reader::ltpData(const IcsInfo& info) {
    bits_.skip(11 + 3);  // ltp_lag, ltp_coef
    bits_.skip(std::min(info.maxSfb, kMaxLtpLongBands));
    return Step::Ok;
}

Step Reader::icsInfo(IcsInfo& info, bool commonWindow) {
    const std::uint64_t at = bits_.position();
    if (bits_.flag()) {
        return invalid(at, "ics_reserved_bit is set");
    }
    info.windowSequence = bits_.read(2);
    bits_.skip(1);  // window_shape
    const std::uint32_t index = config_.samplingIndex;
    if (info.shortWindows()) {
        info.maxSfb = bits_.read(4);
        const std::uint32_t grouping = bits_.read(7);
        info.windowGroups = 1;
        for (int bit = 6; bit >= 0; --bit) {
            info.windowGroups += ((grouping >> bit) & 1U) == 0 ? 1U : 0U;
        }
        if (info.maxSfb > kShortBands[index]) {
            return invalid(at, "max_sfb " + std::to_string(info.maxSfb) + " beyond the " +
                                   std::to_string(kShortBands[index]) + " bands of a short window");
        }
        return checked(at);
    }
    info.maxSfb = bits_.read(6);
    info.windowGroups = 1;
    if (info.maxSfb > kLongBands[index]) {
        return invalid(at, "max_sfb " + std::to_string(info.maxSfb) + " beyond the " +
                               std::to_string(kLongBands[index]) + " bands of a long window");
    }
    if (bits_.flag()) {  // predictor_data_present
        if (config_.objectType == kAacMain) {
            if (bits_.flag()) {
                const std::uint32_t group = bits_.read(5);
                if (group == 0 || group > 30) {
                    return invalid(at, "predictor reset group " + std::to_string(group) + " (1 to 30)");
                }
            }
            bits_.skip(std::min(info.maxSfb, kPredictionBands[index]));
        } else if (config_.objectType == kAacLtp) {
            if (bits_.flag()) {
                ltpData(info);
            }
            if (commonWindow && bits_.flag()) {
                ltpData(info);
            }
        } else {
            return invalid(at, "prediction data in " + objectTypeName(config_.objectType));
        }
    }
    return checked(at);
}

Step Reader::sectionData(const IcsInfo& info, bool& allZero) {
    const unsigned lengthBits = info.shortWindows() ? 3 : 5;
    const std::uint32_t escape = (1U << lengthBits) - 1;
    allZero = true;
    for (std::uint32_t group = 0; group < info.windowGroups; ++group) {
        std::uint32_t band = 0;
        while (band < info.maxSfb) {
            const std::uint64_t at = bits_.position();
            const std::uint32_t codebook = bits_.read(4);
            if (codebook == 12) {
                return invalid(at, "a section with the reserved codebook 12");
            }
            std::uint32_t length = 0;
            for (;;) {
                const std::uint32_t increment = bits_.read(lengthBits);
                length += increment;
                if (increment != escape || bits_.overrun()) {
                    break;
                }
            }
            if (bits_.overrun()) {
                return invalid(at, {});
            }
            band += length;
            if (band > info.maxSfb) {
                return invalid(at, "sections run to band " + std::to_string(band) + ", beyond max_sfb " +
                                       std::to_string(info.maxSfb));
            }
            if (length > 0 && codebook != 0) {
                allZero = false;
            }
        }
    }
    return Step::Ok;
}

Step Reader::tnsData(const IcsInfo& info) {
    const bool shortWindows = info.shortWindows();
    const std::uint32_t maxOrder = shortWindows                         ? kMaxTnsOrderShort
                                   : config_.objectType == kAacMain ? kMaxTnsOrderMain
                                                                    : kMaxTnsOrderLong;
    for (int window = 0; window < (shortWindows ? 8 : 1); ++window) {
        const std::uint32_t filters = bits_.read(shortWindows ? 1 : 2);
        if (filters == 0) {
            continue;
        }
        const std::uint32_t coefficientResolution = bits_.read(1);
        for (std::uint32_t filter = 0; filter < filters; ++filter) {
            const std::uint64_t at = bits_.position();
            bits_.skip(shortWindows ? 4 : 6);  // length
            const std::uint32_t order = bits_.read(shortWindows ? 3 : 5);
            if (order > maxOrder) {
                return invalid(at, "TNS filter order " + std::to_string(order) + " above " + std::to_string(maxOrder));
            }
            if (order > 0) {
                bits_.skip(1);  // direction
                const std::uint32_t compress = bits_.read(1);
                bits_.skip(std::uint64_t{order} * (coefficientResolution + 3 - compress));
            }
        }
    }
    return Step::Ok;
}

Step Reader::stream(const IcsInfo* common) {
    const std::uint64_t at = bits_.position();
    bits_.skip(8);  // global_gain
    IcsInfo own;
    if (common == nullptr) {
        if (const Step step = icsInfo(own, false); step != Step::Ok) {
            return step;
        }
    }
    const IcsInfo& info = common != nullptr ? *common : own;
    bool allZero = true;
    if (const Step step = sectionData(info, allZero); step != Step::Ok) {
        return step;
    }
    if (!allZero) {
        return stopped(bits_.position(), "coded scale factors");
    }
    // No scale factors and no spectral data: the zero codebook everywhere.
    const std::uint64_t pulseAt = bits_.position();
    if (bits_.flag()) {
        if (info.shortWindows()) {
            return invalid(pulseAt, "pulse data in a stream of short windows");
        }
        const std::uint32_t pulses = bits_.read(2) + 1;
        const std::uint32_t startBand = bits_.read(6);
        if (startBand >= kLongBands[config_.samplingIndex]) {
            return invalid(pulseAt, "pulse data from band " + std::to_string(startBand) + " of " +
                                        std::to_string(kLongBands[config_.samplingIndex]));
        }
        bits_.skip(std::uint64_t{9} * pulses);  // pulse_offset, pulse_amp
    }
    if (bits_.flag()) {
        if (const Step step = tnsData(info); step != Step::Ok) {
            return step;
        }
    }
    const std::uint64_t gainAt = bits_.position();
    if (bits_.flag()) {
        return invalid(gainAt, "gain control data in " + objectTypeName(config_.objectType));
    }
    return checked(at);
}

Step Reader::channelElement(Element& element) {
    element.start = bits_.position();
    bits_.skip(4);  // element_instance_tag
    const auto finish = [&](Step step) {
        element.readTo = bits_.position();
        if (step == Step::Ok) {
            element.end = element.readTo;
        }
        return step;
    };
    if (element.id != kCpe) {
        return finish(stream(nullptr));
    }
    element.commonWindow = bits_.flag();
    if (element.commonWindow) {
        if (const Step step = icsInfo(element.commonInfo, true); step != Step::Ok) {
            return finish(step);
        }
        const std::uint64_t at = bits_.position();
        const std::uint32_t mask = bits_.read(2);
        if (mask == 3) {
            return finish(invalid(at, "ms_mask_present is the reserved 3"));
        }
        if (mask == 1) {
            bits_.skip(std::uint64_t{element.commonInfo.windowGroups} * element.commonInfo.maxSfb);
        }
    }
    const IcsInfo* common = element.commonWindow ? &element.commonInfo : nullptr;
    Step step = stream(common);
    if (step == Step::Ok) {
        element.secondStream = bits_.position();
        step = stream(common);
    }
    return finish(step);
}

Step Reader::dataElement() {
    const std::uint64_t at = bits_.position();
    bits_.skip(4);  // element_instance_tag
    const bool align = bits_.flag();
    std::uint32_t count = bits_.read(8);
    if (count == 255) {
        count += bits_.read(8);
    }
    if (align) {
        alignToByte();
    }
    bits_.skip(std::uint64_t{8} * count);
    return checked(at);
}

// program_config_element() (ISO/IEC 14496-3 4.4.1.1); byte_alignment()
// relative to `base`.
void readProgramConfig(MsbBits& bits, std::uint64_t base) {
    bits.skip(4 + 2 + 4);  // element_instance_tag, object_type, sampling_frequency_index
    const std::uint32_t front = bits.read(4);
    const std::uint32_t side = bits.read(4);
    const std::uint32_t back = bits.read(4);
    const std::uint32_t lfe = bits.read(2);
    const std::uint32_t associated = bits.read(3);
    const std::uint32_t coupling = bits.read(4);
    for (const unsigned mixdown : {4U, 4U, 3U}) {  // mono, stereo, matrix mixdown
        if (bits.flag()) {
            bits.skip(mixdown);
        }
    }
    bits.skip(std::uint64_t{5} * (front + side + back) + std::uint64_t{4} * (lfe + associated) +
              std::uint64_t{5} * coupling);
    const std::uint64_t offset = (bits.position() - base) % 8;
    if (offset != 0) {
        bits.skip(8 - offset);
    }
    bits.skip(std::uint64_t{8} * bits.read(8));  // comment_field_bytes
}

Step Reader::programElement() {
    const std::uint64_t at = bits_.position();
    readProgramConfig(bits_, begin_);
    return checked(at);
}

Step Reader::fillElement() {
    const std::uint64_t at = bits_.position();
    std::uint32_t count = bits_.read(4);
    if (count == 15) {
        count += bits_.read(8) - 1;
    }
    bits_.skip(std::uint64_t{8} * count);
    return checked(at);
}

}  // namespace

std::uint32_t samplingIndexOf(std::uint32_t frequency) noexcept {
    // ISO/IEC 14496-3 Table 4.82: the tables of the nearest standard frequency.
    static constexpr std::array<std::uint32_t, 11> kLowerBounds = {92017, 75132, 55426, 46009, 37566, 27713,
                                                                   23004, 18783, 13856, 11502, 9391};
    for (std::uint32_t index = 0; index < kLowerBounds.size(); ++index) {
        if (frequency >= kLowerBounds[index]) {
            return index;
        }
    }
    return 11;
}

std::uint32_t sampleRate(std::uint32_t samplingIndex) noexcept {
    return samplingIndex < kSamplingIndices ? kSampleRates[samplingIndex] : 0;
}

std::string objectTypeName(std::uint32_t objectType) {
    switch (objectType) {
    case kAacMain:
        return "AAC Main";
    case kAacLc:
        return "AAC LC";
    case kAacSsr:
        return "AAC SSR";
    case kAacLtp:
        return "AAC LTP";
    case kSbr:
        return "HE-AAC (SBR)";
    case kPs:
        return "HE-AAC v2 (PS)";
    default:
        return "audio object type " + std::to_string(objectType);
    }
}

std::string elementName(std::uint32_t id) {
    static constexpr std::array<const char*, 8> kNames = {"SCE", "CPE", "CCE", "LFE", "DSE", "PCE", "FIL", "END"};
    return kNames[id & 7U];
}

std::size_t Block::count(std::uint32_t id) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(elements.begin(), elements.end(), [id](const Element& element) { return element.id == id; }));
}

Block readRawDataBlock(std::span<const std::uint8_t> bytes, std::uint64_t begin, std::uint64_t end,
                       const Config& config) {
    Block block;
    block.status = BlockStatus::Invalid;
    block.at = begin;
    if (begin % 8 != 0 || end % 8 != 0 || begin > end || end > std::uint64_t{bytes.size()} * 8 ||
        config.samplingIndex >= kSamplingIndices) {
        block.detail = "no raw data block to read";
        return block;
    }
    Reader reader(bytes, begin, end, config);
    MsbBits& bits = reader.bits();
    const std::uint32_t configured =
        config.channelConfig < kConfigChannels.size() ? kConfigChannels[config.channelConfig] : 0;
    std::uint32_t channels = 0;
    for (;;) {
        const std::uint64_t at = bits.position();
        const std::uint32_t id = bits.read(3);
        if (bits.overrun()) {
            block.at = at;
            block.detail = "no ID_END before the end of the block";
            return block;
        }
        if (id == kEnd) {
            block.status = BlockStatus::Complete;
            block.end = bits.position();
            return block;
        }
        if (id == kCce) {
            block.status = BlockStatus::Stopped;
            block.at = at;
            block.detail = "a coupling channel element";
            return block;
        }
        Element& element = block.elements.emplace_back();
        element.id = id;
        element.start = bits.position();
        Step step = Step::Ok;
        if (element.isChannel()) {
            const std::uint32_t elementChannels = id == kCpe ? 2 : 1;
            if (configured != 0 && channels + elementChannels > std::max(configured, 2U)) {
                block.at = at;
                block.detail = "channel elements for more than the " + std::to_string(configured) +
                               (configured == 1 ? " channel" : " channels") + " of channel configuration " +
                               std::to_string(config.channelConfig);
                return block;
            }
            if (id == kLfe && configured != 0 && config.channelConfig < 6) {
                block.at = at;
                block.detail = "an LFE element in channel configuration " + std::to_string(config.channelConfig) +
                               ", which has no LFE channel";
                return block;
            }
            channels += elementChannels;
            step = reader.channelElement(element);
        } else {
            step = id == kDse ? reader.dataElement() : id == kPce ? reader.programElement() : reader.fillElement();
            element.readTo = bits.position();
            if (step == Step::Ok) {
                element.end = element.readTo;
            }
        }
        if (step != Step::Ok) {
            block.status = step == Step::Stopped ? BlockStatus::Stopped : BlockStatus::Invalid;
            block.at = reader.at();
            block.detail = elementName(id) + ": " + std::move(reader.detail());
            return block;
        }
    }
}

bool secondStreamFits(std::span<const std::uint8_t> bytes, std::uint64_t position, std::uint64_t end,
                      const Element& cpe, const Config& config) {
    if (position >= end || end > std::uint64_t{bytes.size()} * 8 || config.samplingIndex >= kSamplingIndices) {
        return false;
    }
    Reader reader(bytes, position, (end + 7) / 8 * 8, config);
    reader.bits().skip(8);  // global_gain
    IcsInfo own;
    const IcsInfo* info = &cpe.commonInfo;
    if (!cpe.commonWindow) {
        if (reader.icsInfo(own, false) != Step::Ok) {
            return false;
        }
        info = &own;
    }
    bool allZero = true;
    return reader.sectionData(*info, allZero) == Step::Ok && !reader.bits().overrun() &&
           reader.bits().position() <= end;
}

std::optional<std::vector<Span>> readTail(std::span<const std::uint8_t> bytes, std::uint64_t begin,
                                          std::uint64_t position, std::uint64_t end) {
    if (end % 8 != 0 || position > end || end > std::uint64_t{bytes.size()} * 8) {
        return std::nullopt;
    }
    const Config config;
    Reader reader(bytes, begin, end, config);
    MsbBits& bits = reader.bits();
    bits.seek(position);
    std::vector<Span> data;
    for (;;) {
        const std::uint32_t id = bits.read(3);
        if (bits.overrun()) {
            return std::nullopt;
        }
        if (id == kEnd) {
            const std::uint64_t offset = (bits.position() - begin) % 8;
            const std::uint64_t aligned = bits.position() + (offset != 0 ? 8 - offset : 0);
            if (aligned != end) {
                return std::nullopt;
            }
            return data;
        }
        const std::uint64_t start = bits.position();
        Step step = Step::Invalid;
        if (id == kFil) {
            step = reader.fillElement();
        } else if (id == kDse) {
            step = reader.dataElement();
            data.push_back(Span{start, bits.position()});
        }
        if (step != Step::Ok) {
            return std::nullopt;
        }
    }
}

AscRead readAudioSpecificConfig(std::span<const std::uint8_t> bytes) {
    AscRead read;
    MsbBits bits(bytes);
    const auto objectType = [&bits]() {
        const std::uint32_t type = bits.read(5);
        return type == 31 ? 32 + bits.read(6) : type;
    };
    // sampling_frequency_index and the frequency it stands for; false for a reserved index.
    const auto frequency = [&bits](std::uint32_t& index, std::uint32_t& hertz) {
        index = bits.read(4);
        if (index == 15) {
            hertz = bits.read(24);
            return hertz != 0;
        }
        hertz = sampleRate(index);
        return index < kSamplingIndices;
    };
    AudioSpecificConfig config;
    config.audioObjectType = objectType();
    std::uint32_t index = 0;
    if (!frequency(index, config.samplingFrequency)) {
        read.problem = "a reserved or zero sampling frequency";
        return read;
    }
    const std::uint32_t channels = bits.read(4);
    std::uint32_t core = config.audioObjectType;
    if (core == kSbr || core == kPs) {
        config.sbr = true;
        config.ps = core == kPs;
        std::uint32_t extensionIndex = 0;
        std::uint32_t extensionFrequency = 0;
        if (!frequency(extensionIndex, extensionFrequency)) {
            read.problem = "a reserved or zero SBR sampling frequency";
            return read;
        }
        core = objectType();
    }
    if (core != kAacMain && core != kAacLc && core != kAacLtp) {
        read.unsupported = true;
        read.problem = objectTypeName(core) + " is not read";
        return read;
    }
    if (channels == 8 || channels == 9 || channels == 10 || channels == 15) {
        read.problem = "the reserved channel configuration " + std::to_string(channels);
        return read;
    }
    // GASpecificConfig.
    if (bits.flag()) {
        read.unsupported = true;
        read.problem = "960-sample frames are not read";
        return read;
    }
    if (bits.flag()) {  // dependsOnCoreCoder
        bits.skip(14);
    }
    const bool extension = bits.flag();
    if (channels == 0) {
        readProgramConfig(bits, 0);
    }
    if (extension) {
        bits.skip(1);  // extensionFlag3
    }
    // Backward-compatible SBR and PS signalling after the core's configuration.
    if (!config.sbr && bits.left() >= 16) {
        if (bits.read(11) == kSyncExtension && objectType() == kSbr) {
            config.sbr = bits.flag();
            if (config.sbr) {
                std::uint32_t extensionIndex = 0;
                std::uint32_t extensionFrequency = 0;
                if (!frequency(extensionIndex, extensionFrequency)) {
                    read.problem = "a reserved or zero SBR sampling frequency";
                    return read;
                }
                if (bits.left() >= 12 && bits.read(11) == kSyncPs) {
                    config.ps = bits.flag();
                }
            }
        }
    }
    if (bits.overrun()) {
        read.problem = "the AudioSpecificConfig ends inside its fields";
        return read;
    }
    config.core.objectType = core;
    config.core.samplingIndex = index == 15 ? samplingIndexOf(config.samplingFrequency) : index;
    config.core.channelConfig = channels;
    read.config = config;
    return read;
}

std::uint16_t Crc16::bits(std::uint16_t crc, std::span<const std::uint8_t> bytes, std::uint64_t begin,
                          std::uint64_t count) noexcept {
    for (std::uint64_t bit = begin; bit < begin + count; ++bit) {
        const auto value = static_cast<std::uint32_t>(bytes[static_cast<std::size_t>(bit / 8)] >> (7 - bit % 8)) & 1U;
        const std::uint32_t top = (crc >> 15) & 1U;
        crc = static_cast<std::uint16_t>(crc << 1);
        if ((top ^ value) != 0) {
            crc ^= kX16;
        }
    }
    return crc;
}

std::uint16_t Crc16::zeros(std::uint16_t crc, std::uint64_t count) noexcept {
    return multiply(crc, power(count));
}

std::uint16_t Crc16::region(std::uint16_t crc, std::span<const std::uint8_t> bytes, std::uint64_t begin,
                            std::uint64_t end, std::uint64_t size) noexcept {
    const std::uint64_t data = end > begin ? std::min(end - begin, size) : 0;
    return zeros(bits(crc, bytes, begin, data), size - data);
}

std::uint16_t Crc16::multiply(std::uint16_t a, std::uint16_t b) noexcept {
    std::uint16_t product = 0;
    for (int bit = 15; bit >= 0; --bit) {
        product = timesX(product);
        if (((b >> bit) & 1U) != 0) {
            product ^= a;
        }
    }
    return product;
}

std::uint16_t Crc16::power(std::uint64_t n) noexcept {
    std::uint16_t result = 1;
    std::uint16_t square = 2;  // x
    for (; n != 0; n >>= 1) {
        if ((n & 1U) != 0) {
            result = multiply(result, square);
        }
        square = multiply(square, square);
    }
    return result;
}

}  // namespace recovery::validation::detail::aac
