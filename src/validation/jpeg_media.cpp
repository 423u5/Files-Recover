// JPEG media validation: every scan's entropy-coded data decoded (ITU T.81
// Annex F and G, Huffman coding): baseline, extended (8 and 12 bits) and
// progressive frames. Every Huffman code must exist in its table, every
// magnitude category fit the sample precision, every run stay inside its
// block or spectral band, the MCUs come out exactly as many as the frame
// needs, restart markers arrive in order after each interval, and progressive
// scans refine only what earlier scans coded. Coefficient values are not
// kept, only which coefficients of each block are not zero (what the
// refinement scans need), so memory is 8 bytes per block of a progressive
// image. Files without DHT segments use the tables of Annex K.3, as
// libjpeg-turbo does (Motion JPEG frames leave them out). Arithmetic coding,
// lossless and hierarchical frames are not decoded.
//
// Bytes of entropy-coded data after the last MCU of a scan or interval
// ("extraneous bytes before marker", which libjpeg only warns about) are
// accepted up to kMaxExtraneousBytes before each marker, the slack some
// encoders leave; more means the data does not decode to the frame: Huffman
// codes resynchronise, so foreign data inside a scan often shows only as
// data left over after the last MCU.

#include "content_bytes.hpp"
#include "media_decoders.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace recovery::validation::detail {

namespace {

constexpr std::uint8_t kSof0 = 0xC0;
constexpr std::uint8_t kSof1 = 0xC1;
constexpr std::uint8_t kSof2 = 0xC2;
constexpr std::uint8_t kDht = 0xC4;
constexpr std::uint8_t kDac = 0xCC;
constexpr std::uint8_t kRst0 = 0xD0;
constexpr std::uint8_t kRst7 = 0xD7;
constexpr std::uint8_t kSoi = 0xD8;
constexpr std::uint8_t kEoi = 0xD9;
constexpr std::uint8_t kSos = 0xDA;
constexpr std::uint8_t kDqt = 0xDB;
constexpr std::uint8_t kDnl = 0xDC;
constexpr std::uint8_t kDri = 0xDD;

constexpr unsigned kLookahead = 9;
constexpr std::size_t kMaxBlocksInMcu = 10;
constexpr std::uint64_t kMaxExtraneousBytes = 16;

// Annex K.3, as cjpeg writes them.
constexpr std::array<std::uint8_t, 16> kDcLuminanceCounts = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 12> kDcLuminanceValues = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
};
constexpr std::array<std::uint8_t, 16> kDcChrominanceCounts = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 12> kDcChrominanceValues = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
};
constexpr std::array<std::uint8_t, 16> kAcLuminanceCounts = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125};
constexpr std::array<std::uint8_t, 162> kAcLuminanceValues = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
    0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
};
constexpr std::array<std::uint8_t, 16> kAcChrominanceCounts = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119};
constexpr std::array<std::uint8_t, 162> kAcChrominanceValues = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1,
    0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
    0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA,
    0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
};

std::string markerName(std::uint8_t code) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    return std::string("marker FF") + kDigits[code >> 4] + kDigits[code & 0x0F];
}

// A Huffman table (Annex C), decoded most significant bit first.
class HuffmanTable {
public:
    // false when the codes do not fit their lengths (libjpeg's "bogus Huffman table").
    bool build(std::span<const std::uint8_t> counts, std::span<const std::uint8_t> values) {
        std::array<std::uint8_t, 257> sizes{};
        std::array<std::uint32_t, 257> codes{};
        std::size_t count = 0;
        for (std::size_t length = 1; length <= 16; ++length) {
            for (std::uint8_t i = 0; i < counts[length - 1]; ++i) {
                if (count >= 256) {
                    return false;
                }
                sizes[count++] = static_cast<std::uint8_t>(length);
            }
        }
        if (count != values.size()) {
            return false;
        }
        std::uint32_t code = 0;
        std::size_t p = 0;
        unsigned size = sizes[0];
        while (sizes[p] != 0) {
            while (sizes[p] == size) {
                codes[p++] = code++;
            }
            // The all-ones code of each length is reserved: codes must stay below 2^size.
            if (code >= (1U << size)) {
                return false;
            }
            code <<= 1;
            ++size;
        }
        p = 0;
        for (std::size_t length = 1; length <= 16; ++length) {
            if (counts[length - 1] != 0) {
                valueOffset_[length] = static_cast<std::int32_t>(p) - static_cast<std::int32_t>(codes[p]);
                p += counts[length - 1];
                maxCode_[length] = static_cast<std::int32_t>(codes[p - 1]);
            } else {
                maxCode_[length] = -1;
            }
        }
        values_.fill(0);
        std::copy(values.begin(), values.end(), values_.begin());
        lookahead_.fill(0);
        p = 0;
        for (unsigned length = 1; length <= kLookahead; ++length) {
            for (std::uint8_t i = 0; i < counts[length - 1]; ++i, ++p) {
                const std::uint32_t first = codes[p] << (kLookahead - length);
                for (std::uint32_t fill = 0; fill < (1U << (kLookahead - length)); ++fill) {
                    lookahead_[first + fill] = static_cast<std::uint16_t>((length << 8) | values[p]);
                }
            }
        }
        defined_ = true;
        largest_ = 0;
        for (const std::uint8_t value : values) {
            largest_ = std::max(largest_, value);
        }
        return true;
    }

    [[nodiscard]] bool defined() const noexcept { return defined_; }
    [[nodiscard]] std::uint8_t largestValue() const noexcept { return largest_; }

    template <typename Bits>
    [[nodiscard]] int decode(Bits& bits) const {
        const std::uint16_t entry = lookahead_[bits.peek(kLookahead)];
        if (entry != 0) {
            bits.drop(entry >> 8);
            return entry & 0xFF;
        }
        const std::uint32_t peeked = bits.peek(16);
        for (unsigned length = kLookahead + 1; length <= 16; ++length) {
            const auto code = static_cast<std::int32_t>(peeked >> (16 - length));
            if (code <= maxCode_[length]) {
                bits.drop(length);
                return values_[static_cast<std::size_t>(valueOffset_[length] + code)];
            }
        }
        return -1;
    }

private:
    bool defined_ = false;
    std::uint8_t largest_ = 0;
    std::array<std::int32_t, 17> maxCode_{};
    std::array<std::int32_t, 17> valueOffset_{};
    std::array<std::uint8_t, 256> values_{};
    std::array<std::uint16_t, 1U << kLookahead> lookahead_{};
};

// The bits of entropy-coded data, most significant first: FF 00 is a data
// byte FF, fill bytes FF before a marker are skipped, and a marker (or the end
// of the content) ends the data. Beyond it, zero bits are supplied, and taking
// one of them is an overrun.
class EntropyBits {
public:
    explicit EntropyBits(ContentBytes& bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::uint32_t peek(unsigned n) {
        if (count_ < n) {
            fill();
        }
        return static_cast<std::uint32_t>((buffer_ >> (count_ - n)) & ((std::uint64_t{1} << n) - 1));
    }
    void drop(unsigned n) {
        if (n > count_ - padded_) {
            overrun_ = true;
        }
        count_ -= n;
        padded_ = std::min(padded_, count_);
    }
    [[nodiscard]] std::uint32_t read(unsigned n) {
        if (n == 0) {
            return 0;
        }
        const std::uint32_t value = peek(n);
        drop(n);
        return value;
    }

    // Bits were taken from beyond the data.
    [[nodiscard]] bool overrun() const noexcept { return overrun_; }

    struct SegmentEnd {
        // Whole bytes of data after the last bit taken, before the marker.
        std::uint64_t extraneous = 0;
        // The marker that ends the data; none at the end of the content.
        std::optional<std::uint8_t> marker;
        std::uint64_t markerOffset = 0;
    };

    // Ends the data: drops what is left of the current byte, counts whole data
    // bytes up to the marker, and starts afresh after it.
    SegmentEnd finish() {
        SegmentEnd end;
        end.extraneous = (count_ - padded_) / 8;
        while (!stopped_) {
            std::uint8_t value = 0;
            if (nextDataByte(value)) {
                ++end.extraneous;
            }
        }
        end.marker = marker_;
        end.markerOffset = markerOffset_;
        buffer_ = 0;
        count_ = 0;
        padded_ = 0;
        overrun_ = false;
        stopped_ = false;
        marker_.reset();
        return end;
    }

    [[nodiscard]] bool contentEnded() const noexcept { return stopped_ && !marker_.has_value(); }

private:
    // One data byte; false (and stopped) at a marker or the end of the content.
    bool nextDataByte(std::uint8_t& value) {
        std::uint8_t byte = 0;
        if (!bytes_.next(byte)) {
            stopped_ = true;
            return false;
        }
        if (byte != 0xFF) {
            value = byte;
            return true;
        }
        std::uint8_t following = 0xFF;
        while (following == 0xFF) {
            if (!bytes_.next(following)) {
                stopped_ = true;
                return false;
            }
        }
        if (following == 0x00) {
            value = 0xFF;
            return true;
        }
        stopped_ = true;
        marker_ = following;
        markerOffset_ = bytes_.position() - 2;
        return false;
    }

    void fill() {
        while (count_ <= 48) {
            std::uint8_t value = 0;
            if (stopped_ || !nextDataByte(value)) {
                value = 0;
                padded_ += 8;
            }
            buffer_ = (buffer_ << 8) | value;
            count_ += 8;
        }
    }

    ContentBytes& bytes_;
    std::uint64_t buffer_ = 0;
    unsigned count_ = 0;
    unsigned padded_ = 0;
    bool overrun_ = false;
    bool stopped_ = false;
    std::optional<std::uint8_t> marker_;
    std::uint64_t markerOffset_ = 0;
};

struct Component {
    std::uint8_t id = 0;
    std::uint32_t h = 1;
    std::uint32_t v = 1;
    std::uint8_t quantTable = 0;
    // Blocks of a scan of this component alone.
    std::uint64_t blocksWide = 0;
    std::uint64_t blocksHigh = 0;
    // Sequential: coded by a scan already.
    bool coded = false;
    // Progressive: the successive approximation bit each coefficient was last
    // coded to (-1: not yet), and which coefficients of each block are not zero.
    std::array<std::int32_t, 64> coefficientBits{};
    std::vector<std::uint64_t> nonZero;
};

struct ScanComponent {
    std::size_t component = 0;
    std::uint8_t dcTable = 0;
    std::uint8_t acTable = 0;
};

struct Scan {
    std::vector<ScanComponent> components;
    std::uint32_t ss = 0;
    std::uint32_t se = 63;
    std::uint32_t ah = 0;
    std::uint32_t al = 0;
};

class JpegDecoder {
public:
    JpegDecoder(carving::IContentReader& content, const MediaLimits& limits)
        : content_(content), limits_(limits), bytes_(content, 0, content.size()), verdict_("jpeg decoder") {}

    Result<LevelResult> run();

private:
    using Outcome = std::optional<Result<LevelResult>>;

    Outcome ended(std::string_view inside) {
        if (bytes_.error().has_value()) {
            return Result<LevelResult>(*bytes_.error());
        }
        return Result<LevelResult>(verdict_.truncated(content_.size(), "the data ends inside " + std::string(inside)));
    }
    Outcome failed(std::uint64_t offset, std::string detail) {
        return Result<LevelResult>(verdict_.failed(offset, std::move(detail)));
    }

    // The payload of the length-prefixed segment starting after its marker.
    Outcome readSegment(std::uint8_t marker, std::vector<std::uint8_t>& payload);
    Outcome defineTables(std::uint64_t at, const std::vector<std::uint8_t>& payload);
    Outcome defineQuantization(std::uint64_t at, const std::vector<std::uint8_t>& payload);
    Outcome startFrame(std::uint8_t marker, std::uint64_t at, const std::vector<std::uint8_t>& payload);
    Outcome readScan(std::uint64_t at, const std::vector<std::uint8_t>& payload, Scan& scan);
    Outcome decodeScan(const Scan& scan, std::uint64_t at, std::optional<std::uint8_t>& nextMarker);

    // The blocks of one scan, bits taken from `bits`; empty when they decode.
    std::optional<std::string> sequentialBlock(EntropyBits& bits, const ScanComponent& component);
    std::optional<std::string> dcFirstBlock(EntropyBits& bits, const ScanComponent& component, const Scan& scan);
    std::optional<std::string> acFirstBlock(EntropyBits& bits, const ScanComponent& component, const Scan& scan,
                                            std::uint64_t block);
    std::optional<std::string> acRefineBlock(EntropyBits& bits, const ScanComponent& component, const Scan& scan,
                                             std::uint64_t block);

    carving::IContentReader& content_;
    const MediaLimits& limits_;
    ContentBytes bytes_;
    MediaVerdict verdict_;

    std::array<HuffmanTable, 4> dcTables_{};
    std::array<HuffmanTable, 4> acTables_{};
    std::array<bool, 4> quantDefined_{};
    bool frameSeen_ = false;
    bool progressive_ = false;
    std::uint8_t frameMarker_ = 0;
    std::uint32_t precision_ = 8;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<Component> components_;
    std::uint32_t hMax_ = 1;
    std::uint32_t vMax_ = 1;
    std::uint64_t mcusWide_ = 0;
    std::uint64_t mcusHigh_ = 0;
    std::uint32_t restartInterval_ = 0;
    std::uint64_t eobRun_ = 0;

    // What was decoded.
    std::uint64_t scans_ = 0;
    std::uint64_t mcus_ = 0;
    std::uint64_t restarts_ = 0;
    std::uint64_t extraneous_ = 0;
    std::uint64_t work_ = 0;
    std::uint64_t memory_ = 0;
    bool defaultTables_ = false;
};

JpegDecoder::Outcome JpegDecoder::readSegment(std::uint8_t marker, std::vector<std::uint8_t>& payload) {
    std::uint8_t high = 0;
    std::uint8_t low = 0;
    if (!bytes_.next(high) || !bytes_.next(low)) {
        return ended(markerName(marker) + "'s length");
    }
    const std::uint32_t length = (std::uint32_t{high} << 8) | low;
    if (length < 2) {
        return failed(bytes_.position() - 2, markerName(marker) + " has a length of " + std::to_string(length));
    }
    payload.resize(length - 2);
    for (std::uint8_t& value : payload) {
        if (!bytes_.next(value)) {
            return ended(markerName(marker));
        }
    }
    return std::nullopt;
}

JpegDecoder::Outcome JpegDecoder::defineTables(std::uint64_t at, const std::vector<std::uint8_t>& payload) {
    std::size_t p = 0;
    while (p < payload.size()) {
        const std::uint8_t classAndId = payload[p];
        const std::uint32_t tableClass = classAndId >> 4;
        const std::uint32_t id = classAndId & 0x0F;
        if (tableClass > 1 || id > 3 || payload.size() - p < 17) {
            return failed(at, "DHT defines table class " + std::to_string(tableClass) + " id " + std::to_string(id));
        }
        const std::span<const std::uint8_t> counts(payload.data() + p + 1, 16);
        std::size_t total = 0;
        for (const std::uint8_t count : counts) {
            total += count;
        }
        if (total > 256 || payload.size() - p - 17 < total) {
            return failed(at, "DHT holds a table of " + std::to_string(total) + " codes");
        }
        const std::span<const std::uint8_t> values(payload.data() + p + 17, total);
        HuffmanTable& table = tableClass == 0 ? dcTables_[id] : acTables_[id];
        if (!table.build(counts, values)) {
            return failed(at, "DHT table " + std::to_string(id) + ": the code lengths do not make a prefix code");
        }
        if (tableClass == 0 && table.largestValue() > 15) {
            return failed(at, "DHT: a DC table with the symbol " + std::to_string(table.largestValue()));
        }
        p += 17 + total;
    }
    return std::nullopt;
}

JpegDecoder::Outcome JpegDecoder::defineQuantization(std::uint64_t at, const std::vector<std::uint8_t>& payload) {
    std::size_t p = 0;
    while (p < payload.size()) {
        const std::uint32_t precision = payload[p] >> 4;
        const std::uint32_t id = payload[p] & 0x0F;
        const std::size_t size = 1 + 64 * (precision == 0 ? 1 : 2);
        if (precision > 1 || id > 3 || payload.size() - p < size) {
            return failed(at, "DQT defines a table of precision " + std::to_string(precision) + " id " +
                                  std::to_string(id));
        }
        quantDefined_[id] = true;
        p += size;
    }
    return std::nullopt;
}

JpegDecoder::Outcome JpegDecoder::startFrame(std::uint8_t marker, std::uint64_t at,
                                             const std::vector<std::uint8_t>& payload) {
    if (frameSeen_) {
        return failed(at, "a second frame header");
    }
    if (payload.size() < 6) {
        return failed(at, "the frame header is too short");
    }
    frameSeen_ = true;
    frameMarker_ = marker;
    progressive_ = marker == kSof2;
    precision_ = payload[0];
    height_ = (std::uint32_t{payload[1]} << 8) | payload[2];
    width_ = (std::uint32_t{payload[3]} << 8) | payload[4];
    const std::uint32_t count = payload[5];
    if (count > 4 && payload.size() == 6 + 3 * std::size_t{count}) {
        return Result<LevelResult>(verdict_.unsupported(std::to_string(count) + " components (at most 4 are decoded)"));
    }
    if ((precision_ != 8 && !(precision_ == 12 && marker != kSof0)) || width_ == 0 || count == 0 || count > 4 ||
        payload.size() != 6 + 3 * std::size_t{count}) {
        return failed(at, "the frame header is not valid (precision " + std::to_string(precision_) + ", " +
                              std::to_string(width_) + " wide, " + std::to_string(count) + " components)");
    }
    if (height_ == 0) {
        return Result<LevelResult>(verdict_.unsupported("the number of lines is given by a DNL segment"));
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        Component component;
        component.id = payload[6 + 3 * i];
        component.h = payload[7 + 3 * i] >> 4;
        component.v = payload[7 + 3 * i] & 0x0F;
        component.quantTable = payload[8 + 3 * i];
        component.coefficientBits.fill(-1);
        if (component.h < 1 || component.h > 4 || component.v < 1 || component.v > 4 || component.quantTable > 3) {
            return failed(at, "component " + std::to_string(component.id) + " has sampling " +
                                  std::to_string(component.h) + "x" + std::to_string(component.v) +
                                  " and quantization table " + std::to_string(component.quantTable));
        }
        for (const Component& other : components_) {
            if (other.id == component.id) {
                return failed(at, "two components with the id " + std::to_string(component.id));
            }
        }
        hMax_ = std::max(hMax_, component.h);
        vMax_ = std::max(vMax_, component.v);
        components_.push_back(component);
    }
    mcusWide_ = (width_ + 8ULL * hMax_ - 1) / (8ULL * hMax_);
    mcusHigh_ = (height_ + 8ULL * vMax_ - 1) / (8ULL * vMax_);
    for (Component& component : components_) {
        const std::uint64_t samplesWide = (std::uint64_t{width_} * component.h + hMax_ - 1) / hMax_;
        const std::uint64_t samplesHigh = (std::uint64_t{height_} * component.v + vMax_ - 1) / vMax_;
        component.blocksWide = (samplesWide + 7) / 8;
        component.blocksHigh = (samplesHigh + 7) / 8;
    }
    return std::nullopt;
}

JpegDecoder::Outcome JpegDecoder::readScan(std::uint64_t at, const std::vector<std::uint8_t>& payload, Scan& scan) {
    if (!frameSeen_) {
        return failed(at, "a scan before the frame header");
    }
    if (payload.empty() || payload[0] < 1 || payload[0] > 4 || payload.size() != 4 + 2 * std::size_t{payload[0]}) {
        return failed(at, "the scan header is not valid");
    }
    const std::size_t count = payload[0];
    std::size_t blocks = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t id = payload[1 + 2 * i];
        std::size_t index = components_.size();
        for (std::size_t c = 0; c < components_.size(); ++c) {
            if (components_[c].id == id) {
                index = c;
            }
        }
        if (index == components_.size()) {
            return failed(at, "the scan codes component " + std::to_string(id) + ", which the frame does not have");
        }
        for (const ScanComponent& other : scan.components) {
            if (other.component == index) {
                return failed(at, "the scan codes component " + std::to_string(id) + " twice");
            }
        }
        const std::uint8_t tables = payload[2 + 2 * i];
        if ((tables >> 4) > 3 || (tables & 0x0F) > 3) {
            return failed(at, "the scan names Huffman table " + std::to_string(tables >> 4) + "/" +
                                  std::to_string(tables & 0x0F));
        }
        if (!quantDefined_[components_[index].quantTable]) {
            return failed(at, "component " + std::to_string(id) + "'s quantization table " +
                                  std::to_string(components_[index].quantTable) + " is not defined");
        }
        scan.components.push_back(ScanComponent{index, static_cast<std::uint8_t>(tables >> 4),
                                                 static_cast<std::uint8_t>(tables & 0x0F)});
        blocks += std::size_t{components_[index].h} * components_[index].v;
    }
    if (count > 1 && blocks > kMaxBlocksInMcu) {
        return failed(at, "an MCU of " + std::to_string(blocks) + " blocks (at most 10)");
    }
    scan.ss = payload[1 + 2 * count];
    scan.se = payload[2 + 2 * count];
    scan.ah = payload[3 + 2 * count] >> 4;
    scan.al = payload[3 + 2 * count] & 0x0F;

    if (!progressive_) {
        if (scan.ss != 0 || scan.se != 63 || scan.ah != 0 || scan.al != 0) {
            return failed(at, "a sequential scan with spectral selection or successive approximation");
        }
        for (const ScanComponent& component : scan.components) {
            if (components_[component.component].coded) {
                return failed(at, "component " + std::to_string(components_[component.component].id) +
                                      " is coded by two scans");
            }
            components_[component.component].coded = true;
        }
        return std::nullopt;
    }
    // Progressive (G.1.1.1.1): the bands, then what each scan refines.
    const bool dc = scan.ss == 0;
    if ((dc && scan.se != 0) || (!dc && (scan.ss > scan.se || scan.se > 63 || count != 1)) ||
        (scan.ah != 0 && scan.al != scan.ah - 1) || scan.al > 13) {
        return failed(at, "a progressive scan with Ss " + std::to_string(scan.ss) + ", Se " + std::to_string(scan.se) +
                              ", Ah " + std::to_string(scan.ah) + ", Al " + std::to_string(scan.al) + " for " +
                              std::to_string(count) + " components");
    }
    for (const ScanComponent& scanned : scan.components) {
        Component& component = components_[scanned.component];
        if (!dc && component.coefficientBits[0] < 0) {
            return failed(at, "component " + std::to_string(component.id) + " gets AC coefficients before its DC");
        }
        for (std::uint32_t k = scan.ss; k <= scan.se; ++k) {
            const std::int32_t coded = component.coefficientBits[k];
            if ((scan.ah == 0 && coded >= 0) || (scan.ah != 0 && coded != static_cast<std::int32_t>(scan.ah))) {
                return failed(at, "component " + std::to_string(component.id) + "'s coefficient " +
                                      std::to_string(k) + " is " +
                                      (scan.ah == 0 ? "coded again" : "refined out of order"));
            }
            component.coefficientBits[k] = static_cast<std::int32_t>(scan.al);
        }
    }
    return std::nullopt;
}

std::optional<std::string> JpegDecoder::sequentialBlock(EntropyBits& bits, const ScanComponent& component) {
    const std::uint32_t dcLimit = precision_ == 8 ? 11 : 15;
    const std::uint32_t acLimit = precision_ == 8 ? 10 : 14;
    const int dc = dcTables_[component.dcTable].decode(bits);
    if (dc < 0) {
        return "an invalid DC code";
    }
    if (static_cast<std::uint32_t>(dc) > dcLimit) {
        return "a DC difference of category " + std::to_string(dc);
    }
    static_cast<void>(bits.read(static_cast<unsigned>(dc)));
    const HuffmanTable& ac = acTables_[component.acTable];
    for (std::uint32_t k = 1; k < 64;) {
        const int symbol = ac.decode(bits);
        if (symbol < 0) {
            return "an invalid AC code";
        }
        const std::uint32_t run = static_cast<std::uint32_t>(symbol) >> 4;
        const std::uint32_t size = static_cast<std::uint32_t>(symbol) & 0x0F;
        if (size == 0) {
            if (run != 15) {
                break;
            }
            k += 16;
            if (k > 64) {
                return "a run of zeros beyond the block's 64 coefficients";
            }
            continue;
        }
        k += run;
        if (k > 63) {
            return "an AC coefficient at index " + std::to_string(k);
        }
        if (size > acLimit) {
            return "an AC coefficient of category " + std::to_string(size);
        }
        static_cast<void>(bits.read(size));
        ++k;
    }
    return std::nullopt;
}

std::optional<std::string> JpegDecoder::dcFirstBlock(EntropyBits& bits, const ScanComponent& component,
                                                     const Scan& scan) {
    if (scan.ah != 0) {
        static_cast<void>(bits.read(1));
        return std::nullopt;
    }
    const int dc = dcTables_[component.dcTable].decode(bits);
    if (dc < 0) {
        return "an invalid DC code";
    }
    if (static_cast<std::uint32_t>(dc) > (precision_ == 8 ? 11U : 15U)) {
        return "a DC difference of category " + std::to_string(dc);
    }
    static_cast<void>(bits.read(static_cast<unsigned>(dc)));
    return std::nullopt;
}

std::optional<std::string> JpegDecoder::acFirstBlock(EntropyBits& bits, const ScanComponent& component,
                                                     const Scan& scan, std::uint64_t block) {
    if (eobRun_ > 0) {
        --eobRun_;
        return std::nullopt;
    }
    std::uint64_t& nonZero = components_[component.component].nonZero[static_cast<std::size_t>(block)];
    const HuffmanTable& ac = acTables_[component.acTable];
    const std::uint32_t acLimit = precision_ == 8 ? 10 : 14;
    for (std::uint32_t k = scan.ss; k <= scan.se;) {
        const int symbol = ac.decode(bits);
        if (symbol < 0) {
            return "an invalid AC code";
        }
        const std::uint32_t run = static_cast<std::uint32_t>(symbol) >> 4;
        const std::uint32_t size = static_cast<std::uint32_t>(symbol) & 0x0F;
        if (size != 0) {
            k += run;
            if (k > scan.se) {
                return "an AC coefficient at index " + std::to_string(k) + " beyond the band";
            }
            if (size > acLimit) {
                return "an AC coefficient of category " + std::to_string(size);
            }
            static_cast<void>(bits.read(size));
            nonZero |= std::uint64_t{1} << k;
            ++k;
        } else if (run == 15) {
            k += 16;
            if (k > scan.se + 1) {
                return "a run of zeros beyond the band";
            }
        } else {
            eobRun_ = (std::uint64_t{1} << run) - 1 + bits.read(run);
            break;
        }
    }
    return std::nullopt;
}

std::optional<std::string> JpegDecoder::acRefineBlock(EntropyBits& bits, const ScanComponent& component,
                                                      const Scan& scan, std::uint64_t block) {
    std::uint64_t& nonZero = components_[component.component].nonZero[static_cast<std::size_t>(block)];
    const HuffmanTable& ac = acTables_[component.acTable];
    std::uint32_t k = scan.ss;
    if (eobRun_ == 0) {
        for (; k <= scan.se; ++k) {
            const int symbol = ac.decode(bits);
            if (symbol < 0) {
                return "an invalid AC code";
            }
            std::uint32_t run = static_cast<std::uint32_t>(symbol) >> 4;
            const std::uint32_t size = static_cast<std::uint32_t>(symbol) & 0x0F;
            const bool newCoefficient = size != 0;
            if (newCoefficient) {
                if (size != 1) {
                    return "a refinement coefficient of category " + std::to_string(size);
                }
                static_cast<void>(bits.read(1));
            } else if (run != 15) {
                eobRun_ = (std::uint64_t{1} << run) + bits.read(run);
                break;
            }
            // Past the coefficients already not zero (a correction bit each) and
            // `run` zero ones, to the one the code is about.
            while (k <= scan.se) {
                if ((nonZero & (std::uint64_t{1} << k)) != 0) {
                    static_cast<void>(bits.read(1));
                } else {
                    if (run == 0) {
                        break;
                    }
                    --run;
                }
                ++k;
            }
            if (k > scan.se) {
                return "a refinement run beyond the band";
            }
            if (newCoefficient) {
                nonZero |= std::uint64_t{1} << k;
            }
        }
    }
    if (eobRun_ > 0) {
        for (; k <= scan.se; ++k) {
            if ((nonZero & (std::uint64_t{1} << k)) != 0) {
                static_cast<void>(bits.read(1));
            }
        }
        --eobRun_;
    }
    return std::nullopt;
}

JpegDecoder::Outcome JpegDecoder::decodeScan(const Scan& scan, std::uint64_t at,
                                             std::optional<std::uint8_t>& nextMarker) {
    const bool dcScan = !progressive_ || scan.ss == 0;
    const bool acScan = !progressive_ || scan.ss > 0;
    const bool needsDc = !progressive_ || (scan.ss == 0 && scan.ah == 0);
    for (const ScanComponent& component : scan.components) {
        if ((needsDc && !dcTables_[component.dcTable].defined()) ||
            (acScan && !acTables_[component.acTable].defined())) {
            return failed(at, "the scan uses a Huffman table that is not defined");
        }
    }
    // The MCUs: one block each for a scan of one component, the components'
    // sampling factors otherwise.
    const bool single = scan.components.size() == 1;
    const Component& first = components_[scan.components.front().component];
    const std::uint64_t total = single ? first.blocksWide * first.blocksHigh : mcusWide_ * mcusHigh_;
    std::uint64_t blocksPerMcu = 0;
    for (const ScanComponent& component : scan.components) {
        const Component& coded = components_[component.component];
        blocksPerMcu += single ? 1 : std::uint64_t{coded.h} * coded.v;
    }
    const std::uint64_t work = total * blocksPerMcu * 64;
    if (work > limits_.maxDecodedBytes - work_) {
        return Result<LevelResult>(verdict_.unsupported("the scans visit more blocks than the decoding limit (" +
                                                        describeBytes(limits_.maxDecodedBytes) + ") allows"));
    }
    work_ += work;
    if (progressive_ && scan.ss > 0) {
        Component& component = components_[scan.components.front().component];
        if (component.nonZero.empty()) {
            const std::uint64_t bytes = component.blocksWide * component.blocksHigh * sizeof(std::uint64_t);
            if (bytes > limits_.maxMemory - memory_) {
                const std::string limit = describeBytes(limits_.maxMemory);
                return Result<LevelResult>(
                    verdict_.unsupported("the progressive image needs more memory than the limit (" + limit + ")"));
            }
            memory_ += bytes;
            component.nonZero.assign(static_cast<std::size_t>(component.blocksWide * component.blocksHigh), 0);
        }
    }

    EntropyBits bits(bytes_);
    eobRun_ = 0;
    std::uint32_t restart = 0;
    const auto mcuName = [&](std::uint64_t mcu) {
        return "scan " + std::to_string(scans_ + 1) + ", MCU " + std::to_string(mcu) + " of " + std::to_string(total);
    };
    for (std::uint64_t mcu = 0; mcu < total; ++mcu) {
        if (restartInterval_ != 0 && mcu != 0 && mcu % restartInterval_ == 0) {
            const EntropyBits::SegmentEnd end = bits.finish();
            extraneous_ += end.extraneous;
            if (!end.marker.has_value()) {
                return ended("the entropy-coded data, before restart marker " + std::to_string(restart % 8) + " (" +
                             mcuName(mcu) + ")");
            }
            if (end.extraneous > kMaxExtraneousBytes) {
                return failed(end.markerOffset, mcuName(mcu) + ": " + std::to_string(end.extraneous) +
                                                    " bytes of entropy-coded data after the restart interval's "
                                                    "last MCU");
            }
            if (*end.marker != kRst0 + restart % 8) {
                return failed(end.markerOffset, mcuName(mcu) + ": " + markerName(*end.marker) + " where RST" +
                                                    std::to_string(restart % 8) + " is due");
            }
            ++restart;
            ++restarts_;
            eobRun_ = 0;
        }
        const std::uint64_t mcuStart = bytes_.position();
        std::optional<std::string> problem;
        if (single) {
            const ScanComponent& component = scan.components.front();
            if (!progressive_) {
                problem = sequentialBlock(bits, component);
            } else if (dcScan) {
                problem = dcFirstBlock(bits, component, scan);
            } else if (scan.ah == 0) {
                problem = acFirstBlock(bits, component, scan, mcu);
            } else {
                problem = acRefineBlock(bits, component, scan, mcu);
            }
        } else {
            for (const ScanComponent& component : scan.components) {
                const std::uint64_t blocks =
                    std::uint64_t{components_[component.component].h} * components_[component.component].v;
                for (std::uint64_t b = 0; b < blocks && !problem.has_value(); ++b) {
                    problem = progressive_ ? dcFirstBlock(bits, component, scan) : sequentialBlock(bits, component);
                }
                if (problem.has_value()) {
                    break;
                }
            }
        }
        if (bits.overrun()) {
            if (bits.contentEnded()) {
                return ended("the entropy-coded data (" + mcuName(mcu) + ")");
            }
            return failed(mcuStart, mcuName(mcu) + ": the entropy-coded data ends before the MCU does");
        }
        if (problem.has_value()) {
            return failed(mcuStart, mcuName(mcu) + ": " + *problem);
        }
        ++mcus_;
    }
    const EntropyBits::SegmentEnd end = bits.finish();
    extraneous_ += end.extraneous;
    if (end.marker.has_value() && end.extraneous > kMaxExtraneousBytes) {
        return failed(end.markerOffset, "scan " + std::to_string(scans_ + 1) + ": " + std::to_string(end.extraneous) +
                                            " bytes of entropy-coded data after the last MCU");
    }
    if (eobRun_ > 0) {
        return failed(at, "scan " + std::to_string(scans_ + 1) + ": an end-of-band run reaches " +
                              std::to_string(eobRun_) + " blocks beyond the last one");
    }
    nextMarker = end.marker;
    if (!nextMarker.has_value()) {
        return ended("the data after scan " + std::to_string(scans_ + 1) + " (no EOI)");
    }
    ++scans_;
    return std::nullopt;
}

Result<LevelResult> JpegDecoder::run() {
    std::uint8_t first = 0;
    std::uint8_t second = 0;
    if (!bytes_.next(first) || !bytes_.next(second)) {
        return *ended("SOI");
    }
    if (first != 0xFF || second != kSoi) {
        return verdict_.failed(0, "no SOI marker");
    }
    std::optional<std::uint8_t> pending;
    std::vector<std::uint8_t> payload;
    for (;;) {
        std::uint8_t marker = 0;
        std::uint64_t at = 0;
        if (pending.has_value()) {
            marker = *pending;
            at = bytes_.position() - 2;
            pending.reset();
        } else {
            std::uint8_t prefix = 0;
            if (!bytes_.next(prefix)) {
                return *ended("the marker after " + std::to_string(scans_) + " scans");
            }
            at = bytes_.position() - 1;
            if (prefix != 0xFF) {
                return verdict_.failed(at, "data where a marker is due");
            }
            marker = 0xFF;
            while (marker == 0xFF) {
                if (!bytes_.next(marker)) {
                    return *ended("a marker");
                }
            }
        }
        if (marker == kEoi) {
            break;
        }
        if (marker == kSoi || marker == 0x00) {
            return verdict_.failed(at, markerName(marker) + " out of place");
        }
        if ((marker >= kRst0 && marker <= kRst7) || marker == 0x01) {
            continue;  // parameterless markers between segments, which libjpeg skips too
        }
        if (marker == kDac || (marker >= 0xC3 && marker <= 0xCF && marker != kDht)) {
            return verdict_.unsupported(markerName(marker) + ": arithmetic coding, lossless and hierarchical "
                                                             "frames are not decoded");
        }
        if (Outcome outcome = readSegment(marker, payload); outcome.has_value()) {
            return std::move(*outcome);
        }
        Outcome outcome;
        switch (marker) {
        case kSof0:
        case kSof1:
        case kSof2:
            outcome = startFrame(marker, at, payload);
            break;
        case kDht:
            outcome = defineTables(at, payload);
            break;
        case kDqt:
            outcome = defineQuantization(at, payload);
            break;
        case kDri:
            if (payload.size() != 2) {
                outcome = failed(at, "DRI is not 4 bytes long");
            } else {
                restartInterval_ = (std::uint32_t{payload[0]} << 8) | payload[1];
            }
            break;
        case kDnl:
            outcome = Result<LevelResult>(verdict_.unsupported("DNL segments are not supported"));
            break;
        case kSos: {
            // Tables 0 and 1 that are missing when decoding starts are those of
            // Annex K.3 (libjpeg-turbo's rule, for Motion JPEG).
            if (scans_ == 0) {
                const auto standard = [&](HuffmanTable& table, std::span<const std::uint8_t> counts,
                                          std::span<const std::uint8_t> values) {
                    if (!table.defined()) {
                        static_cast<void>(table.build(counts, values));
                        defaultTables_ = true;
                    }
                };
                standard(dcTables_[0], kDcLuminanceCounts, kDcLuminanceValues);
                standard(dcTables_[1], kDcChrominanceCounts, kDcChrominanceValues);
                standard(acTables_[0], kAcLuminanceCounts, kAcLuminanceValues);
                standard(acTables_[1], kAcChrominanceCounts, kAcChrominanceValues);
            }
            Scan scan;
            outcome = readScan(at, payload, scan);
            if (!outcome.has_value()) {
                outcome = decodeScan(scan, at, pending);
            }
            break;
        }
        default:
            break;  // APPn, COM and the like: nothing coded
        }
        if (outcome.has_value()) {
            return std::move(*outcome);
        }
    }
    if (!frameSeen_ || scans_ == 0) {
        return verdict_.failed(0, "no frame or no scan before EOI");
    }
    if (!progressive_) {
        for (const Component& component : components_) {
            if (!component.coded) {
                return verdict_.failed(0, "component " + std::to_string(component.id) + " is never coded");
            }
        }
    }
    std::string kind = frameMarker_ == kSof0 ? "baseline" : frameMarker_ == kSof1 ? "extended" : "progressive";
    std::string detail = kind + ", " + std::to_string(precision_) + "-bit, " + std::to_string(width_) + "x" +
                         std::to_string(height_) + ", " + std::to_string(components_.size()) +
                         (components_.size() == 1 ? " component: " : " components: ") + std::to_string(scans_) +
                         (scans_ == 1 ? " scan, " : " scans, ") + std::to_string(mcus_) + " MCUs decoded";
    if (restarts_ > 0) {
        detail += ", " + std::to_string(restarts_) + " restart markers";
    }
    if (defaultTables_) {
        detail += ", the standard Huffman tables";
    }
    if (extraneous_ > 0) {
        detail += "; " + std::to_string(extraneous_) + " extraneous bytes before markers";
    }
    return verdict_.passed(std::move(detail));
}

class JpegMediaValidator final : public IMediaValidator {
public:
    [[nodiscard]] std::string_view formatId() const noexcept override { return "jpeg"; }

    [[nodiscard]] Result<LevelResult> validate(carving::IContentReader& content,
                                               const MediaLimits& limits) const override {
        const auto decoder = std::make_unique<JpegDecoder>(content, limits);
        return decoder->run();
    }
};

}  // namespace

std::shared_ptr<const IMediaValidator> makeJpegMediaValidator() {
    return std::make_shared<JpegMediaValidator>();
}

}  // namespace recovery::validation::detail
