#pragma once

// Small audio files made by independent encoders: LAME, FFmpeg, faac,
// fdkaac, SoX and Python's wave module (tests/reference/
// make_audio_samples.sh), and Windows' speech synthesizer and Media
// Foundation (make_audio_samples.ps1), embedded by tests/reference/
// embed_audio_samples.py. They guard against the audio builders and the
// format modules sharing a misreading of a specification.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::test::audio_samples {

struct Sample {
    std::string_view name;
    // The carving format id: "mp3", "wav", "m4a" or "aac".
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

}  // namespace recovery::test::audio_samples
