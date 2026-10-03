#pragma once

// BMP (Windows and OS/2 device-independent bitmap files) carving and
// structural validation.
//
// A BMP file has a 14-byte file header ("BM", the file size, the pixel data
// offset) and a DIB header whose size identifies its version (OS/2 core 12,
// OS/2 2.x 16 or 64, Windows 40, 52, 56, 108 or 124 bytes). The pixel data's
// size follows from the dimensions, bit depth and compression. See
// docs/formats/images.md.

#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/config.hpp"

#include <cstdint>
#include <span>

namespace recovery::formats {

class BmpFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // File header, core DIB header and one 24-bit pixel.
    static constexpr std::uint64_t kMinimumSize = 14 + 12 + 4;
    static constexpr std::uint64_t kMaximumSize = 2 * kGiB;
    // File header, the largest DIB header (BITMAPV5HEADER) and alpha bit field masks.
    static constexpr std::uint32_t kHeaderSize = 14 + 124 + 16;

    BmpFormat();

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    // A known DIB header size, one plane, a bit depth and compression that go
    // together, non-zero dimensions, and a pixel data offset past the headers.
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // SizeField: the file size field when it covers what the headers need,
    // otherwise the end of the pixel data (and of an embedded color profile).
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    // The headers' consistency with each other and with the size, and for
    // RLE-compressed bitmaps the RLE data up to its end-of-bitmap code.
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

}  // namespace recovery::formats
