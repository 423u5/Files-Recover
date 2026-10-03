#pragma once

// Small MP4, MOV and 3GP files made by independent writers: FFmpeg and GPAC's
// MP4Box (tests/reference/make_mp4_samples.sh) and Windows' Media Foundation
// (make_mp4_samples.ps1), embedded by tests/reference/embed_mp4_samples.py
// together with what FFmpeg's demuxer finds in them. They guard against the
// MP4 builder and the parser sharing a misreading of the specification.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::test::mp4_samples {

// A track as ffprobe reports it.
struct Track {
    // "video", "audio" or "other".
    std::string_view kind;
    // The sample description's format ("avc1", "mp4a", "tx3g", ...).
    std::string_view codec;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t channels;
    std::uint32_t sampleRate;
    // Samples in the sample tables and the movie fragments.
    std::int64_t samples;
    // FNV-1a 64 of every sample's offset (8 bytes, little-endian) and size
    // (4 bytes, little-endian), in order.
    std::uint64_t locations;
};

struct Sample {
    std::string_view name;
    std::string_view producer;
    std::span<const std::uint8_t> bytes;
    // The file has movie fragments (moof).
    bool fragmented;
    std::span<const Track> tracks;

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

}  // namespace recovery::test::mp4_samples
