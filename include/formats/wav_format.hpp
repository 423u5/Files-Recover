#pragma once

// WAV (RIFF WAVE) carving and structural validation.
//
// A WAV file is a RIFF container: "RIFF", the size of what follows, "WAVE",
// then chunks of [FourCC][size LE32][payload][pad to even]. The RIFF size says
// where the file ends, and the chunks must fill it. Validation checks that
// there is exactly one fmt chunk, before exactly one data chunk; that the fmt
// fields agree with each other (block alignment, byte rate, bits per sample
// for PCM, IEEE float, A-law and mu-law, and WAVE_FORMAT_EXTENSIBLE); that the
// data is a whole number of blocks; and that LIST chunks are filled by their
// sub-chunks. The samples themselves have no structure to check. See
// docs/formats/audio.md.

#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/config.hpp"

#include <cstdint>
#include <span>

namespace recovery::formats {

class WavFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // RIFF header, a 16-byte fmt chunk and an empty data chunk.
    static constexpr std::uint64_t kMinimumSize = 12 + 8 + 16 + 8;
    // The largest RIFF size, and its header.
    static constexpr std::uint64_t kMaximumSize = 8 + 0xFFFFFFFFULL;
    static constexpr std::uint32_t kHeaderSize = 4096;

    WavFormat();

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    // "RIFF" <size> "WAVE" with room for fmt and data and not the placeholder
    // size 0xFFFFFFFF of a recording that was never finished; chunks, as far
    // as the header holds them, with printable ids that fit in the RIFF data;
    // and a fmt chunk, if it is among them, without zero fields.
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // SizeField: 8 + the RIFF size, where the chunks must end.
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

}  // namespace recovery::formats
