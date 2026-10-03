#pragma once

// VP8 key frames (RFC 6386) for media validation, as far as they can be
// checked without the codec's tables: the frame tag, the start code and the
// size, the frame header in the first partition (decoded with the boolean
// decoder: color space, segmentation, loop filter, the number of DCT
// partitions, the quantizer indexes, up to the token probability updates,
// whose decoding needs the coefficient update probabilities), and the sizes
// of the DCT partitions against the data.

#include "carving/content_reader.hpp"
#include "recovery/result.hpp"

#include <cstdint>
#include <string>

namespace recovery::validation::detail {

struct Vp8Check {
    enum class Kind : std::uint8_t { Done, Ended, Invalid };

    Kind kind = Kind::Invalid;
    std::string detail;
    // Content offset of the problem.
    std::uint64_t offset = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t partitions = 0;
};

// The VP8 bitstream at content [begin, end) (a VP8 chunk's payload). `end`
// beyond the content: the content ends inside it. Fails only with the
// reader's errors.
[[nodiscard]] Result<Vp8Check> checkVp8(carving::IContentReader& content, std::uint64_t begin, std::uint64_t end);

}  // namespace recovery::validation::detail
