#pragma once

// ADTS files with CRC protection made by fdkaac for the AAC media decoder
// tests (tests/reference/make_aac_media_vectors.sh, embedded by
// embed_media_vectors.py into aac_media_vectors.cpp): mono and stereo at
// constant and variable bitrates, 5.1, HE-AAC, silence, clicks (short
// windows) and 8 kHz.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::test::aac_vectors {

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

}  // namespace recovery::test::aac_vectors
