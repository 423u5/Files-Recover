#pragma once

// Canonical prefix (Huffman) codes as DEFLATE (RFC 1951 3.2.2) and VP8L
// (RFC 9649 3.7) use them: given each symbol's code length, codes are
// assigned in order of length, then of symbol, and a code's bits are read
// first bit first from the least significant end of each byte (LsbBits).
//
// Decoding goes through a table for codes of up to kFastBits bits and bit by
// bit (RFC 1951's canonical construction, as zlib's puff does) beyond.

#include "bit_reader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace recovery::validation::detail {

class PrefixCode {
public:
    static constexpr unsigned kMaxLength = 15;
    static constexpr unsigned kFastBits = 9;
    // Largest alphabet (VP8L's green and length codes with the largest color cache).
    static constexpr std::size_t kMaxSymbols = 256 + 24 + 2048;

    enum class Shape : std::uint8_t {
        // No symbol has a code.
        Empty,
        // Exactly one symbol has a code.
        Single,
        // The codes fill the code space (Kraft's sum is 1).
        Complete,
        // Codes are left over: some bit sequences decode to nothing.
        Incomplete,
        // More codes than the lengths allow: not a prefix code.
        OverSubscribed,
    };

    // `lengths` holds each symbol's code length (0: no code), at most
    // kMaxLength each, and at most kMaxSymbols symbols. Returns what the
    // lengths make; only Single, Complete and Incomplete codes can decode.
    Shape build(std::span<const std::uint8_t> lengths);

    // The next symbol: -1 when the bits form no code of an incomplete code,
    // -2 when the data ends inside a code.
    [[nodiscard]] int decode(LsbBits& bits) const;

    [[nodiscard]] Shape shape() const noexcept { return shape_; }
    // Single: the symbol (its code is one bit long in DEFLATE, zero in VP8L).
    [[nodiscard]] int single() const noexcept { return single_; }
    // Bytes the code holds (for memory accounting).
    [[nodiscard]] std::size_t memory() const noexcept;

private:
    Shape shape_ = Shape::Empty;
    int single_ = -1;
    std::array<std::uint16_t, kMaxLength + 1> counts_{};
    // Symbols ordered by code (length, then symbol).
    std::vector<std::uint16_t> symbols_;
    // Entry for the next kFastBits bits: (symbol << 4) | length, or 0 when
    // the code is longer (or there is none).
    std::vector<std::uint16_t> fast_;
};

}  // namespace recovery::validation::detail
