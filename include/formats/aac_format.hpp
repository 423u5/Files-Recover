#pragma once

// Raw AAC (ADTS: Audio Data Transport Stream, ISO/IEC 13818-7 and 14496-3)
// carving and structural validation: the .aac files of encoders and radio
// recordings. AAC inside an MPEG-4 container is M4aFormat.
//
// An ADTS file is a stream of frames, each with a 7-byte header (9 with a
// CRC) that records the frame's length, optionally with ID3v2 tags before it
// and APEv2, Lyrics3 and ID3v1 tags after it. The walk follows the frames
// while their headers agree with the first one (MPEG version, profile,
// sampling frequency, channel configuration, protection); nothing records
// where the stream ends. Only the headers are checked: the raw data blocks
// are Huffman-coded and are not decoded. See docs/formats/audio.md.
//
// Every frame is a signature hit, so the format is self-synchronizing.

#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/config.hpp"

#include <cstdint>
#include <span>

namespace recovery::formats {

class AacFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // Frames a stream needs.
    static constexpr std::uint64_t kMinimumFrames = 4;
    // Four of the smallest frames: a header and one byte of data.
    static constexpr std::uint64_t kMinimumSize = kMinimumFrames * 8;
    static constexpr std::uint64_t kMaximumSize = 1 * kGiB;
    static constexpr std::uint32_t kHeaderSize = 4096;

    AacFormat();

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    // An ID3v2 header, or an ADTS header followed, as far as the header
    // holds them, by frames whose headers agree with it.
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // StructureWalk: the end of the last frame that agrees with the first, then its trailing tags.
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

}  // namespace recovery::formats
