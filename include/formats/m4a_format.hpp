#pragma once

// M4A (MPEG-4 audio in the ISO base media file format, ISO/IEC 14496-12 and
// 14496-14) carving and structural validation: AAC and ALAC music, audiobooks,
// and the voice recordings of phones.
//
// An M4A file is a sequence of boxes, [size BE32][type] (size 1: a 64-bit size
// follows), starting with ftyp. End detection follows the top-level boxes:
// moov and mdat in either order, free, skip, wide, udta and the other
// top-level types, until something that is not a box of the file follows once
// both moov and mdat have been seen. A box's size is trusted only if it is
// plausible; the next ftyp always ends the file. The MP4 video format (P12,
// mp4_format.hpp) walks the same way (src/formats/iso_walk.hpp). See
// docs/formats/audio.md.
//
// A file is audio when its major brand is an audio brand (M4A, M4B, M4P, F4A,
// F4B), or when it has a generic brand (isom, mp42, 3gp4, ...) and moov holds
// a sound track and no video track (mp4::classify, shared with the MP4
// format). Files with a video or image brand, and files with a generic brand
// that are not audio, are not M4A: end detection reports them Broken at 0
// bytes, which the carver rejects; the MP4 format carves the video ones.
//
// Validation is the dedicated parser of P11 (mp4_parser.hpp): the boxes, the
// movie, the sample tables, and every chunk inside the media data (P12
// moved M4A onto it).

#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/config.hpp"

#include <cstdint>
#include <span>

namespace recovery::formats {

class M4aFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // ftyp with a major brand, and the headers of moov and mdat.
    static constexpr std::uint64_t kMinimumSize = 16 + 8 + 8;
    static constexpr std::uint64_t kMaximumSize = 4 * kGiB;
    static constexpr std::uint32_t kHeaderSize = 4096;
    // Largest ftyp box accepted (a few dozen bytes in practice).
    static constexpr std::uint32_t kMaxFtypSize = 1024;

    M4aFormat();

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    // An ftyp box of 16 to kMaxFtypSize bytes (a whole number of brands) with a
    // printable major brand that is not a video or image brand, and, when the
    // header holds it, a plausible box header after it.
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // StructureWalk: the end of the last top-level box.
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

}  // namespace recovery::formats
