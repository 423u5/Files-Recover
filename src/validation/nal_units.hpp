#pragma once

// NAL units (ITU-T H.264 7.3.1 and 7.4.1, ITU-T H.265 7.3.1 and 7.4.2) for
// the AVC and HEVC media checks (avc_syntax.hpp, hevc_syntax.hpp):
//
//  * the byte sequences a NAL unit may not hold at any byte-aligned
//    position: 00 00 00, 00 00 01 and 00 00 02 (encoders insert an
//    emulation_prevention_three_byte to avoid them), and 00 00 03 followed
//    by a byte above 03. Zero bytes at the very end of a unit are taken as
//    padding: FFmpeg drops them, and a muxer that copied a byte stream may
//    have kept its trailing_zero_8bits. A run of zeros with data after it
//    is never padding: it is how a zero-filled cluster in the middle of a
//    picture shows;
//  * the RBSP: the unit's bytes after its header without the emulation
//    prevention bytes, read MSB first, with the position of its
//    rbsp_stop_one_bit (the last set bit) for more_rbsp_data() and for
//    telling fields that run past the end of the syntax.

#include "bit_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace recovery::validation::detail::nal {

struct Violation {
    // Content offset of the sequence's first byte.
    std::uint64_t offset = 0;
    std::string detail;
};

// Scans the bytes of one NAL unit, fed in pieces in order.
class SequenceScan {
public:
    // The next bytes of the unit; `offset` is the content offset of the first.
    void feed(std::span<const std::uint8_t> bytes, std::uint64_t offset);
    void feed(std::span<const std::byte> bytes, std::uint64_t offset);
    // After the last byte: the first violation, if any.
    [[nodiscard]] const std::optional<Violation>& violation() const noexcept { return violation_; }

private:
    void next(std::uint8_t value, std::uint64_t at);

    std::optional<Violation> violation_;
    // Zero bytes just before the next one, and where a run of three began.
    std::uint64_t zeros_ = 0;
    std::uint64_t runStart_ = 0;
    // The previous byte was an emulation prevention byte (03 after 00 00).
    bool afterEscape_ = false;
    std::uint64_t escapeOffset_ = 0;
};

// The RBSP of a NAL unit whose first bytes are `unit` (`headerBytes` of NAL
// unit header, then the payload): the payload without its emulation
// prevention bytes. `complete`: `unit` is the whole NAL unit, so the RBSP
// ends with its stop bit; otherwise only its first part is there.
class Rbsp {
public:
    Rbsp(std::span<const std::uint8_t> unit, std::size_t headerBytes, bool complete);
    // The reader points into the bytes held here.
    Rbsp(const Rbsp&) = delete;
    Rbsp& operator=(const Rbsp&) = delete;
    Rbsp(Rbsp&&) = delete;
    Rbsp& operator=(Rbsp&&) = delete;
    ~Rbsp() = default;

    [[nodiscard]] MsbBits& bits() noexcept { return bits_; }
    // The rbsp_stop_one_bit's position, when the whole RBSP is there and has one.
    [[nodiscard]] const std::optional<std::uint64_t>& stopBit() const noexcept { return stopBit_; }
    [[nodiscard]] bool complete() const noexcept { return complete_; }
    // more_rbsp_data(): fields before the stop bit (needs a complete RBSP).
    [[nodiscard]] bool moreData() const noexcept {
        return stopBit_.has_value() && bits_.position() < *stopBit_;
    }
    // The offset in the NAL unit of RBSP byte `index` (header and emulation
    // prevention bytes counted).
    [[nodiscard]] std::uint64_t unitOffset(std::uint64_t index) const noexcept;
    // The fields read so far run past the syntax: beyond the stop bit of a
    // complete RBSP, or beyond the bytes there are.
    [[nodiscard]] bool overrun() const noexcept {
        return bits_.overrun() || (stopBit_.has_value() && bits_.position() > *stopBit_) ||
               (complete_ && !stopBit_.has_value());
    }

private:
    std::size_t headerBytes_;
    // For each emulation prevention byte, the index of the RBSP byte after it.
    std::vector<std::uint64_t> escapes_;
    std::vector<std::uint8_t> bytes_;
    MsbBits bits_;
    std::optional<std::uint64_t> stopBit_;
    bool complete_;
};

// Ceil(Log2(n)) for n >= 1: the bits of a field that holds 0 to n - 1.
[[nodiscard]] unsigned ceilLog2(std::uint64_t n) noexcept;

}  // namespace recovery::validation::detail::nal
