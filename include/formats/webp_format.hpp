#pragma once

// WebP (RFC 9649) carving and structural validation.
//
// A WebP file is a RIFF container: "RIFF", the size of what follows, "WEBP",
// then chunks of [FourCC][size LE32][payload][pad to even]. The RIFF size
// says where the file ends; the chunks must fill it exactly. Validation also
// checks the chunk layout of simple (VP8, VP8L) and extended (VP8X) files,
// animation frames, and the headers of the VP8 and VP8L bitstreams. See
// docs/formats/images.md.

#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/config.hpp"

#include <cstdint>
#include <span>

namespace recovery::formats {

class WebpFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // RIFF header and one VP8L chunk with its 5-byte header, padded.
    static constexpr std::uint64_t kMinimumSize = 12 + 8 + 6;
    static constexpr std::uint64_t kMaximumSize = 1 * kGiB;
    // RIFF header, the first chunk's header and 10 bytes of its payload.
    static constexpr std::uint32_t kHeaderSize = 12 + 8 + 10;

    WebpFormat();

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    // "RIFF" <size> "WEBP", a first chunk VP8, VP8L or VP8X that fits in the
    // RIFF size, and that chunk's own signature bytes.
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // SizeField: 8 + the RIFF size, where the chunks must end.
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

}  // namespace recovery::formats
