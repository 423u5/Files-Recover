#pragma once

// ZIP archives that Python's zipfile wrote, and archives reworked from them
// into what other writers make (ZIP64 end records, an APK signing block),
// made by tests/reference/make_zip_samples.py (zip_samples.cpp). Every one
// is valid: the tests break them byte by byte.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::test::zip_samples {

struct Sample {
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

[[nodiscard]] const std::vector<Sample>& all();
// The sample named `name` (the first one when none is).
[[nodiscard]] const Sample& named(std::string_view name);

}  // namespace recovery::test::zip_samples
