#pragma once

// GIF (87a and 89a) carving and structural validation.
//
// A GIF file is walked block by block from the header to the trailer (0x3B):
// the logical screen descriptor and color tables by their sizes, image
// descriptors and extensions by their chains of data sub-blocks. See
// docs/formats/images.md.

#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/config.hpp"

#include <cstdint>
#include <span>

namespace recovery::formats {

class GifFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // Header, logical screen descriptor, one image descriptor with its code
    // size, one data sub-block and terminator, and the trailer.
    static constexpr std::uint64_t kMinimumSize = 6 + 7 + 10 + 1 + 2 + 1 + 1;
    static constexpr std::uint64_t kMaximumSize = 256 * kMiB;
    // Header, logical screen descriptor, the largest global color table and
    // the first block's introducer.
    static constexpr std::uint32_t kHeaderSize = 6 + 7 + 768 + 1;

    GifFormat();

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    // "GIF87a" or "GIF89a", and a valid block introducer after the global color table.
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // StructureWalk: the end of the trailer.
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

}  // namespace recovery::formats
