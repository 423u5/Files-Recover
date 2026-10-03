#pragma once

// JPEG (ITU T.81, JFIF, Exif) carving and structural validation.
//
// A JPEG file is walked marker by marker from SOI to EOI: length-prefixed
// segments are skipped by their lengths (so an Exif thumbnail inside APP1
// never ends the file early), and the entropy-coded data after each SOS is
// scanned byte by byte for the marker that ends it, honouring byte stuffing
// (FF 00), fill bytes and restart markers. See docs/formats/images.md.

#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/config.hpp"

#include <cstdint>
#include <span>

namespace recovery::formats {

class JpegFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // SOI, the smallest frame header (13) and scan header (10), one byte of
    // entropy-coded data, and EOI.
    static constexpr std::uint64_t kMinimumSize = 28;
    static constexpr std::uint64_t kMaximumSize = 256 * kMiB;
    static constexpr std::uint32_t kHeaderSize = 4 * static_cast<std::uint32_t>(kKiB);
    // A longer run of 0xFF fill bytes (before a marker, or in entropy-coded
    // data) is taken as erased media, not as part of the file.
    static constexpr std::uint64_t kMaxFillBytes = 4 * kKiB;

    JpegFormat();

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    // SOI, then a marker that can start a JPEG (APPn, DQT, DHT, a frame
    // header, COM, DRI, DAC) and the chain of segments that fits in the header.
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // StructureWalk: the end of EOI.
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    // The marker walk plus the contents of frame, scan, table and restart
    // segments, restart marker order and count, and marker order.
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

}  // namespace recovery::formats
