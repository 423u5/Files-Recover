#pragma once

// Media files of independent writers with what exiftool and ffprobe read in
// them (tests/reference/embed_metadata_samples.py): the files of
// tests/reference/make_metadata_samples.sh (FFmpeg, ExifTool, mutagen),
// embedded here, and the samples embedded in tests/support (image, audio
// and MP4 samples), found there by name. The expectations are key/value
// pairs in the engine's terms (the script lists the keys).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::test::metadata_samples {

struct Field {
    std::string_view key;
    std::string_view value;
};

struct Sample {
    std::string_view name;
    // The carving format id it is read as.
    std::string_view format;
    // Empty for the samples of tests/support (their tables name the producer).
    std::string_view producer;
    // Empty for the samples of tests/support: their bytes are there.
    std::span<const std::uint8_t> bytes;
    std::span<const Field> expected;
};

[[nodiscard]] const std::vector<Sample>& all();

}  // namespace recovery::test::metadata_samples
