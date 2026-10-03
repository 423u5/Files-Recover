#pragma once

// PNG (ISO/IEC 15948, W3C PNG 3rd edition) carving and structural validation.
//
// A PNG file is walked chunk by chunk from the signature to IEND. End
// detection follows the chunk lengths; validation also checks every chunk's
// CRC-32, IHDR's fields, the order of the critical chunks, and that the image
// data starts a zlib stream. See docs/formats/images.md.

#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/config.hpp"

#include <cstdint>
#include <span>

namespace recovery::formats {

class PngFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // Signature, IHDR, an IDAT holding a zlib header, and IEND.
    static constexpr std::uint64_t kMinimumSize = 8 + 25 + 14 + 12;
    static constexpr std::uint64_t kMaximumSize = 1 * kGiB;
    // Signature and IHDR.
    static constexpr std::uint32_t kHeaderSize = 8 + 25;

    PngFormat();

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    // The signature, then an IHDR chunk with valid fields and a matching CRC.
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // StructureWalk: the end of IEND.
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

}  // namespace recovery::formats
