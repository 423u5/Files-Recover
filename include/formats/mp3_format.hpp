#pragma once

// MP3 (MPEG-1, MPEG-2 and MPEG-2.5 Layer III audio, ISO/IEC 11172-3 and
// 13818-3) carving and structural validation.
//
// An MP3 file is a stream of frames, [header][CRC][side information][main
// data] each, with optional ID3v2 tags before it and APEv2, Lyrics3 and ID3v1
// tags after it. Nothing records where the stream ends except, optionally, an
// info tag in its first frame (Xing or Info from LAME, FFmpeg and others; VBRI
// from Fraunhofer). The walk follows the frames while their headers agree with
// the first one (version, layer, protection, sample rate, mono or not) and,
// with a Xing or Info tag, up to the number of frames it records. Validation
// also checks the frames' CRCs, their side information and bit reservoir, and
// a LAME tag's checksums of itself and of the music; a first audio frame
// whose main data begins before it means the stream's start is missing (the
// file is Invalid). See docs/formats/audio.md.
//
// Every frame is a signature hit, so the format is self-synchronizing
// (FormatDescriptor::selfSynchronizing): the carver skips the frames of a
// stream it has carved already.

#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/config.hpp"

#include <cstdint>
#include <span>

namespace recovery::formats {

class Mp3Format final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // Frames a stream needs (the plan: multiple consecutive valid frames). An
    // info tag's frame counts.
    static constexpr std::uint64_t kMinimumFrames = 4;
    // Four of the smallest frames (MPEG-2, 8 kbit/s at 24 kHz: 24 bytes).
    static constexpr std::uint64_t kMinimumSize = kMinimumFrames * 24;
    static constexpr std::uint64_t kMaximumSize = 1 * kGiB;
    // Room for three of the largest frames (MPEG-1, 320 kbit/s at 32 kHz: 1441 bytes).
    static constexpr std::uint32_t kHeaderSize = 4096;

    Mp3Format();

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    // An ID3v2 header, or a Layer III frame with plausible side information
    // (its main data fits) whose following frames, as far as the header holds
    // them, agree with it. A frame in the middle of a stream passes too: it is
    // where the rest of a stream starts after a break.
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // StructureWalk: the end of the last frame (the info tag's count, where
    // the frames stop agreeing, or another file's info tag), then its trailing tags.
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

}  // namespace recovery::formats
