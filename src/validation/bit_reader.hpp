#pragma once

// Bit readers for the media decoders.
//
//  * LsbBits: bits from the least significant end of each byte, over
//    ContentBytes (DEFLATE, GIF's LZW, VP8L).
//  * MsbBits: bits from the most significant end of each byte, over bytes in
//    memory (AVC and HEVC parameter sets and slice headers, AAC syntax). It
//    also reads the Exp-Golomb codes of ITU-T H.264 9.1.

#include "content_bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace recovery::validation::detail {

class LsbBits {
public:
    explicit LsbBits(ContentBytes& bytes) noexcept : bytes_(&bytes) {}

    // Buffers at least n (<= 32) bits; false when the data ends first.
    bool ensure(unsigned n) {
        while (count_ < n) {
            std::uint8_t byte = 0;
            if (!bytes_->next(byte)) {
                return false;
            }
            buffer_ |= std::uint64_t{byte} << count_;
            count_ += 8;
        }
        return true;
    }
    // Reads n (<= 32) bits, the first in the least significant position;
    // false when the data ends first.
    bool read(unsigned n, std::uint32_t& value) {
        if (!ensure(n)) {
            return false;
        }
        value = static_cast<std::uint32_t>(buffer_ & mask(n));
        buffer_ >>= n;
        count_ -= n;
        return true;
    }
    // The next n (<= 32) bits without taking them; zeros beyond the end of the data.
    [[nodiscard]] std::uint32_t peek(unsigned n) {
        static_cast<void>(ensure(n));
        return static_cast<std::uint32_t>(buffer_ & mask(n));
    }
    // Takes n bits that peek() showed; false when fewer were left (the data ended).
    bool drop(unsigned n) {
        if (n > count_) {
            buffer_ = 0;
            count_ = 0;
            return false;
        }
        buffer_ >>= n;
        count_ -= n;
        return true;
    }
    // Drops the bits up to the next byte boundary.
    void alignToByte() {
        const unsigned extra = count_ % 8;
        buffer_ >>= extra;
        count_ -= extra;
    }
    // Whole bytes buffered after alignToByte(): taken before the stream's next ones.
    [[nodiscard]] unsigned bufferedBits() const noexcept { return count_; }

    // Content offset of the byte holding the next bit.
    [[nodiscard]] std::uint64_t position() const noexcept { return bytes_->position() - (count_ + 7) / 8; }
    [[nodiscard]] ContentBytes& bytes() noexcept { return *bytes_; }

private:
    static constexpr std::uint64_t mask(unsigned n) noexcept { return n >= 64 ? ~0ULL : (1ULL << n) - 1; }

    ContentBytes* bytes_;
    std::uint64_t buffer_ = 0;
    unsigned count_ = 0;
};

class MsbBits {
public:
    explicit MsbBits(std::span<const std::uint8_t> data) noexcept : data_(data) {}

    // Reads n (<= 32) bits, the first in the most significant position.
    // Beyond the end: zeros, and overrun() becomes true.
    [[nodiscard]] std::uint32_t read(unsigned n) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < n; ++i) {
            value = (value << 1) | bit();
        }
        return value;
    }
    [[nodiscard]] bool flag() { return bit() != 0; }
    void skip(std::uint64_t n) {
        if (n > left()) {
            overrun_ = true;
            position_ = size();
            return;
        }
        position_ += n;
    }
    // Moves to bit `position` (at most size()) and clears overrun() and malformed().
    void seek(std::uint64_t position) noexcept {
        position_ = position < size() ? position : size();
        overrun_ = false;
        malformed_ = false;
    }
    // ue(v): an unsigned Exp-Golomb code of at most 32 bits of value.
    // Longer codes (more than 31 leading zeros) set malformed().
    [[nodiscard]] std::uint32_t ue() {
        unsigned zeros = 0;
        while (bit() == 0) {
            if (overrun_ || ++zeros > 31) {
                malformed_ = true;
                return 0;
            }
        }
        if (zeros == 0) {
            return 0;
        }
        return static_cast<std::uint32_t>((std::uint64_t{1} << zeros) - 1 + read(zeros));
    }
    // se(v): a signed Exp-Golomb code.
    [[nodiscard]] std::int32_t se() {
        const std::uint32_t code = ue();
        const auto magnitude = static_cast<std::int64_t>((std::uint64_t{code} + 1) / 2);
        return static_cast<std::int32_t>((code & 1U) != 0 ? magnitude : -magnitude);
    }

    [[nodiscard]] std::uint64_t position() const noexcept { return position_; }
    [[nodiscard]] std::uint64_t size() const noexcept { return std::uint64_t{data_.size()} * 8; }
    [[nodiscard]] std::uint64_t left() const noexcept { return size() - position_; }
    [[nodiscard]] bool byteAligned() const noexcept { return position_ % 8 == 0; }
    // A read went beyond the data.
    [[nodiscard]] bool overrun() const noexcept { return overrun_; }
    // An Exp-Golomb code was too long, or a read went beyond the data.
    [[nodiscard]] bool malformed() const noexcept { return malformed_ || overrun_; }

private:
    [[nodiscard]] std::uint32_t bit() {
        if (position_ >= size()) {
            overrun_ = true;
            return 0;
        }
        const std::uint8_t byte = data_[static_cast<std::size_t>(position_ / 8)];
        const unsigned shift = 7 - static_cast<unsigned>(position_ % 8);
        ++position_;
        return (byte >> shift) & 1U;
    }

    std::span<const std::uint8_t> data_;
    std::uint64_t position_ = 0;
    bool overrun_ = false;
    bool malformed_ = false;
};

}  // namespace recovery::validation::detail
