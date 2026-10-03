#pragma once

// MP4 files of AVC and HEVC video made by FFmpeg with libx264 and libx265 for
// the MP4 media decoder tests (tests/reference/make_video_media_vectors.sh,
// embedded by embed_media_vectors.py into video_media_vectors.cpp): CAVLC and
// CABAC, B pictures, weighted prediction, interlaced coding, slices, 4:2:2,
// 4:4:4 and 10 bits, lossless coding, scaling matrices, HRD parameters,
// wavefront entry points, open GOPs, temporal sub-layers, and parameter sets
// inside the samples.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::test::video_vectors {

struct Vector {
    std::string_view name;
    std::span<const std::uint8_t> bytes;

    [[nodiscard]] std::vector<std::byte> data() const {
        std::vector<std::byte> out(bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            out[i] = static_cast<std::byte>(bytes[i]);
        }
        return out;
    }
};

[[nodiscard]] const std::vector<Vector>& all();

// The vector with this file name; aborts the test program when there is none.
[[nodiscard]] const Vector& named(std::string_view name);

}  // namespace recovery::test::video_vectors
