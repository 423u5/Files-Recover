#pragma once

// PNG files around zlib streams of every kind and DEFLATE bit streams that
// each break one rule, made by tests/reference/make_png_media_vectors.py
// (png_media_vectors.cpp). Every chunk CRC is correct.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::test::png_vectors {

struct Vector {
    std::string_view name;
    // The media level's status: "passed", "failed" or "truncated".
    std::string_view expect;
    // A part of the media level's detail ("" for files that pass).
    std::string_view detail;
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

}  // namespace recovery::test::png_vectors
