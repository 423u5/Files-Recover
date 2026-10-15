#pragma once

// PDF documents (ISO 32000-1 and ISO 32000-2) carving and structural
// validation.
//
// A document is walked from its header to the end of its last revision:
// every indirect object (its value, and its stream's data, which /Length
// delimits or, when /Length is a reference to another object, the
// "endstream" keyword), the cross-reference tables with their trailers,
// startxref and %%EOF. What follows a %%EOF directly (an incremental update,
// the rest of a linearized file) is part of the document when the newest
// startxref's chain of cross-reference sections (/Prev, /XRefStm) reaches
// every section before it; bytes that do not continue the document end it.
//
// Validation also checks every cross-reference section against the objects
// walked (classic tables and cross-reference streams), startxref, the /Prev
// chain and the document catalog, and the data of every stream: FlateDecode
// data is inflated and its Adler-32 checked, DCTDecode data is checked as a
// JPEG image (JpegFormat's own validation), and data without a filter must
// be text, in which no file the engine carves could lie. A document is Valid
// only when every stream is checked: the carver then trusts it and skips the
// hits inside it (the JPEG images of a PDF are part of it). One with a
// stream the engine cannot check (another filter, an encrypted document,
// binary data without a filter), or whose streams unpack to more than the
// limit, is NotValidated: kept, never trusted. See docs/formats/documents.md.
//
// A document whose end was found lies in one piece: each element is found
// where the one before it ends, so a walk across a gap would not have
// reached %%EOF (FormatDescriptor::foundEndMeansContiguous).

#include "carving/content_reader.hpp"
#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "formats/deflate_decoder.hpp"
#include "formats/jpeg_format.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace recovery::formats {

// Bounds on what one document may cost (PdfFormat::Limits).
struct PdfLimits {
    // Objects, cross-reference entries and streams recorded for validation.
    // A document with more is walked to its end, its cross-reference
    // sections and its streams beyond are not checked, and it is
    // NotValidated.
    std::uint64_t maxObjects = std::uint64_t{1} << 20;
    // Bytes the streams of one document may unpack to in all. Streams beyond
    // are not checked, and the document is NotValidated.
    std::uint64_t maxUnpacked = 4 * kGiB;
    // The longest stream data searched for "endstream" when /Length is a
    // reference. A longer one breaks the walk.
    std::uint64_t maxSearchedStream = 1 * kGiB;
    // Revisions, and cross-reference sections, walked: each revision's end
    // follows the chain of sections, so their number bounds the walk's time.
    // A document with more breaks the walk there.
    std::uint64_t maxSections = 4096;
};

class PdfFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // "%PDF-1.0", an object, "startxref", an offset and "%%EOF".
    static constexpr std::uint64_t kMinimumSize = 64;
    static constexpr std::uint64_t kMaximumSize = 16 * kGiB;
    // The header and the start of the first object.
    static constexpr std::uint32_t kHeaderSize = 4 * static_cast<std::uint32_t>(kKiB);

    using Limits = PdfLimits;

    // InvalidInput when `decoder` is null or a limit is 0.
    [[nodiscard]] static Result<std::shared_ptr<const PdfFormat>> make(std::shared_ptr<const IDeflateDecoder> decoder,
                                                                       Limits limits = Limits{});

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    // "%PDF-" and a version, then (after comments) the header of an indirect
    // object or a cross-reference table within the header's bytes.
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // StructureWalk: the end of the last revision's %%EOF line.
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    PdfFormat(std::shared_ptr<const IDeflateDecoder> decoder, Limits limits);

    carving::FormatDescriptor descriptor_;
    std::shared_ptr<const IDeflateDecoder> decoder_;
    Limits limits_;
    JpegFormat jpeg_;
};

}  // namespace recovery::formats
