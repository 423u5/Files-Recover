#include "vp8_header.hpp"

#include "recovery/byte_order.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace recovery::validation::detail {

namespace {

// The boolean entropy decoder of RFC 6386 section 7. Bytes beyond the data
// read as zeros and are counted.
class BoolDecoder {
public:
    explicit BoolDecoder(std::span<const std::uint8_t> data) noexcept : data_(data) {
        value_ = (std::uint32_t{nextByte()} << 8) | nextByte();
    }

    [[nodiscard]] std::uint32_t bit(std::uint32_t probability) {
        const std::uint32_t split = 1 + (((range_ - 1) * probability) >> 8);
        const std::uint32_t bigSplit = split << 8;
        std::uint32_t result = 0;
        if (value_ >= bigSplit) {
            result = 1;
            range_ -= split;
            value_ -= bigSplit;
        } else {
            range_ = split;
        }
        while (range_ < 128) {
            value_ <<= 1;
            range_ <<= 1;
            if (++bitCount_ == 8) {
                bitCount_ = 0;
                value_ |= nextByte();
            }
        }
        return result;
    }
    [[nodiscard]] std::uint32_t literal(unsigned n) {
        std::uint32_t value = 0;
        while (n-- > 0) {
            value = (value << 1) | bit(128);
        }
        return value;
    }
    [[nodiscard]] bool flag() { return literal(1) != 0; }
    // A flagged signed value of n bits: read and discarded.
    void optionalSigned(unsigned n) {
        if (flag()) {
            static_cast<void>(literal(n + 1));
        }
    }
    [[nodiscard]] bool overrun() const noexcept { return overrun_; }

private:
    std::uint8_t nextByte() {
        if (position_ < data_.size()) {
            return data_[position_++];
        }
        overrun_ = true;
        return 0;
    }

    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
    std::uint32_t value_ = 0;
    std::uint32_t range_ = 255;
    unsigned bitCount_ = 0;
    bool overrun_ = false;
};

Vp8Check outcome(Vp8Check::Kind kind, std::uint64_t offset, std::string detail) {
    Vp8Check check;
    check.kind = kind;
    check.offset = offset;
    check.detail = std::move(detail);
    return check;
}

}  // namespace

Result<Vp8Check> checkVp8(carving::IContentReader& content, std::uint64_t begin, std::uint64_t end) {
    const std::uint64_t size = content.size();
    const std::uint64_t available = std::min(end, size);
    const auto short_ = [&](std::uint64_t offset, std::string inside) {
        return end > size ? outcome(Vp8Check::Kind::Ended, size, "the data ends inside " + inside)
                          : outcome(Vp8Check::Kind::Invalid, offset, "the VP8 data ends inside " + inside);
    };
    if (available < begin || available - begin < 10) {
        return short_(begin, "the frame tag");
    }
    Result<std::span<const std::byte>> head = content.read(begin, 10);
    if (!head.ok()) {
        return head.error();
    }
    const std::uint32_t tag = loadU8(*head, 0) | (std::uint32_t{loadU8(*head, 1)} << 8) |
                              (std::uint32_t{loadU8(*head, 2)} << 16);
    const bool keyFrame = (tag & 1U) == 0;
    const std::uint32_t version = (tag >> 1) & 7U;
    const bool shown = ((tag >> 4) & 1U) != 0;
    const std::uint32_t firstPartition = tag >> 5;
    if (!keyFrame || version > 3 || !shown) {
        return outcome(Vp8Check::Kind::Invalid, begin,
                       "the VP8 frame is not a shown key frame of version 0 to 3 (version " + std::to_string(version) +
                           ")");
    }
    if (loadU8(*head, 3) != 0x9D || loadU8(*head, 4) != 0x01 || loadU8(*head, 5) != 0x2A) {
        return outcome(Vp8Check::Kind::Invalid, begin + 3, "no VP8 start code");
    }
    Vp8Check check;
    check.width = loadLe16(*head, 6) & 0x3FFFU;
    check.height = loadLe16(*head, 8) & 0x3FFFU;
    if (check.width == 0 || check.height == 0) {
        return outcome(Vp8Check::Kind::Invalid, begin + 6, "a VP8 frame of width or height 0");
    }

    // The first partition: the frame header.
    const std::uint64_t first = begin + 10;
    if (firstPartition > end - first) {
        return outcome(Vp8Check::Kind::Invalid, begin,
                       "the first partition's " + std::to_string(firstPartition) + " bytes do not fit the VP8 data");
    }
    if (first + firstPartition > size) {
        return short_(first, "the first partition");
    }
    Result<std::span<const std::byte>> partition = content.read(first, firstPartition);
    if (!partition.ok()) {
        return partition.error();
    }
    std::vector<std::uint8_t> header(partition->size());
    std::transform(partition->begin(), partition->end(), header.begin(),
                   [](std::byte b) { return static_cast<std::uint8_t>(b); });
    BoolDecoder bits(header);
    static_cast<void>(bits.literal(2));  // color space, clamping type
    if (bits.flag()) {                   // segmentation
        const bool updateMap = bits.flag();
        if (bits.flag()) {  // segment feature data
            static_cast<void>(bits.flag());
            for (int i = 0; i < 4; ++i) {
                bits.optionalSigned(7);  // quantizer
            }
            for (int i = 0; i < 4; ++i) {
                bits.optionalSigned(6);  // loop filter level
            }
        }
        if (updateMap) {
            for (int i = 0; i < 3; ++i) {
                if (bits.flag()) {
                    static_cast<void>(bits.literal(8));
                }
            }
        }
    }
    static_cast<void>(bits.literal(1 + 6 + 3));  // filter type, level, sharpness
    if (bits.flag() && bits.flag()) {             // loop filter deltas, updated
        for (int i = 0; i < 8; ++i) {
            bits.optionalSigned(6);
        }
    }
    check.partitions = 1U << bits.literal(2);
    static_cast<void>(bits.literal(7));  // y_ac_qi
    for (int i = 0; i < 5; ++i) {
        bits.optionalSigned(4);  // the quantizer deltas
    }
    static_cast<void>(bits.flag());  // refresh_entropy_probs
    if (bits.overrun()) {
        return outcome(Vp8Check::Kind::Invalid, first, "the frame header runs past the first partition");
    }

    // The DCT partitions: their sizes, then their data, to the end.
    const std::uint64_t table = first + firstPartition;
    const std::uint64_t tableSize = 3ULL * (check.partitions - 1);
    if (tableSize > end - table) {
        return outcome(Vp8Check::Kind::Invalid, table, "the partition size table does not fit the VP8 data");
    }
    if (table + tableSize > size) {
        return short_(table, "the partition size table");
    }
    std::uint64_t position = table + tableSize;
    if (tableSize > 0) {
        Result<std::span<const std::byte>> sizes = content.read(table, static_cast<std::size_t>(tableSize));
        if (!sizes.ok()) {
            return sizes.error();
        }
        for (std::uint32_t p = 0; p + 1 < check.partitions; ++p) {
            const std::uint64_t length = loadU8(*sizes, 3 * p) | (std::uint32_t{loadU8(*sizes, 3 * p + 1)} << 8) |
                                         (std::uint32_t{loadU8(*sizes, 3 * p + 2)} << 16);
            if (length > end - position) {
                return outcome(Vp8Check::Kind::Invalid, table + 3 * p,
                               "DCT partition " + std::to_string(p + 1) + "'s " + std::to_string(length) +
                                   " bytes do not fit the VP8 data");
            }
            position += length;
        }
    }
    if (position >= end) {
        return outcome(Vp8Check::Kind::Invalid, position, "the last DCT partition is empty");
    }
    if (end > size) {
        return short_(position, "the DCT partitions");
    }
    check.kind = Vp8Check::Kind::Done;
    check.offset = begin;
    check.detail = "VP8 key frame " + std::to_string(check.width) + "x" + std::to_string(check.height) + ", " +
                   std::to_string(check.partitions) + (check.partitions == 1 ? " DCT partition" : " DCT partitions") +
                   ", frame header decoded up to the token probabilities";
    return check;
}

}  // namespace recovery::validation::detail
