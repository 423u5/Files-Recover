#pragma once

// What a user interface shows about a candidate before anything is read
// from the source (P17): its name and where it was, its kind, format and
// size, its validation, the condition it would be recovered in, its content
// identity and its duplicates. Everything here comes from the evaluated
// candidate (P14) alone; the media metadata of its content is read on demand
// (media_metadata.hpp), and what recovery jobs did with it is in
// recovery_status.hpp.

#include "evaluation/evaluated_candidate.hpp"
#include "filesystem/filesystem.hpp"
#include "metadata/media_metadata.hpp"
#include "recovery/recovery_candidate.hpp"
#include "recovery/sha256.hpp"
#include "validation/validation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::metadata {

// The condition a candidate would be recovered in: one word for a list,
// decided by the first rule that applies, in this order.
enum class RecoveryCondition : std::uint8_t {
    // Every byte is located and read, and the structure validates; nothing
    // is known to be wrong.
    Complete,
    // Every byte is located and read and nothing is known to be wrong, but
    // nothing checked the content: no format is known, or validation did not
    // run.
    Unverified,
    // The whole file is there, but some of it is damaged: a validation level
    // failed, bytes could not be read (zeros), bytes come from clusters
    // allocated to other data now, or reconstruction found damage.
    Corrupted,
    // Part of the file is missing: bytes the metadata does not locate, content
    // cut short, or a reconstruction that placed only part of it.
    Partial,
    // Nothing of the file can be recovered: no byte is located, or
    // reconstruction found no layout that validates.
    Unrecoverable,
    // One of several layouts the evidence supports equally: which one holds
    // the file is not known.
    Ambiguous,
};

[[nodiscard]] std::string_view toString(RecoveryCondition condition) noexcept;

// The facts the condition rests on. Every fact that applies is listed, so a
// Partial file can also say it is Corrupted.
enum class ConditionReason : std::uint8_t {
    // Ambiguous: a tied layout (EvaluationWarning::AlternativeLayout).
    AlternativeLayout,
    // Unrecoverable: the file has bytes, none of them located.
    NothingLocated,
    // Unrecoverable: fragment reconstruction found no layout that validates
    // (UNRECOVERABLE, EvaluationWarning::ReconstructionFailed).
    ReconstructionFailed,
    // Partial: bytes the metadata does not locate (RegionKind::Missing).
    DataMissing,
    // Partial: a validation level found the content cut short.
    ContentTruncated,
    // Partial: fragment reconstruction placed only part of the file (PARTIAL).
    ReconstructionPartial,
    // Corrupted: a validation level failed.
    ValidationFailed,
    // Corrupted: stored bytes that could not be read, delivered as zeros.
    DataUnreadable,
    // Corrupted: a deleted file's bytes in clusters allocated to other data now.
    ClustersReallocated,
    // Corrupted: fragment reconstruction placed the file but found damage (CORRUPTED).
    ReconstructionCorrupted,
    // Unverified: no level validated the content (NotValidated).
    NotValidated,
};

[[nodiscard]] std::string_view toString(ConditionReason reason) noexcept;

struct ConditionAssessment {
    RecoveryCondition condition = RecoveryCondition::Complete;
    // In the order of the enum.
    std::vector<ConditionReason> reasons;

    [[nodiscard]] bool has(ConditionReason reason) const noexcept;
};

// The rules, first match wins: Ambiguous (a tied layout), Unrecoverable
// (nothing located, or no layout validates), Partial (missing bytes, content
// cut short, a partial reconstruction), Corrupted (a failed level, unreadable
// or reallocated bytes, a corrupted reconstruction), Unverified (not
// validated), Complete. An empty file is Complete when its format validates
// it and Unverified otherwise.
[[nodiscard]] ConditionAssessment assessCondition(const evaluation::EvaluatedCandidate& candidate);

struct CandidateMetadata {
    evaluation::EvaluatedCandidateId id{0};
    // The file name (UTF-8, as the metadata records it; "recovered_000001.jpg"
    // for a file only carving found), its path on its volume ("/DCIM/IMG_1.JPG";
    // empty without filesystem evidence) and its extension (lower case).
    std::string name;
    std::string path;
    std::string extension;
    // The filesystem says it is deleted (itself, or its directory). False
    // without filesystem evidence: carving cannot tell.
    bool deleted = false;
    RecoveryMethod method = RecoveryMethod::Filesystem;
    // From the format the evaluation validated the content as.
    MediaKind kind = MediaKind::Unknown;
    std::string formatId;
    std::string mediaType;
    // The bytes recovery writes (recoveredSize) and the size the metadata
    // or the structure gives (expectedSize).
    std::uint64_t size = 0;
    std::uint64_t expectedSize = 0;
    // The source offset of its first stored byte, and its pieces on the source.
    std::optional<std::uint64_t> sourceOffset;
    std::size_t fragments = 0;
    // Filesystem times (FAT keeps local times: see Timestamp::local).
    std::optional<filesystem::Timestamp> created;
    std::optional<filesystem::Timestamp> modified;
    // The validation: all levels together, each level, the deepest that passed.
    carving::ValidationStatus validation = carving::ValidationStatus::NotValidated;
    validation::LevelStatus structural = validation::LevelStatus::NotRun;
    validation::LevelStatus media = validation::LevelStatus::NotRun;
    validation::LevelStatus playability = validation::LevelStatus::NotRun;
    std::optional<validation::ValidationLevel> deepestPassed;
    RecoveryCondition condition = RecoveryCondition::Complete;
    std::vector<ConditionReason> reasons;
    // Content identity: SHA-256 of the bytes recovery writes, and the earlier
    // candidate with the same bytes.
    std::optional<Sha256Digest> sha256;
    std::optional<evaluation::EvaluatedCandidateId> duplicateOf;
    // The active file a carved file lies inside.
    std::optional<evaluation::EvaluatedCandidateId> container;
};

[[nodiscard]] CandidateMetadata describeCandidate(const evaluation::EvaluatedCandidate& candidate);

// Candidates with the same content: an original (the first delivered) and
// its duplicates, whatever their names.
struct DuplicateGroup {
    // The original first, then its duplicates in id order.
    std::vector<evaluation::EvaluatedCandidateId> members;
    std::optional<Sha256Digest> sha256;
    std::uint64_t size = 0;

    [[nodiscard]] evaluation::EvaluatedCandidateId original() const noexcept { return members.front(); }
};

// The duplicate groups of a list of candidates, built from the evaluation's
// duplicateOf links (P14 decides what is a duplicate: equal SHA-256 of
// non-empty content). A group holds at least two candidates; its original is
// listed even when it is not among the candidates given (a page of a longer
// list).
//
// Thread safety: const members from any thread once built.
class DuplicateGroups {
public:
    [[nodiscard]] static DuplicateGroups build(std::span<const evaluation::EvaluatedCandidate> candidates);

    // By original id.
    [[nodiscard]] const std::vector<DuplicateGroup>& groups() const noexcept { return groups_; }
    // The group the candidate belongs to (as original or duplicate), if any.
    [[nodiscard]] const DuplicateGroup* groupOf(evaluation::EvaluatedCandidateId id) const noexcept;
    // Candidates that are duplicates of another (every member but the originals).
    [[nodiscard]] std::size_t duplicateCount() const noexcept;

private:
    std::vector<DuplicateGroup> groups_;
    // Candidate id -> index into groups_.
    std::map<std::uint64_t, std::size_t> index_;
};

}  // namespace recovery::metadata
