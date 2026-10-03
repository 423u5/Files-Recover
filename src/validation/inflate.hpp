#pragma once

// zlib (RFC 1950) and DEFLATE (RFC 1951) decoding for media validation: the
// image data of PNG files. The data is inflated through a 32 KiB window and
// handed on in pieces, never held whole. What is accepted follows zlib, the
// library nearly every PNG is written and read with: an incomplete literal or
// distance code only when it is a single one-bit code, no distance beyond
// the data or the window, an end-of-block code in every block, and the
// Adler-32 of the output.

#include "content_bytes.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace recovery::validation::detail {

// Adler-32 (RFC 1950 8.2).
class Adler32 {
public:
    void update(std::span<const std::uint8_t> data) noexcept;
    [[nodiscard]] std::uint32_t value() const noexcept { return (b_ << 16) | a_; }

private:
    std::uint32_t a_ = 1;
    std::uint32_t b_ = 0;
};

class InflateSink {
public:
    virtual ~InflateSink() = default;
    // The next piece of output, in order. false stops inflating: the sink
    // found a problem of its own (too much data, a wrong value).
    virtual bool write(std::span<const std::uint8_t> data) = 0;

protected:
    InflateSink() = default;
    InflateSink(const InflateSink&) = default;
    InflateSink& operator=(const InflateSink&) = default;
    InflateSink(InflateSink&&) = default;
    InflateSink& operator=(InflateSink&&) = default;
};

struct InflateOutcome {
    enum class Kind : std::uint8_t {
        // The stream ended with a valid Adler-32.
        Done,
        // The data ended inside the stream.
        Truncated,
        // The stream is malformed (see detail).
        Invalid,
        // The sink stopped it.
        Stopped,
        // The reader failed (see ContentBytes::error()).
        ReadError,
    };

    Kind kind = Kind::Invalid;
    std::string detail;
    // Content offset of the problem (Truncated: the end of the data).
    std::uint64_t offset = 0;
    // Bytes of output.
    std::uint64_t output = 0;
    // Done: bytes of the ranges left after the Adler-32.
    std::uint64_t trailing = 0;
    // Deflate blocks read.
    std::uint64_t blocks = 0;
};

// Inflates the zlib stream at the start of `bytes`, handing its output to
// `sink`.
[[nodiscard]] InflateOutcome inflateZlib(ContentBytes& bytes, InflateSink& sink);

}  // namespace recovery::validation::detail
