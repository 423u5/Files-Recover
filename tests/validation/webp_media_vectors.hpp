#pragma once

// WebP files made by libwebp's cwebp for the media decoder tests
// (tests/reference/make_webp_media_vectors.sh, embedded by
// embed_webp_media_vectors.py into webp_media_vectors.cpp): VP8L images with
// a color cache and meta prefix codes, color-indexed images of every
// packing, lossless alpha with filtering, lossy frames with segmentation.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::test::webp_vectors {

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

}  // namespace recovery::test::webp_vectors
