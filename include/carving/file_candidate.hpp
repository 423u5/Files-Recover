#pragma once

// Carved file candidates: files found by their content (a signature, then
// the format's own header, end and structure checks), with where their bytes
// are on the source and the evidence they were found with.
//
// A FileCandidate is carving's own result. P14 unifies it with the recovery
// candidates found through filesystem metadata (recovery_candidate.hpp).

#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/result.hpp"
#include "recovery/strong_types.hpp"
#include "storage/bad_region.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::carving {

using FileCandidateId = StrongValue<struct FileCandidateIdTag, std::uint64_t>;

// File bytes [fileOffset, fileOffset + length) are stored at sourceOffset.
struct CarvedExtent {
    std::uint64_t fileOffset = 0;
    std::uint64_t sourceOffset = 0;
    std::uint64_t length = 0;

    friend bool operator==(const CarvedExtent&, const CarvedExtent&) = default;
};

struct SignatureEvidence {
    std::size_t signatureIndex = 0;
    std::string signatureName;
    // Source offset of the matched pattern.
    std::uint64_t matchOffset = 0;
};

struct EndEvidence {
    EndDetectionMethod method = EndDetectionMethod::None;
    EndStatus status = EndStatus::Unknown;
    std::string detail;
    // Bytes end detection could read: the smaller of the format's maximum
    // size and the bytes left on the source.
    std::uint64_t available = 0;
};

// Consequences of the evidence that a user (or a later stage) must know
// about. Each has one precise cause.
enum class CarveWarning : std::uint8_t {
    // End detection: Truncated, and the source ends before the maximum size.
    TruncatedBySourceEnd,
    // End detection: Truncated, and the maximum size ends before the source.
    TruncatedByMaximumSize,
    // End detection: Broken. The candidate holds the consistent bytes before the break.
    StructureBroken,
    // End detection: Unknown. The length is the format's estimate.
    EndUnknown,
    // Some bytes could not be read from the source; the carve saw zeros there.
    UnreadableData,
};

[[nodiscard]] std::string_view toString(CarveWarning warning) noexcept;

struct FileCandidate {
    FileCandidateId id{0};
    // FormatDescriptor::id and ::extension of the format that carved it.
    std::string formatId;
    std::string extension;
    // Source offset of the file's first byte.
    std::uint64_t sourceOffset = 0;
    std::uint64_t length = 0;
    ExtractionStrategy extraction = ExtractionStrategy::Contiguous;
    // The file's bytes in file order: contiguous in file offsets from 0 to
    // length, none empty (one extent for contiguous extraction).
    std::vector<CarvedExtent> extents;
    SignatureEvidence signature;
    EndEvidence end;
    ValidationResult validation;
    // Unreadable source ranges inside the file (the carve saw zeros), merged.
    std::vector<storage::BadRegion> unreadableRegions;
    std::vector<CarveWarning> warnings;

    [[nodiscard]] bool hasWarning(CarveWarning warning) const noexcept;
    // Source offset one past the file's last byte, for contiguous candidates.
    [[nodiscard]] std::uint64_t sourceEnd() const noexcept { return sourceOffset + length; }
};

// Checks the structural invariants of a candidate: a length of at least 1,
// extents contiguous from 0 to length, none empty, no offset overflow, and
// for contiguous extraction exactly one extent at sourceOffset. Candidates
// may come from outside the engine (a saved session), so readers call this
// before using one.
[[nodiscard]] Status validateFileCandidate(const FileCandidate& candidate);

}  // namespace recovery::carving
