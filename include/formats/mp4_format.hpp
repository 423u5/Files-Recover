#pragma once

// MP4 carving and structural validation (P12): MP4, MOV (QuickTime), M4V and
// 3GP video in the ISO base media file format, built on the dedicated parser
// of P11 (mp4_parser.hpp) and the analysis of mp4_analysis.hpp.
//
// Header detection: an ftyp box of 16 to kMaxFtypSize bytes (a whole number
// of brands) whose major brand is printable and neither an audio brand (M4A's)
// nor an image brand (HEIF, AVIF, CR3, JPEG 2000), and, when the header holds
// it, a plausible box header after it.
//
// End detection (ftyp detection, then moov and mdat discovery): the top-level
// walk the M4A format uses (src/formats/iso_walk.hpp): moov before, between or
// after the mdat boxes, movie fragments (moof and mdat pairs) after moov, and
// the next ftyp ends the file. A generic brand whose moov holds only sound
// tracks is an audio file: it is left to M4A (Broken at 0). A generic brand
// without moov is taken as video (docs/formats/mp4.md). An mdat of
// size 0 ("to the end of the file", as recorders leave it until they finish)
// after moov ends where the sample tables say its media data ends.
//
// Validation: the parser (boxes, movie, sample tables, movie fragments,
// every chunk and run inside the media data, no overlaps), then the framing
// of every AVC and HEVC sample (mp4::checkSampleFraming), which notices
// samples that other data replaced or that are not where the tables say.

#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "formats/mp4_analysis.hpp"
#include "formats/mp4_parser.hpp"
#include "recovery/config.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace recovery::formats {

struct Mp4FormatOptions {
    // Bounds on each parse (end detection and validation).
    mp4::ParseLimits limits;
    // Validation walks the NAL units of every AVC and HEVC sample (reading
    // the samples' length fields, so about all of the media data).
    bool checkSampleFraming = true;
};

class Mp4Format final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // ftyp with a major brand, and the headers of moov and mdat.
    static constexpr std::uint64_t kMinimumSize = 16 + 8 + 8;
    // Cameras split recordings at 4 GiB on FAT32, but not on exFAT or NTFS.
    static constexpr std::uint64_t kMaximumSize = 256 * kGiB;
    static constexpr std::uint32_t kHeaderSize = 4096;
    // Largest ftyp box accepted (a few dozen bytes in practice).
    static constexpr std::uint32_t kMaxFtypSize = 1024;

    explicit Mp4Format(Mp4FormatOptions options = {});

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // StructureWalk: the end of the last top-level box of the file.
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

    [[nodiscard]] const Mp4FormatOptions& options() const noexcept { return options_; }

private:
    carving::FormatDescriptor descriptor_;
    Mp4FormatOptions options_;
};

// Mp4Format's verdict on content of `contentSize` bytes that parses as
// `file`, with the sample framing of its movie when it was checked: for
// callers that parsed the content already (MP4 recovery).
[[nodiscard]] carving::ValidationResult mp4Verdict(const mp4::Mp4File& file, std::uint64_t contentSize,
                                                   const std::optional<mp4::FramingCheck>& framing);

}  // namespace recovery::formats
