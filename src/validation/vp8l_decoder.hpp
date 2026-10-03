#pragma once

// VP8L (WebP lossless, RFC 9649 section 3) decoding for media validation:
// every transform, prefix code, color cache index and backward reference of
// an image stream, checked as libwebp checks them (each transform at most
// once, prefix codes complete or of one symbol, no reference before the
// first pixel or beyond the last, cache indexes inside the cache, the data
// not running out). Pixel values are kept only for the entropy image, whose
// values choose the prefix codes; the image itself is counted, not stored.

#include "content_bytes.hpp"
#include "validation/validation.hpp"

#include <cstdint>
#include <string>

namespace recovery::validation::detail {

struct Vp8lOutcome {
    enum class Kind : std::uint8_t {
        Done,
        // The data ends inside the image stream.
        Ended,
        Invalid,
        // A limit (memory, work) stops it.
        Unsupported,
        // The reader failed (see ContentBytes::error()).
        ReadError,
    };

    Kind kind = Kind::Invalid;
    std::string detail;
    // Content offset of the problem.
    std::uint64_t offset = 0;
    // What was decoded.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    unsigned transforms = 0;
    unsigned cacheBits = 0;
    std::uint64_t groups = 0;
    std::uint64_t pixels = 0;
    // Pixels decoded and bytes held, toward the limits.
    std::uint64_t work = 0;
};

// A VP8L chunk's payload: the 5-byte header (signature, size, alpha hint,
// version), then the image stream. `work` and `memory` already spent count
// toward the limits.
[[nodiscard]] Vp8lOutcome decodeVp8l(ContentBytes& bytes, const MediaLimits& limits, std::uint64_t work);

// An image stream without a header, of width x height (lossless alpha in an
// ALPH chunk).
[[nodiscard]] Vp8lOutcome decodeVp8lStream(ContentBytes& bytes, std::uint32_t width, std::uint32_t height,
                                           const MediaLimits& limits, std::uint64_t work);

}  // namespace recovery::validation::detail
