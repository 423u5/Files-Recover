#pragma once

// Small image files made by independent encoders: libjpeg-turbo, libwebp and
// giflib (tests/reference/make_image_samples.sh) and Windows GDI+
// (make_image_samples.ps1), embedded by tests/reference/embed_image_samples.py.
// They guard against the image builders and the format modules sharing a
// misreading of a specification, and they supply the VP8 and VP8L bitstreams
// that the WebP builder wraps.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::test::samples {

struct Sample {
    std::string_view name;
    // The carving format id: "jpeg", "png", "webp", "gif" or "bmp".
    std::string_view format;
    std::string_view producer;
    std::span<const std::uint8_t> bytes;

    [[nodiscard]] std::vector<std::byte> data() const {
        std::vector<std::byte> out(bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            out[i] = static_cast<std::byte>(bytes[i]);
        }
        return out;
    }
};

[[nodiscard]] const std::vector<Sample>& all();

// The sample with this file name; aborts the test program when there is none.
[[nodiscard]] const Sample& named(std::string_view name);

}  // namespace recovery::test::samples
