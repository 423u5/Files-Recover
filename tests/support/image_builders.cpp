#include "support/image_builders.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/crc32.hpp"
#include "support/image_samples.hpp"
#include "support/test_files.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace recovery::test {

namespace {

// ---------------------------------------------------------------------------
// Byte helpers
// ---------------------------------------------------------------------------

void put8(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFF));
}

void putBe16(std::vector<std::byte>& out, std::uint32_t value) {
    put8(out, value >> 8);
    put8(out, value);
}

void putBe32(std::vector<std::byte>& out, std::uint32_t value) {
    putBe16(out, value >> 16);
    putBe16(out, value & 0xFFFF);
}

void putLe16(std::vector<std::byte>& out, std::uint32_t value) {
    put8(out, value);
    put8(out, value >> 8);
}

void putLe24(std::vector<std::byte>& out, std::uint32_t value) {
    putLe16(out, value & 0xFFFF);
    put8(out, value >> 16);
}

void putLe32(std::vector<std::byte>& out, std::uint32_t value) {
    putLe16(out, value & 0xFFFF);
    putLe16(out, value >> 16);
}

void putText(std::vector<std::byte>& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
}

void append(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

std::uint8_t u8(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint8_t>(bytes[offset]);
}

// A smooth test picture with some texture: gradients, a wave and seeded noise.
struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb picture(int x, int y, int width, int height, std::uint64_t seed) {
    const auto s = static_cast<int>(seed % 97);
    const int r = 255 * x / std::max(1, width - 1);
    const int g = 255 * y / std::max(1, height - 1);
    const int b = static_cast<int>(127 + 120 * std::sin((x + y + s) / 5.0));
    const int noise = static_cast<int>((static_cast<unsigned>(x * 73 + y * 151 + s * 31) * 2654435761U) >> 28) - 8;
    return Rgb{std::clamp(r + noise, 0, 255), std::clamp(g - noise, 0, 255), std::clamp(b + noise, 0, 255)};
}

// ---------------------------------------------------------------------------
// JPEG encoder
// ---------------------------------------------------------------------------

constexpr std::array<int, 64> kZigzag = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                         12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                         35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                         58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// ITU T.81 Annex K example tables (natural order).
constexpr std::array<int, 64> kLuminance = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                            14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                            18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                            49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr std::array<int, 64> kChrominance = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                              24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                              99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                              99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// Huffman tables of fixed-length codes: every DC category (0-11) gets a
// 4-bit code, every AC symbol (EOB, ZRL, run/size for sizes 1-10) an 8-bit
// code. Valid canonical tables that any decoder accepts; the code of a
// symbol is its position in the table.
std::vector<std::uint8_t> dcSymbols() {
    std::vector<std::uint8_t> symbols;
    for (std::uint8_t category = 0; category <= 11; ++category) {
        symbols.push_back(category);
    }
    return symbols;
}

std::vector<std::uint8_t> acSymbols() {
    std::vector<std::uint8_t> symbols = {0x00, 0xF0};
    for (int size = 1; size <= 10; ++size) {
        for (int run = 0; run < 16; ++run) {
            symbols.push_back(static_cast<std::uint8_t>((run << 4) | size));
        }
    }
    return symbols;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<std::byte>& out) : out_(out) {}

    void put(std::uint32_t value, int length) {
        for (int bit = length - 1; bit >= 0; --bit) {
            accumulator_ = (accumulator_ << 1) | ((value >> bit) & 1);
            if (++bits_ == 8) {
                emit();
            }
        }
    }

    // Pads the last byte with 1-bits.
    void flush() {
        while (bits_ != 0) {
            put(1, 1);
        }
    }

private:
    void emit() {
        const auto byte = static_cast<std::uint8_t>(accumulator_ & 0xFF);
        out_.push_back(static_cast<std::byte>(byte));
        if (byte == 0xFF) {
            out_.push_back(std::byte{0x00});  // byte stuffing
        }
        accumulator_ = 0;
        bits_ = 0;
    }

    std::vector<std::byte>& out_;
    std::uint32_t accumulator_ = 0;
    int bits_ = 0;
};

// The magnitude category of a DC difference (at most 11) or AC coefficient (at most 10).
int category(int value, int limit) {
    int magnitude = value < 0 ? -value : value;
    int bits = 0;
    while (magnitude != 0) {
        ++bits;
        magnitude >>= 1;
    }
    if (bits > limit) {
        throw std::invalid_argument("JPEG coefficient beyond the Huffman tables (quality too high)");
    }
    return bits;
}

// The low `category` bits that encode `value` after its category's code.
std::uint32_t magnitudeBits(int value, int bits) {
    return static_cast<std::uint32_t>(value >= 0 ? value : value + (1 << bits) - 1);
}

using Block = std::array<int, 64>;  // quantized coefficients in zigzag order

struct JpegComponent {
    std::uint8_t id = 1;
    int h = 1;
    int v = 1;
    int table = 0;
    // Blocks of the padded plane (MCU-aligned), row by row.
    int blocksWide = 0;
    int blocksHigh = 0;
    std::vector<Block> blocks;
    // Blocks a non-interleaved scan codes.
    int scanBlocksWide = 0;
    int scanBlocksHigh = 0;
};

// A TIFF structure for an Exif block (P17): IFD0 (make, model, orientation,
// the Exif IFD's offset), the Exif IFD (date taken and its offset), IFD1 (a
// JPEG thumbnail), then the values that do not fit in their entries, then
// the thumbnail. Entries are in tag order, as TIFF requires.
std::vector<std::byte> exifTiff(const JpegOptions& options, std::span<const std::byte> thumbnail) {
    const bool big = options.exifBigEndian;
    const auto put16 = [&](std::vector<std::byte>& out, std::uint32_t value) {
        big ? putBe16(out, value) : putLe16(out, value);
    };
    const auto put32 = [&](std::vector<std::byte>& out, std::uint32_t value) {
        big ? putBe32(out, value) : putLe32(out, value);
    };
    struct Entry {
        std::uint16_t tag;
        std::uint16_t type;
        std::uint32_t count;
        // ASCII text (written with its NUL), or a number.
        std::string text;
        std::uint32_t number;
    };
    const auto ascii = [](std::uint16_t tag, const std::string& text) {
        return Entry{tag, 2, static_cast<std::uint32_t>(text.size() + 1), text, 0};
    };
    std::vector<Entry> ifd0;
    std::vector<Entry> exif;
    std::vector<Entry> ifd1;
    if (!options.exifMake.empty()) {
        ifd0.push_back(ascii(0x010F, options.exifMake));
    }
    if (!options.exifModel.empty()) {
        ifd0.push_back(ascii(0x0110, options.exifModel));
    }
    if (options.exifOrientation != 0) {
        ifd0.push_back(Entry{0x0112, 3, 1, {}, options.exifOrientation});
    }
    if (!options.exifDateTaken.empty()) {
        exif.push_back(ascii(0x9003, options.exifDateTaken));
        if (!options.exifOffsetTime.empty()) {
            exif.push_back(ascii(0x9011, options.exifOffsetTime));
        }
        ifd0.push_back(Entry{0x8769, 4, 1, {}, 0});  // the Exif IFD's offset, set below
    }
    if (!thumbnail.empty()) {
        ifd1.push_back(Entry{0x0103, 3, 1, {}, 6});
        ifd1.push_back(Entry{0x0201, 4, 1, {}, 0});  // the thumbnail's offset, set below
        ifd1.push_back(Entry{0x0202, 4, 1, {}, static_cast<std::uint32_t>(thumbnail.size())});
    }
    const auto ifdSize = [](const std::vector<Entry>& entries) {
        return entries.empty() ? 0U : static_cast<std::uint32_t>(2 + 12 * entries.size() + 4);
    };
    const std::uint32_t ifd0At = 8;
    const std::uint32_t exifAt = ifd0At + std::max(ifdSize(ifd0), 6U);
    const std::uint32_t ifd1At = exifAt + ifdSize(exif);
    std::uint32_t valuesAt = ifd1At + ifdSize(ifd1);
    std::uint32_t valuesSize = 0;
    for (const std::vector<Entry>* entries : {&ifd0, &exif, &ifd1}) {
        for (const Entry& entry : *entries) {
            if (entry.type == 2 && entry.count > 4) {
                valuesSize += (entry.count + 1) & ~1U;
            }
        }
    }
    const std::uint32_t thumbnailAt = valuesAt + valuesSize;
    for (Entry& entry : ifd0) {
        entry.number = entry.tag == 0x8769 ? exifAt : entry.number;
    }
    for (Entry& entry : ifd1) {
        entry.number = entry.tag == 0x0201 ? thumbnailAt : entry.number;
    }
    std::vector<std::byte> out;
    putText(out, big ? "MM" : "II");
    put16(out, 42);
    put32(out, ifd0At);
    std::vector<std::byte> values;
    const auto writeIfd = [&](const std::vector<Entry>& entries, std::uint32_t next) {
        put16(out, static_cast<std::uint32_t>(entries.size()));
        for (const Entry& entry : entries) {
            put16(out, entry.tag);
            put16(out, entry.type);
            put32(out, entry.count);
            if (entry.type == 2) {
                std::vector<std::byte> text;
                putText(text, entry.text);
                text.push_back(std::byte{0});
                if (text.size() <= 4) {
                    text.resize(4, std::byte{0});
                    append(out, text);
                } else {
                    put32(out, valuesAt + static_cast<std::uint32_t>(values.size()));
                    append(values, text);
                    if ((values.size() & 1U) != 0) {
                        values.push_back(std::byte{0});
                    }
                }
            } else if (entry.type == 3) {
                put16(out, entry.number);
                put16(out, 0);
            } else {
                put32(out, entry.number);
            }
        }
        put32(out, next);
    };
    if (ifd0.empty()) {
        put16(out, 0);
        put32(out, ifd1.empty() ? 0 : ifd1At);
    } else {
        writeIfd(ifd0, ifd1.empty() ? 0 : ifd1At);
    }
    if (!exif.empty()) {
        writeIfd(exif, 0);
    }
    if (!ifd1.empty()) {
        writeIfd(ifd1, 0);
    }
    append(out, values);
    append(out, thumbnail);
    return out;
}

class JpegEncoder {
public:
    explicit JpegEncoder(const JpegOptions& options);

    std::vector<std::byte> encode();

private:
    std::array<int, 64> scaledTable(const std::array<int, 64>& base) const;
    void transform();
    void writeDc(BitWriter& bits, int& predictor, const Block& block) const;
    void writeAc(BitWriter& bits, const Block& block) const;
    void segment(std::vector<std::byte>& out, std::uint8_t marker, std::span<const std::byte> payload) const;
    void marker(std::vector<std::byte>& out, std::uint8_t code) const;
    void scanHeader(std::vector<std::byte>& out, const std::vector<std::size_t>& components, int ss, int se) const;
    void baselineScan(std::vector<std::byte>& out);
    void progressiveScans(std::vector<std::byte>& out);
    std::vector<std::byte> exifSegment() const;

    JpegOptions options_;
    std::vector<JpegComponent> components_;
    int hMax_ = 1;
    int vMax_ = 1;
    int mcusWide_ = 0;
    int mcusHigh_ = 0;
    std::array<std::array<int, 64>, 2> tables_{};
    std::vector<std::uint8_t> dc_ = dcSymbols();
    std::vector<std::uint8_t> ac_ = acSymbols();
    std::array<int, 256> acCode_{};
};

JpegEncoder::JpegEncoder(const JpegOptions& options) : options_(options) {
    if (options.width == 0 || options.height == 0) {
        throw std::invalid_argument("empty JPEG");
    }
    tables_[0] = scaledTable(kLuminance);
    tables_[1] = scaledTable(kChrominance);
    acCode_.fill(-1);
    for (std::size_t i = 0; i < ac_.size(); ++i) {
        acCode_[ac_[i]] = static_cast<int>(i);
    }
    int lumaH = 1;
    int lumaV = 1;
    if (options.sampling == JpegSampling::Yuv422) {
        lumaH = 2;
    } else if (options.sampling == JpegSampling::Yuv420) {
        lumaH = 2;
        lumaV = 2;
    }
    components_.push_back(JpegComponent{1, lumaH, lumaV, 0, 0, 0, {}, 0, 0});
    if (options.sampling != JpegSampling::Gray) {
        components_.push_back(JpegComponent{2, 1, 1, 1, 0, 0, {}, 0, 0});
        components_.push_back(JpegComponent{3, 1, 1, 1, 0, 0, {}, 0, 0});
    }
    hMax_ = lumaH;
    vMax_ = lumaV;
    mcusWide_ = (options.width + 8 * hMax_ - 1) / (8 * hMax_);
    mcusHigh_ = (options.height + 8 * vMax_ - 1) / (8 * vMax_);
    if (components_.size() == 1) {
        // A single-component frame codes 8x8 blocks, whatever its sampling factors.
        mcusWide_ = (options.width + 7) / 8;
        mcusHigh_ = (options.height + 7) / 8;
    }
    transform();
}

std::array<int, 64> JpegEncoder::scaledTable(const std::array<int, 64>& base) const {
    const int quality = std::clamp(options_.quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    std::array<int, 64> table{};
    for (std::size_t i = 0; i < 64; ++i) {
        table[i] = std::clamp((base[i] * scale + 50) / 100, 1, 255);
    }
    return table;
}

void JpegEncoder::transform() {
    const int width = options_.width;
    const int height = options_.height;
    std::array<std::array<double, 8>, 8> cosines{};
    for (int x = 0; x < 8; ++x) {
        for (int u = 0; u < 8; ++u) {
            cosines[x][u] = std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16);
        }
    }
    for (std::size_t c = 0; c < components_.size(); ++c) {
        JpegComponent& component = components_[c];
        const bool single = components_.size() == 1;
        component.blocksWide = single ? mcusWide_ : mcusWide_ * component.h;
        component.blocksHigh = single ? mcusHigh_ : mcusHigh_ * component.v;
        const int planeWidth = (width * component.h + hMax_ - 1) / hMax_;
        const int planeHeight = (height * component.v + vMax_ - 1) / vMax_;
        component.scanBlocksWide = (planeWidth + 7) / 8;
        component.scanBlocksHigh = (planeHeight + 7) / 8;
        const int stepX = hMax_ / component.h;
        const int stepY = vMax_ / component.v;
        // A component sample: the average of the pixels it covers, edges replicated.
        const auto sample = [&](int sx, int sy) {
            double sum = 0;
            for (int dy = 0; dy < stepY; ++dy) {
                for (int dx = 0; dx < stepX; ++dx) {
                    const int x = std::min(sx * stepX + dx, width - 1);
                    const int y = std::min(sy * stepY + dy, height - 1);
                    const Rgb p = picture(x, y, width, height, options_.seed);
                    if (c == 0) {
                        sum += 0.299 * p.r + 0.587 * p.g + 0.114 * p.b;
                    } else if (c == 1) {
                        sum += -0.168736 * p.r - 0.331264 * p.g + 0.5 * p.b + 128;
                    } else {
                        sum += 0.5 * p.r - 0.418688 * p.g - 0.081312 * p.b + 128;
                    }
                }
            }
            return sum / (stepX * stepY);
        };
        const std::array<int, 64>& table = tables_[static_cast<std::size_t>(component.table)];
        component.blocks.resize(static_cast<std::size_t>(component.blocksWide) *
                                static_cast<std::size_t>(component.blocksHigh));
        for (int by = 0; by < component.blocksHigh; ++by) {
            for (int bx = 0; bx < component.blocksWide; ++bx) {
                std::array<double, 64> samples{};
                for (int y = 0; y < 8; ++y) {
                    for (int x = 0; x < 8; ++x) {
                        samples[static_cast<std::size_t>(y * 8 + x)] = sample(bx * 8 + x, by * 8 + y) - 128;
                    }
                }
                Block& block = component.blocks[static_cast<std::size_t>(by * component.blocksWide + bx)];
                for (std::size_t k = 0; k < 64; ++k) {
                    const int natural = kZigzag[k];
                    const int v = natural / 8;
                    const int u = natural % 8;
                    double sum = 0;
                    for (int y = 0; y < 8; ++y) {
                        for (int x = 0; x < 8; ++x) {
                            sum += samples[static_cast<std::size_t>(y * 8 + x)] * cosines[x][u] * cosines[y][v];
                        }
                    }
                    const double cu = u == 0 ? 1 / std::sqrt(2.0) : 1.0;
                    const double cv = v == 0 ? 1 / std::sqrt(2.0) : 1.0;
                    const double coefficient = 0.25 * cu * cv * sum;
                    block[k] = static_cast<int>(std::lround(coefficient / table[static_cast<std::size_t>(natural)]));
                }
            }
        }
    }
}

void JpegEncoder::writeDc(BitWriter& bits, int& predictor, const Block& block) const {
    const int diff = block[0] - predictor;
    predictor = block[0];
    const int size = category(diff, 11);
    bits.put(static_cast<std::uint32_t>(size), 4);
    if (size > 0) {
        bits.put(magnitudeBits(diff, size), size);
    }
}

void JpegEncoder::writeAc(BitWriter& bits, const Block& block) const {
    int run = 0;
    for (std::size_t k = 1; k < 64; ++k) {
        if (block[k] == 0) {
            ++run;
            continue;
        }
        while (run > 15) {
            bits.put(static_cast<std::uint32_t>(acCode_[0xF0]), 8);
            run -= 16;
        }
        const int size = category(block[k], 10);
        bits.put(static_cast<std::uint32_t>(acCode_[static_cast<std::size_t>((run << 4) | size)]), 8);
        bits.put(magnitudeBits(block[k], size), size);
        run = 0;
    }
    if (run > 0) {
        bits.put(static_cast<std::uint32_t>(acCode_[0x00]), 8);
    }
}

void JpegEncoder::marker(std::vector<std::byte>& out, std::uint8_t code) const {
    out.insert(out.end(), options_.fillBytes, std::byte{0xFF});
    put8(out, 0xFF);
    put8(out, code);
}

void JpegEncoder::segment(std::vector<std::byte>& out, std::uint8_t code, std::span<const std::byte> payload) const {
    marker(out, code);
    putBe16(out, static_cast<std::uint32_t>(payload.size() + 2));
    append(out, payload);
}

void JpegEncoder::scanHeader(std::vector<std::byte>& out, const std::vector<std::size_t>& components, int ss,
                             int se) const {
    std::vector<std::byte> payload;
    put8(payload, static_cast<std::uint32_t>(components.size()));
    for (const std::size_t c : components) {
        put8(payload, components_[c].id);
        const auto table = static_cast<std::uint32_t>(components_[c].table);
        put8(payload, (table << 4) | table);
    }
    put8(payload, static_cast<std::uint32_t>(ss));
    put8(payload, static_cast<std::uint32_t>(se));
    put8(payload, 0);
    segment(out, 0xDA, payload);
}

void JpegEncoder::baselineScan(std::vector<std::byte>& out) {
    std::vector<std::size_t> all;
    for (std::size_t c = 0; c < components_.size(); ++c) {
        all.push_back(c);
    }
    scanHeader(out, all, 0, 63);
    BitWriter bits(out);
    std::vector<int> predictors(components_.size(), 0);
    const int mcus = mcusWide_ * mcusHigh_;
    int restarts = 0;
    for (int mcu = 0; mcu < mcus; ++mcu) {
        if (options_.restartInterval != 0 && mcu != 0 && mcu % options_.restartInterval == 0) {
            bits.flush();
            put8(out, 0xFF);
            put8(out, static_cast<std::uint32_t>(0xD0 + restarts++ % 8));
            std::fill(predictors.begin(), predictors.end(), 0);
        }
        const int mx = mcu % mcusWide_;
        const int my = mcu / mcusWide_;
        for (std::size_t c = 0; c < components_.size(); ++c) {
            const JpegComponent& component = components_[c];
            const int h = components_.size() == 1 ? 1 : component.h;
            const int v = components_.size() == 1 ? 1 : component.v;
            for (int y = 0; y < v; ++y) {
                for (int x = 0; x < h; ++x) {
                    const int bx = mx * h + x;
                    const int by = my * v + y;
                    const Block& block = component.blocks[static_cast<std::size_t>(by * component.blocksWide + bx)];
                    writeDc(bits, predictors[c], block);
                    writeAc(bits, block);
                }
            }
        }
    }
    bits.flush();
}

void JpegEncoder::progressiveScans(std::vector<std::byte>& out) {
    // The DC scan: every component, interleaved.
    std::vector<std::size_t> all;
    for (std::size_t c = 0; c < components_.size(); ++c) {
        all.push_back(c);
    }
    scanHeader(out, all, 0, 0);
    {
        BitWriter bits(out);
        std::vector<int> predictors(components_.size(), 0);
        const int mcus = mcusWide_ * mcusHigh_;
        int restarts = 0;
        for (int mcu = 0; mcu < mcus; ++mcu) {
            if (options_.restartInterval != 0 && mcu != 0 && mcu % options_.restartInterval == 0) {
                bits.flush();
                put8(out, 0xFF);
                put8(out, static_cast<std::uint32_t>(0xD0 + restarts++ % 8));
                std::fill(predictors.begin(), predictors.end(), 0);
            }
            const int mx = mcu % mcusWide_;
            const int my = mcu / mcusWide_;
            for (std::size_t c = 0; c < components_.size(); ++c) {
                const JpegComponent& component = components_[c];
                const int h = components_.size() == 1 ? 1 : component.h;
                const int v = components_.size() == 1 ? 1 : component.v;
                for (int y = 0; y < v; ++y) {
                    for (int x = 0; x < h; ++x) {
                        const std::size_t index =
                            static_cast<std::size_t>((my * v + y) * component.blocksWide + mx * h + x);
                        writeDc(bits, predictors[c], component.blocks[index]);
                    }
                }
            }
        }
        bits.flush();
    }
    // One AC scan per component, non-interleaved: one block per MCU, over the
    // component's own blocks. With end-of-band runs of 1 the AC coding is the
    // same as in a sequential scan.
    for (std::size_t c = 0; c < components_.size(); ++c) {
        const JpegComponent& component = components_[c];
        scanHeader(out, {c}, 1, 63);
        BitWriter bits(out);
        int restarts = 0;
        int mcu = 0;
        for (int by = 0; by < component.scanBlocksHigh; ++by) {
            for (int bx = 0; bx < component.scanBlocksWide; ++bx, ++mcu) {
                if (options_.restartInterval != 0 && mcu != 0 && mcu % options_.restartInterval == 0) {
                    bits.flush();
                    put8(out, 0xFF);
                    put8(out, static_cast<std::uint32_t>(0xD0 + restarts++ % 8));
                }
                writeAc(bits, component.blocks[static_cast<std::size_t>(by * component.blocksWide + bx)]);
            }
        }
        bits.flush();
    }
}

std::vector<std::byte> JpegEncoder::exifSegment() const {
    JpegOptions thumbnailOptions;
    thumbnailOptions.width = 16;
    thumbnailOptions.height = 16;
    thumbnailOptions.jfif = false;
    thumbnailOptions.seed = options_.seed + 1000;
    const std::vector<std::byte> thumbnail = makeJpeg(thumbnailOptions);
    std::vector<std::byte> payload;
    putText(payload, std::string_view("Exif\0\0", 6));
    if (options_.exifOrientation != 0 || !options_.exifMake.empty() || !options_.exifModel.empty() ||
        !options_.exifDateTaken.empty() || options_.exifBigEndian) {
        append(payload, exifTiff(options_, options_.exifThumbnail ? std::span<const std::byte>(thumbnail)
                                                                  : std::span<const std::byte>{}));
        return payload;
    }
    // TIFF header, IFD0 without entries, IFD1 with the thumbnail's offset and length.
    putText(payload, "II");
    putLe16(payload, 42);
    putLe32(payload, 8);
    putLe16(payload, 0);   // IFD0: no entries
    putLe32(payload, 14);  // next IFD: IFD1
    putLe16(payload, 2);
    putLe16(payload, 0x0201);  // JPEGInterchangeFormat
    putLe16(payload, 4);       // LONG
    putLe32(payload, 1);
    putLe32(payload, 44);
    putLe16(payload, 0x0202);  // JPEGInterchangeFormatLength
    putLe16(payload, 4);
    putLe32(payload, 1);
    putLe32(payload, static_cast<std::uint32_t>(thumbnail.size()));
    putLe32(payload, 0);  // no further IFD
    append(payload, thumbnail);
    return payload;
}

std::vector<std::byte> JpegEncoder::encode() {
    std::vector<std::byte> out;
    put8(out, 0xFF);
    put8(out, 0xD8);
    if (options_.jfif) {
        std::vector<std::byte> jfif;
        putText(jfif, std::string_view("JFIF\0", 5));
        put8(jfif, 1);
        put8(jfif, 1);
        put8(jfif, 0);
        putBe16(jfif, 1);
        putBe16(jfif, 1);
        put8(jfif, 0);
        put8(jfif, 0);
        segment(out, 0xE0, jfif);
    }
    if (options_.exifThumbnail || options_.exifOrientation != 0 || !options_.exifMake.empty() ||
        !options_.exifModel.empty() || !options_.exifDateTaken.empty()) {
        segment(out, 0xE1, exifSegment());
    }
    if (!options_.comment.empty()) {
        std::vector<std::byte> comment;
        putText(comment, options_.comment);
        segment(out, 0xFE, comment);
    }
    std::vector<std::byte> quantization;
    for (std::size_t t = 0; t < (components_.size() == 1 ? 1U : 2U); ++t) {
        put8(quantization, static_cast<std::uint32_t>(t));
        for (std::size_t k = 0; k < 64; ++k) {
            put8(quantization, static_cast<std::uint32_t>(tables_[t][static_cast<std::size_t>(kZigzag[k])]));
        }
    }
    segment(out, 0xDB, quantization);
    std::vector<std::byte> frame;
    put8(frame, 8);
    putBe16(frame, options_.height);
    putBe16(frame, options_.width);
    put8(frame, static_cast<std::uint32_t>(components_.size()));
    for (const JpegComponent& component : components_) {
        put8(frame, component.id);
        put8(frame, static_cast<std::uint32_t>((component.h << 4) | component.v));
        put8(frame, static_cast<std::uint32_t>(component.table));
    }
    segment(out, options_.progressive ? 0xC2 : 0xC0, frame);
    std::vector<std::byte> huffman;
    for (std::uint32_t t = 0; t < (components_.size() == 1 ? 1U : 2U); ++t) {
        for (const bool isAc : {false, true}) {
            const std::vector<std::uint8_t>& symbols = isAc ? ac_ : dc_;
            put8(huffman, (isAc ? 0x10U : 0x00U) | t);
            for (int length = 1; length <= 16; ++length) {
                put8(huffman, length == (isAc ? 8 : 4) ? static_cast<std::uint32_t>(symbols.size()) : 0);
            }
            for (const std::uint8_t symbol : symbols) {
                put8(huffman, symbol);
            }
        }
    }
    segment(out, 0xC4, huffman);
    if (options_.restartInterval != 0) {
        std::vector<std::byte> interval;
        putBe16(interval, options_.restartInterval);
        segment(out, 0xDD, interval);
    }
    if (options_.progressive) {
        progressiveScans(out);
    } else {
        baselineScan(out);
    }
    marker(out, 0xD9);
    return out;
}

// ---------------------------------------------------------------------------
// zlib (stored blocks) and PNG
// ---------------------------------------------------------------------------

std::uint32_t adler32(std::span<const std::byte> data) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::byte byte : data) {
        a = (a + static_cast<std::uint8_t>(byte)) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

std::vector<std::byte> zlibStored(std::span<const std::byte> data) {
    std::vector<std::byte> out;
    put8(out, 0x78);
    put8(out, 0x01);
    std::size_t position = 0;
    do {
        const std::size_t length = std::min<std::size_t>(65535, data.size() - position);
        const bool last = position + length == data.size();
        put8(out, last ? 1 : 0);
        putLe16(out, static_cast<std::uint32_t>(length));
        putLe16(out, static_cast<std::uint32_t>(~length & 0xFFFF));
        append(out, data.subspan(position, length));
        position += length;
    } while (position < data.size());
    putBe32(out, adler32(data));
    return out;
}

int channels(PngColor color) {
    switch (color) {
    case PngColor::Gray:
    case PngColor::Palette:
        return 1;
    case PngColor::GrayAlpha:
        return 2;
    case PngColor::Rgb:
        return 3;
    case PngColor::Rgba:
        return 4;
    }
    return 1;
}

// Filtered scanlines (filter type 0) of a width x height image or pass.
void appendScanlines(std::vector<std::byte>& raw, std::uint32_t width, std::uint32_t height, int bitsPerPixel,
                     std::uint64_t& state) {
    if (width == 0 || height == 0) {
        return;
    }
    const std::size_t rowBytes = (static_cast<std::size_t>(width) * static_cast<std::size_t>(bitsPerPixel) + 7) / 8;
    for (std::uint32_t y = 0; y < height; ++y) {
        put8(raw, 0);
        const std::vector<std::byte> row = makePattern(rowBytes, state++);
        append(raw, row);
    }
}

// ---------------------------------------------------------------------------
// GIF
// ---------------------------------------------------------------------------

// LZW data for `pixels`, with a clear code often enough that the code width
// never grows: valid for every decoder, if not compressed at all.
std::vector<std::byte> lzw(const std::vector<std::uint8_t>& pixels, int minCodeSize) {
    const std::uint32_t clear = 1U << minCodeSize;
    const std::uint32_t end = clear + 1;
    const int width = minCodeSize + 1;
    const std::size_t literalsPerClear = (std::size_t{1} << minCodeSize) - 2;
    std::vector<std::byte> out;
    std::uint32_t accumulator = 0;
    int bits = 0;
    const auto code = [&](std::uint32_t value) {
        accumulator |= value << bits;
        bits += width;
        while (bits >= 8) {
            put8(out, accumulator & 0xFF);
            accumulator >>= 8;
            bits -= 8;
        }
    };
    std::size_t sinceClear = literalsPerClear;
    for (const std::uint8_t pixel : pixels) {
        if (sinceClear == literalsPerClear) {
            code(clear);
            sinceClear = 0;
        }
        code(pixel);
        ++sinceClear;
    }
    code(end);
    if (bits > 0) {
        put8(out, accumulator & 0xFF);
    }
    return out;
}

void subBlocks(std::vector<std::byte>& out, std::span<const std::byte> data) {
    for (std::size_t position = 0; position < data.size();) {
        const std::size_t length = std::min<std::size_t>(255, data.size() - position);
        put8(out, static_cast<std::uint32_t>(length));
        append(out, data.subspan(position, length));
        position += length;
    }
    put8(out, 0);
}

void colorTable(std::vector<std::byte>& out, int bits, std::uint64_t seed) {
    append(out, makePattern(3 * (std::size_t{1} << bits), seed));
}

// ---------------------------------------------------------------------------
// BMP
// ---------------------------------------------------------------------------

// RLE8 or RLE4 data of rows of palette indices (bottom row first).
std::vector<std::byte> rle(const std::vector<std::vector<std::uint8_t>>& rows, bool rle4) {
    std::vector<std::byte> out;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const std::vector<std::uint8_t>& row = rows[r];
        std::size_t x = 0;
        while (x < row.size()) {
            std::size_t run = 1;
            while (x + run < row.size() && run < 255 && row[x + run] == row[x]) {
                ++run;
            }
            if (run >= 2) {
                put8(out, static_cast<std::uint32_t>(run));
                put8(out, rle4 ? static_cast<std::uint32_t>((row[x] << 4) | row[x]) : row[x]);
                x += run;
                continue;
            }
            // An absolute run of pixels that do not repeat (at least 3, as codes 0-2 are escapes).
            std::size_t count = 1;
            while (x + count < row.size() && count < 255 &&
                   (x + count + 1 >= row.size() || row[x + count] != row[x + count + 1])) {
                ++count;
            }
            if (count < 3) {
                put8(out, 1);
                put8(out, rle4 ? static_cast<std::uint32_t>(row[x] << 4) : row[x]);
                ++x;
                continue;
            }
            put8(out, 0);
            put8(out, static_cast<std::uint32_t>(count));
            std::size_t bytes = 0;
            if (rle4) {
                for (std::size_t i = 0; i < count; i += 2) {
                    const std::uint32_t high = row[x + i];
                    const std::uint32_t low = i + 1 < count ? row[x + i + 1] : 0;
                    put8(out, (high << 4) | low);
                    ++bytes;
                }
            } else {
                for (std::size_t i = 0; i < count; ++i) {
                    put8(out, row[x + i]);
                }
                bytes = count;
            }
            if (bytes % 2 != 0) {
                put8(out, 0);
            }
            x += count;
        }
        put8(out, 0);
        put8(out, r + 1 == rows.size() ? 1 : 0);  // end of line, or end of bitmap after the last row
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public builders
// ---------------------------------------------------------------------------

std::vector<std::byte> makeJpeg(const JpegOptions& options) {
    return JpegEncoder(options).encode();
}

std::vector<std::byte> jpegSegment(std::uint8_t marker, std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    put8(out, 0xFF);
    put8(out, marker);
    putBe16(out, static_cast<std::uint32_t>(payload.size() + 2));
    append(out, payload);
    return out;
}

std::vector<JpegMarker> jpegMarkers(std::span<const std::byte> file) {
    std::vector<JpegMarker> markers;
    std::size_t position = 0;
    bool inScan = false;
    while (position + 1 < file.size()) {
        if (u8(file, position) != 0xFF) {
            if (!inScan) {
                break;
            }
            ++position;
            continue;
        }
        std::size_t offset = position;
        while (position + 1 < file.size() && u8(file, position + 1) == 0xFF) {
            ++position;
        }
        const std::uint8_t code = u8(file, position + 1);
        if (inScan && code == 0x00) {
            position += 2;
            continue;
        }
        if (code == 0xD8 || code == 0xD9 || (code >= 0xD0 && code <= 0xD7) || code == 0x01) {
            markers.push_back(JpegMarker{code, offset, 0});
            position += 2;
            if (code == 0xD9) {
                break;
            }
            continue;
        }
        const std::size_t length = loadBe16(file, position + 2);
        markers.push_back(JpegMarker{code, offset, length});
        position += 2 + length;
        inScan = code == 0xDA;
    }
    return markers;
}

std::vector<std::byte> pngChunk(std::string_view type, std::span<const std::byte> data) {
    std::vector<std::byte> out;
    putBe32(out, static_cast<std::uint32_t>(data.size()));
    putText(out, type);
    append(out, data);
    putBe32(out, crc32(std::span<const std::byte>(out).subspan(4)));
    return out;
}

std::vector<PngChunkPosition> pngChunks(std::span<const std::byte> file) {
    std::vector<PngChunkPosition> chunks;
    std::size_t position = 8;
    while (position + 12 <= file.size()) {
        const std::size_t length = loadBe32(file, position);
        std::string type;
        for (std::size_t i = 0; i < 4; ++i) {
            type += static_cast<char>(file[position + 4 + i]);
        }
        chunks.push_back(PngChunkPosition{type, position, length});
        position += 12 + length;
    }
    return chunks;
}

std::vector<std::byte> makePng(const PngOptions& options) {
    const int bitsPerPixel = channels(options.color) * options.bitDepth;
    std::uint64_t state = options.seed * 7919;
    std::vector<std::byte> raw;
    if (options.interlaced) {
        constexpr std::array<std::array<std::uint32_t, 4>, 7> kPasses = {{{0, 0, 8, 8},
                                                                          {4, 0, 8, 8},
                                                                          {0, 4, 4, 8},
                                                                          {2, 0, 4, 4},
                                                                          {0, 2, 2, 4},
                                                                          {1, 0, 2, 2},
                                                                          {0, 1, 1, 2}}};
        for (const auto& pass : kPasses) {
            const std::uint32_t w = options.width > pass[0] ? (options.width - pass[0] + pass[2] - 1) / pass[2] : 0;
            const std::uint32_t h = options.height > pass[1] ? (options.height - pass[1] + pass[3] - 1) / pass[3] : 0;
            appendScanlines(raw, w, h, bitsPerPixel, state);
        }
    } else {
        appendScanlines(raw, options.width, options.height, bitsPerPixel, state);
    }
    const std::vector<std::byte> stream = zlibStored(raw);

    std::vector<std::byte> out;
    putText(out, "\x89PNG\r\n\x1A\n");
    std::vector<std::byte> ihdr;
    putBe32(ihdr, options.width);
    putBe32(ihdr, options.height);
    put8(ihdr, options.bitDepth);
    put8(ihdr, static_cast<std::uint32_t>(options.color));
    put8(ihdr, 0);
    put8(ihdr, 0);
    put8(ihdr, options.interlaced ? 1 : 0);
    append(out, pngChunk("IHDR", ihdr));
    if (options.color == PngColor::Palette) {
        append(out, pngChunk("PLTE", makePattern(3 * (std::size_t{1} << options.bitDepth), options.seed + 1)));
        append(out, pngChunk("tRNS", makePattern(std::size_t{1} << (options.bitDepth - 1), options.seed + 2)));
    }
    if (options.textChunks) {
        std::vector<std::byte> text;
        putText(text, std::string_view("Comment\0made by the RecoveryEngine tests", 40));
        append(out, pngChunk("tEXt", text));
    }
    const auto frameControl = [&](std::uint32_t sequence) {
        std::vector<std::byte> fctl;
        putBe32(fctl, sequence);
        putBe32(fctl, options.width);
        putBe32(fctl, options.height);
        putBe32(fctl, 0);
        putBe32(fctl, 0);
        putBe16(fctl, 1);
        putBe16(fctl, 10);
        put8(fctl, 0);
        put8(fctl, 0);
        return pngChunk("fcTL", fctl);
    };
    if (options.animated) {
        std::vector<std::byte> actl;
        putBe32(actl, 2);
        putBe32(actl, 0);
        append(out, pngChunk("acTL", actl));
        append(out, frameControl(0));
    }
    const std::size_t idatSize = options.idatSize == 0 ? stream.size() : options.idatSize;
    for (std::size_t position = 0; position < stream.size(); position += idatSize) {
        const std::size_t length = std::min(idatSize, stream.size() - position);
        append(out, pngChunk("IDAT", std::span<const std::byte>(stream).subspan(position, length)));
    }
    if (options.animated) {
        append(out, frameControl(1));
        std::vector<std::byte> fdat;
        putBe32(fdat, 2);
        append(fdat, stream);
        append(out, pngChunk("fdAT", fdat));
    }
    if (options.textChunks) {
        std::vector<std::byte> time;
        putBe16(time, 2026);
        put8(time, 9);
        put8(time, 22);
        put8(time, 12);
        put8(time, 30);
        put8(time, 0);
        append(out, pngChunk("tIME", time));
    }
    append(out, pngChunk("IEND", {}));
    return out;
}

std::vector<std::byte> makeGif(const GifOptions& options) {
    if (options.colorBits < 1 || options.colorBits > 8 || options.frames == 0) {
        throw std::invalid_argument("invalid GIF options");
    }
    const bool extensions = options.version89a;
    std::vector<std::byte> out;
    putText(out, options.version89a ? "GIF89a" : "GIF87a");
    putLe16(out, options.width);
    putLe16(out, options.height);
    const std::uint32_t bits = options.colorBits - 1U;
    put8(out, (options.globalColorTable ? 0x80U | bits : 0U) | (bits << 4));
    put8(out, 0);
    put8(out, 0);
    if (options.globalColorTable) {
        colorTable(out, options.colorBits, options.seed);
    }
    if (extensions && options.loop) {
        put8(out, 0x21);
        put8(out, 0xFF);
        put8(out, 11);
        putText(out, "NETSCAPE2.0");
        put8(out, 3);
        put8(out, 1);
        putLe16(out, 0);
        put8(out, 0);
    }
    if (extensions && !options.comment.empty()) {
        put8(out, 0x21);
        put8(out, 0xFE);
        std::vector<std::byte> text;
        putText(text, options.comment);
        subBlocks(out, text);
    }
    const int minCodeSize = std::max<int>(2, options.colorBits);
    for (std::size_t frame = 0; frame < options.frames; ++frame) {
        if (extensions && options.graphicControl) {
            put8(out, 0x21);
            put8(out, 0xF9);
            put8(out, 4);
            put8(out, 1 << 2);  // disposal: leave in place
            putLe16(out, 10);
            put8(out, 0);
            put8(out, 0);
        }
        // Later frames cover a smaller rectangle.
        const std::uint32_t left = frame == 0 ? 0 : static_cast<std::uint32_t>(frame % 4);
        const std::uint32_t top = frame == 0 ? 0 : static_cast<std::uint32_t>(frame % 3);
        const std::uint32_t width = options.width - std::min<std::uint32_t>(left, options.width - 1U);
        const std::uint32_t height = options.height - std::min<std::uint32_t>(top, options.height - 1U);
        const bool local = options.localColorTables || !options.globalColorTable;
        put8(out, 0x2C);
        putLe16(out, left);
        putLe16(out, top);
        putLe16(out, width);
        putLe16(out, height);
        put8(out, (local ? 0x80U | bits : 0U) | (options.interlaced ? 0x40U : 0U));
        if (local) {
            colorTable(out, options.colorBits, options.seed + frame + 1);
        }
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height);
        const std::vector<std::byte> noise = makePattern(pixels.size(), options.seed * 31 + frame);
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(noise[i]) & ((1U << options.colorBits) - 1));
        }
        put8(out, static_cast<std::uint32_t>(minCodeSize));
        subBlocks(out, lzw(pixels, minCodeSize));
    }
    put8(out, 0x3B);
    return out;
}

std::vector<std::byte> makeBmp(const BmpOptions& options) {
    if (options.width <= 0 || options.height <= 0) {
        throw std::invalid_argument("invalid BMP dimensions");
    }
    const auto width = static_cast<std::uint32_t>(options.width);
    const auto height = static_cast<std::uint32_t>(options.height);
    const std::uint16_t bits = options.bitsPerPixel;
    const bool core = options.header == BmpHeader::Core;
    const bool bitfields = options.compression == BmpCompression::Bitfields;
    const bool isRle = options.compression == BmpCompression::Rle8 || options.compression == BmpCompression::Rle4;
    std::uint32_t headerSize = 40;
    switch (options.header) {
    case BmpHeader::Core:
        headerSize = 12;
        break;
    case BmpHeader::Info:
        headerSize = 40;
        break;
    case BmpHeader::V4:
        headerSize = 108;
        break;
    case BmpHeader::V5:
        headerSize = 124;
        break;
    }
    const std::uint32_t colors = bits <= 8 ? 1U << bits : 0;
    const std::uint32_t masks = options.header == BmpHeader::Info && bitfields ? 12 : 0;
    const std::uint32_t pixelOffset = 14 + headerSize + masks + colors * (core ? 3U : 4U);

    // Pixel data, bottom row first unless top-down.
    std::vector<std::byte> pixels;
    std::uint64_t state = options.seed * 104729;
    if (isRle) {
        std::vector<std::vector<std::uint8_t>> rows(height, std::vector<std::uint8_t>(width));
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                // Runs of repeated pixels, and stretches of pixels that differ.
                const std::uint32_t value = x < width / 2 ? (x / 3 + y) : (x * 7 + y * 3 + static_cast<std::uint32_t>(options.seed));
                rows[y][x] = static_cast<std::uint8_t>(value % (bits == 4 ? 16U : 256U));
            }
        }
        pixels = rle(rows, options.compression == BmpCompression::Rle4);
    } else {
        const std::size_t stride = (static_cast<std::size_t>(width) * bits + 31) / 32 * 4;
        for (std::uint32_t y = 0; y < height; ++y) {
            std::vector<std::byte> row = makePattern(stride, state++);
            // Zero the padding at the end of the row, as encoders do.
            const std::size_t used = (static_cast<std::size_t>(width) * bits + 7) / 8;
            std::fill(row.begin() + static_cast<std::ptrdiff_t>(used), row.end(), std::byte{0});
            append(pixels, row);
        }
    }

    const std::uint32_t profileOffset = headerSize + masks + colors * 4 + static_cast<std::uint32_t>(pixels.size());
    const std::size_t fileSize = pixelOffset + pixels.size() + options.profileSize;
    std::vector<std::byte> out;
    putText(out, "BM");
    putLe32(out, options.zeroFileSize ? 0 : static_cast<std::uint32_t>(fileSize));
    putLe32(out, 0);
    putLe32(out, pixelOffset);
    putLe32(out, headerSize);
    if (core) {
        putLe16(out, width);
        putLe16(out, height);
        putLe16(out, 1);
        putLe16(out, bits);
    } else {
        putLe32(out, width);
        putLe32(out, options.topDown ? static_cast<std::uint32_t>(-options.height) : height);
        putLe16(out, 1);
        putLe16(out, bits);
        std::uint32_t compression = 0;
        switch (options.compression) {
        case BmpCompression::Rgb:
            compression = 0;
            break;
        case BmpCompression::Rle8:
            compression = 1;
            break;
        case BmpCompression::Rle4:
            compression = 2;
            break;
        case BmpCompression::Bitfields:
            compression = 3;
            break;
        }
        putLe32(out, compression);
        putLe32(out, static_cast<std::uint32_t>(pixels.size()));
        putLe32(out, 2835);
        putLe32(out, 2835);
        putLe32(out, 0);
        putLe32(out, 0);
        if (headerSize >= 108) {
            // Masks, color space, endpoints and gamma.
            const bool rgb565 = bits == 16;
            putLe32(out, bitfields ? (rgb565 ? 0xF800U : 0x00FF0000U) : 0);
            putLe32(out, bitfields ? (rgb565 ? 0x07E0U : 0x0000FF00U) : 0);
            putLe32(out, bitfields ? (rgb565 ? 0x001FU : 0x000000FFU) : 0);
            putLe32(out, 0);
            putLe32(out, options.profileSize > 0 ? 0x4D424544U : 0x73524742U);  // 'MBED' or 'sRGB'
            for (int i = 0; i < 12; ++i) {
                putLe32(out, 0);
            }
        }
        if (headerSize >= 124) {
            putLe32(out, 4);  // LCS_GM_IMAGES
            putLe32(out, options.profileSize > 0 ? profileOffset : 0);
            putLe32(out, static_cast<std::uint32_t>(options.profileSize));
            putLe32(out, 0);
        }
        if (masks != 0) {
            const bool rgb565 = bits == 16;
            putLe32(out, rgb565 ? 0xF800U : 0x00FF0000U);
            putLe32(out, rgb565 ? 0x07E0U : 0x0000FF00U);
            putLe32(out, rgb565 ? 0x001FU : 0x000000FFU);
        }
    }
    if (colors != 0) {
        const std::vector<std::byte> palette = makePattern(colors * 4, options.seed + 9);
        for (std::uint32_t i = 0; i < colors; ++i) {
            put8(out, static_cast<std::uint8_t>(palette[i * 4]));
            put8(out, static_cast<std::uint8_t>(palette[i * 4 + 1]));
            put8(out, static_cast<std::uint8_t>(palette[i * 4 + 2]));
            if (!core) {
                put8(out, 0);
            }
        }
    }
    append(out, pixels);
    append(out, makePattern(options.profileSize, options.seed + 11));
    if (out.size() != fileSize) {
        throw std::logic_error("BMP builder size mismatch");
    }
    return out;
}

std::vector<std::byte> riffChunk(std::string_view fourCc, std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    putText(out, fourCc);
    putLe32(out, static_cast<std::uint32_t>(payload.size()));
    append(out, payload);
    if (payload.size() % 2 != 0) {
        put8(out, 0);
    }
    return out;
}

std::vector<std::byte> webpBitstream(std::string_view sampleName) {
    const std::vector<std::byte> file = samples::named(sampleName).data();
    const std::uint32_t size = loadLe32(file, 16);
    return std::vector<std::byte>(file.begin() + 20, file.begin() + 20 + static_cast<std::ptrdiff_t>(size));
}

std::vector<std::byte> makeWebp(const WebpOptions& options) {
    // The libwebp bitstreams are 16x16.
    constexpr std::uint32_t kSide = 16;
    const std::vector<std::byte> lossy = webpBitstream("cwebp_lossy.webp");
    const std::vector<std::byte> lossless = webpBitstream("cwebp_lossless.webp");
    const bool animated = options.kind == WebpKind::Animated;
    const bool alpha = options.kind == WebpKind::LossyWithAlpha;
    const bool extended = options.extended || alpha || animated || options.iccSize != 0 || options.exifSize != 0 ||
                          options.xmpSize != 0;

    std::vector<std::byte> body;
    putText(body, "WEBP");
    if (extended) {
        std::uint32_t flags = 0;
        flags |= options.iccSize != 0 ? 0x20U : 0U;
        flags |= alpha ? 0x10U : 0U;
        flags |= options.exifSize != 0 ? 0x08U : 0U;
        flags |= options.xmpSize != 0 ? 0x04U : 0U;
        flags |= animated ? 0x02U : 0U;
        std::vector<std::byte> vp8x;
        put8(vp8x, flags);
        putLe24(vp8x, 0);
        putLe24(vp8x, (animated ? 2 * kSide : kSide) - 1);
        putLe24(vp8x, kSide - 1);
        append(body, riffChunk("VP8X", vp8x));
    }
    if (options.iccSize != 0) {
        append(body, riffChunk("ICCP", makePattern(options.iccSize, 21)));
    }
    if (animated) {
        std::vector<std::byte> anim;
        putLe32(anim, 0xFFFFFFFF);  // background color
        putLe16(anim, 0);           // loop forever
        append(body, riffChunk("ANIM", anim));
        for (std::size_t frame = 0; frame < options.frames; ++frame) {
            std::vector<std::byte> anmf;
            putLe24(anmf, static_cast<std::uint32_t>(frame % 2) * kSide / 2);
            putLe24(anmf, 0);
            putLe24(anmf, kSide - 1);
            putLe24(anmf, kSide - 1);
            putLe24(anmf, 100);
            put8(anmf, 0);
            append(anmf, frame % 2 == 0 ? riffChunk("VP8 ", lossy) : riffChunk("VP8L", lossless));
            append(body, riffChunk("ANMF", anmf));
        }
    } else {
        if (alpha) {
            std::vector<std::byte> alph;
            put8(alph, 0);  // uncompressed, no filtering, no preprocessing
            append(alph, makePattern(kSide * kSide, 22));
            append(body, riffChunk("ALPH", alph));
        }
        if (options.kind == WebpKind::Lossless) {
            append(body, riffChunk("VP8L", lossless));
        } else {
            append(body, riffChunk("VP8 ", lossy));
        }
    }
    if (options.exifSize != 0) {
        append(body, riffChunk("EXIF", makePattern(options.exifSize, 23)));
    }
    if (options.xmpSize != 0) {
        append(body, riffChunk("XMP ", makePattern(options.xmpSize, 24)));
    }
    if (options.unknownChunk) {
        append(body, riffChunk("TEST", makePattern(9, 25)));
    }
    std::vector<std::byte> out;
    putText(out, "RIFF");
    putLe32(out, static_cast<std::uint32_t>(body.size()));
    append(out, body);
    return out;
}

}  // namespace recovery::test
