#pragma once

// The interface every carvable file format implements. A format module
// describes its files (signatures, size limits, how their end is found and
// how they are extracted) and supplies the format-specific steps of carving:
// header detection, end detection and structural validation. The scanner and
// the carver only use this interface, so a new format is added by
// implementing it and registering the format (format_registry.hpp), without
// changing either of them.

#include "carving/content_reader.hpp"
#include "carving/file_signature.hpp"
#include "carving/format_validator.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recovery::carving {

// How a format finds where its files end. Recorded in each candidate as evidence.
enum class EndDetectionMethod : std::uint8_t {
    // A header field records the file's size (BMP, RIFF).
    SizeField,
    // The structure is walked element by element to its last one (PNG chunks,
    // JPEG segments, ISO BMFF boxes).
    StructureWalk,
    // A marker that only occurs at the end of a file is searched for.
    EndMarker,
    // The format has no way to tell: the length is its estimate.
    None,
};

[[nodiscard]] std::string_view toString(EndDetectionMethod method) noexcept;

// How a file's bytes are taken from the source once its end is known.
enum class ExtractionStrategy : std::uint8_t {
    // The bytes from the file's start to its detected end, in one piece.
    // Fragmented files are reconstructed by a subsystem of their own (P13,
    // recovery/fragment_recovery.hpp), which takes carves that break as seeds.
    Contiguous,
};

[[nodiscard]] std::string_view toString(ExtractionStrategy strategy) noexcept;

struct FormatDescriptor {
    static constexpr std::size_t kMaxIdLength = 32;
    static constexpr std::size_t kMaxExtensionLength = 16;
    static constexpr std::size_t kMaxSignatures = 16;
    static constexpr std::uint32_t kMaxHeaderSize = 64 * kKiB;
    static constexpr std::uint64_t kMaxMaximumSize = 1ULL << 40;  // 1 TiB

    // Registry key: 1 to kMaxIdLength lower-case ASCII letters, digits, '-'
    // and '_' ("jpeg").
    std::string id;
    // Human-readable, for reports ("JPEG image").
    std::string name;
    // Extension of carved files: 1 to kMaxExtensionLength lower-case ASCII
    // letters and digits, without the dot ("jpg").
    std::string extension;
    // 1 to kMaxSignatures signatures, each valid (validateSignature).
    std::vector<FileSignature> signatures;
    // Smallest plausible file. At least the reach of every signature. A hit
    // with fewer bytes left before the end of the source is rejected, and so
    // is a file whose detected length is smaller.
    std::uint64_t minimumSize = 0;
    // Largest reasonable file, at most kMaxMaximumSize. End detection and
    // validation never read beyond it.
    std::uint64_t maximumSize = 0;
    // Bytes of a file's start passed to IFileFormat::checkHeader (fewer when
    // the data ends earlier). At least the reach of every signature, at most
    // kMaxHeaderSize.
    std::uint32_t headerSize = 0;
    EndDetectionMethod endDetection = EndDetectionMethod::StructureWalk;
    ExtractionStrategy extraction = ExtractionStrategy::Contiguous;
    // The format's files are made of elements that each start with one of
    // its signatures, so every element of a file is a hit: the frames of a
    // stream (MPEG audio, ADTS; a stream is a file of its own from any frame
    // on), or the members of an archive (ZIP local headers). FileCarver::run
    // skips hits of such a format strictly inside an earlier candidate of the
    // same format, whatever that candidate's end and verdict: they are its own
    // elements (CarveOptions::skipHitsInsideValidCandidates).
    bool selfSynchronizing = false;
    // A file whose end detection Found its end has every byte in place, one
    // after the other: the structure locates each element from the file's
    // start (ZIP: the central directory records every member's offset), so
    // a walk across a gap would not have found the end. An Invalid verdict on
    // such a file is damage, not fragmentation: fragment reconstruction (P13)
    // does not take it as a seed. Files whose end was not found are seeds as
    // for any format.
    bool foundEndMeansContiguous = false;
    // A file whose end was Found and that validated as Valid is trusted: the
    // hits strictly inside it are its own content and are not carved
    // (CarveOptions::skipHitsInsideValidCandidates), and fragment
    // reconstruction (P13) claims its clusters. False for a format whose
    // structure leaves the data of its files unchecked, so that another
    // file written over part of one still validates: OLE2 compound files
    // (no checksums). The hits inside such a file are carved as if it were
    // not there, and fragment reconstruction leaves its files as they were
    // found: any layout that keeps the structure in place validates,
    // whatever data it places, so none is better evidence than another.
    bool trustValidFiles = true;
};

// True for an extension FormatDescriptor::extension allows: 1 to
// kMaxExtensionLength lower-case ASCII letters and digits.
[[nodiscard]] bool isValidExtension(std::string_view extension) noexcept;

// Checks every rule stated in FormatDescriptor.
[[nodiscard]] Status validateDescriptor(const FormatDescriptor& descriptor);

// Result of header detection.
struct HeaderCheck {
    bool plausible = false;
    // Why the header was rejected (for diagnostics; never file content).
    std::string reason;

    [[nodiscard]] static HeaderCheck accept() { return HeaderCheck{true, {}}; }
    [[nodiscard]] static HeaderCheck reject(std::string reason) { return HeaderCheck{false, std::move(reason)}; }
};

enum class EndStatus : std::uint8_t {
    // The structure says where the file ends (a recorded size, an end marker,
    // the last element of the structure).
    Found,
    // The data available ends before the structure does: the file continues
    // beyond the end of the source or beyond the format's maximum size.
    Truncated,
    // The structure is broken: the bytes before `length` are consistent with
    // the format, the bytes at `length` are not (overwritten, fragmented, or
    // not a file of this format after all).
    Broken,
    // The format cannot tell where the file ends.
    Unknown,
};

[[nodiscard]] std::string_view toString(EndStatus status) noexcept;

struct EndDetection {
    EndStatus status = EndStatus::Unknown;
    // Found: the file's length. Truncated: the bytes available that belong to
    // the file. Broken: the consistent bytes before the break. Unknown: the
    // format's estimate. Never more than the content it was given.
    std::uint64_t length = 0;
    // What decided the end (for evidence; never file content).
    std::string detail;
    // The extension of this file when its content says more than its
    // format's (a ZIP archive that is a DOCX document: "docx"; a compound
    // file that is a Word document: "doc"), as isValidExtension() allows;
    // empty for the format's own (FormatDescriptor::extension). The carver
    // names the file by it.
    std::string extension;
};

// A carvable file format.
//
// Thread safety: formats are immutable once registered. Every member
// function is const and must be safe to call concurrently. descriptor() must
// return the same values for the lifetime of the format.
class IFileFormat {
public:
    virtual ~IFileFormat() = default;

    [[nodiscard]] virtual const FormatDescriptor& descriptor() const noexcept = 0;

    // Header detection: whether a file of this format plausibly starts here.
    // `header` holds the first min(descriptor().headerSize, bytes available)
    // bytes of the would-be file, and it holds the matched signature. Called
    // for every signature hit, so it should be cheap.
    [[nodiscard]] virtual HeaderCheck checkHeader(std::span<const std::byte> header) const = 0;

    // End detection: where the file that starts at content offset 0 ends.
    // content.size() is the smaller of descriptor().maximumSize and the bytes
    // left on the source, and never less than descriptor().minimumSize.
    // Structural problems are reported through EndDetection; errors are only
    // the reader's (cancellation, source failures).
    [[nodiscard]] virtual Result<EndDetection> findEnd(IContentReader& content) const = 0;

    // Structural validation of this format's files.
    [[nodiscard]] virtual const FormatValidator& validator() const noexcept = 0;

protected:
    IFileFormat() = default;
    IFileFormat(const IFileFormat&) = default;
    IFileFormat& operator=(const IFileFormat&) = default;
    IFileFormat(IFileFormat&&) = default;
    IFileFormat& operator=(IFileFormat&&) = default;
};

}  // namespace recovery::carving
