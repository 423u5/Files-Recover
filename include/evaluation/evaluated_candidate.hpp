#pragma once

// Evaluated candidates (P14): the one model of a recovered file that every
// way of finding one ends in (filesystem metadata, carving, MP4 recovery,
// fragment reconstruction), with all the evidence it was found with, its
// validation levels (validation.hpp) and its content identity
// (content_identity.hpp). CandidateEvaluation (candidate_evaluation.hpp)
// builds them, one per file.
//
// The data layout is P7's RecoveryCandidate, as it is, so reconstructCandidate
// and RecoveryWriter take `data` unchanged. The fields of the plan's
// RecoveryCandidate are these:
//
//   id                  id
//   format              formatId (FormatDescriptor::id; empty: no format known)
//   filename            data.filename (data.extension)
//   sourceOffset        sourceOffset()
//   sourceRegions       data.sourceRegions
//   expectedSize        data.expectedSize
//   recoveredSize       recoveredSize() (identity.size)
//   filesystemEvidence  data.filesystemEvidence, when hasFilesystemEvidence()
//   signatureEvidence   signature() (carve->signature)
//   structureEvidence   validation.structural, carve (end detection and the
//                       carve's own verdict), mp4, fragments
//   fragmentationCount  fragmentationCount() (data.fragmentation)
//   validationStatus    validationStatus() (validation.status())
//   recoveryMethod      data.method
//   warnings            data.warnings (the layout), carve->warnings (the
//                       carve), mp4Warnings (the MP4 structure), warnings
//                       (the evaluation)
//
// Nothing here is a confidence score: every field is a fact about the
// evidence or the result of one check, and explain() says them in words.

#include "carving/file_candidate.hpp"
#include "evaluation/content_identity.hpp"
#include "filesystem/filesystem.hpp"
#include "recovery/fragment_recovery.hpp"
#include "recovery/mp4_recovery.hpp"
#include "recovery/recovery_candidate.hpp"
#include "recovery/strong_types.hpp"
#include "validation/validation.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::evaluation {

using EvaluatedCandidateId = StrongValue<struct EvaluatedCandidateIdTag, std::uint64_t>;

// Where a carved file lies in a volume's allocation now.
struct AllocationEvidence {
    filesystem::FilesystemType filesystem = filesystem::FilesystemType::Fat32;
    std::uint64_t volumeOffset = 0;
    // The volume's clusters the file covers, by their state now.
    std::uint64_t clusters = 0;
    std::uint64_t freeClusters = 0;
    std::uint64_t allocatedClusters = 0;
    // Bad, invalid or unreadable in the allocation table or bitmap.
    std::uint64_t otherClusters = 0;
    // False when only EvaluationOptions::maxClusterChecks were checked.
    bool complete = true;
    // Paths of active files whose data overlaps the file (the first eight).
    std::vector<std::string> activeFiles;
    // The file lies entirely inside one active file's data (a thumbnail in
    // a photo, a video at the end of a motion photo): it is part of that
    // file, not a lost one.
    bool insideActiveFile = false;
};

// What fragment reconstruction (P13) found.
struct FragmentEvidence {
    // The FragmentCandidate's id in its run.
    CandidateId fragmentCandidate{0};
    ReconstructionStatus status = ReconstructionStatus::Unrecoverable;
    std::string reason;
    SeedOrigin origin = SeedOrigin::Filesystem;
    // The delivered layout: how it was found, the clusters it places, and
    // what it rests on (none for Unrecoverable).
    std::optional<LayoutSource> source;
    std::vector<ClusterRun> clusters;
    std::optional<HypothesisEvidence> evidence;
    // Layouts the search reported, and how many of them are tied (Ambiguous).
    std::size_t hypotheses = 0;
    std::size_t tied = 0;
    // Ambiguous: this candidate is the alternative-th (1-based) of the tied layouts.
    std::size_t alternative = 0;
    SearchStats search;
};

// Consequences of the evaluation that a user must know about. Each has one
// precise cause.
enum class EvaluationWarning : std::uint8_t {
    // The structural level failed.
    StructureInvalid,
    // A level found the content cut short.
    ContentTruncated,
    // The media level failed.
    MediaInvalid,
    // The playability level failed.
    PlaybackFailed,
    // No registered format recognises the content: no level could check it.
    FormatUnknown,
    // Some stored bytes could not be read (bad sectors, beyond the end of the
    // source): they are zeros in the content, its checks and its hash.
    DataUnreadable,
    // The content equals an earlier candidate's (duplicateOf).
    DuplicateContent,
    // One of several layouts the evidence supports equally (fragment
    // reconstruction AMBIGUOUS): none is chosen, all are delivered.
    AlternativeLayout,
    // Fragment reconstruction found no layout that validates; the
    // metadata's (or the carve's) layout is kept.
    ReconstructionFailed,
    // A carved file that lies inside an active file's data (container).
    InsideActiveFile,
};

[[nodiscard]] std::string_view toString(EvaluationWarning warning) noexcept;

struct EvaluatedCandidate {
    EvaluatedCandidateId id{0};
    // The carving format its content was validated as; empty when none.
    std::string formatId;
    // The layout: what recovery writes. data.id is the stage's own id (P7's,
    // the MP4 run's, the fragment run's), not this candidate's.
    RecoveryCandidate data;
    // The candidates of the stages that found it: P7's (in its scan) and the
    // MP4 run's.
    std::optional<CandidateId> filesystemCandidate;
    std::optional<CandidateId> mp4Candidate;
    // The carve that found the file, or that starts where the file starts,
    // and other carves that start there too (other formats, or another
    // carve of the same one).
    std::optional<carving::FileCandidate> carve;
    std::vector<carving::FileCandidate> otherCarves;
    // MP4 recovery's structure and sample evidence, and its warnings.
    std::optional<Mp4Structure> mp4;
    std::vector<Mp4Warning> mp4Warnings;
    std::optional<FragmentEvidence> fragments;
    // Carved files on a volume: where they lie in its allocation.
    std::optional<AllocationEvidence> allocation;
    // The active file a carved file lies inside (an earlier candidate).
    std::optional<EvaluatedCandidateId> container;
    validation::ValidationState validation;
    ContentIdentity identity;
    // Stored bytes that read as zeros (bad sectors, beyond the source).
    std::uint64_t unreadableBytes = 0;
    // The earlier candidate with the same content.
    std::optional<EvaluatedCandidateId> duplicateOf;
    std::vector<EvaluationWarning> warnings;

    [[nodiscard]] std::optional<std::uint64_t> sourceOffset() const noexcept { return data.sourceOffset(); }
    [[nodiscard]] std::uint64_t recoveredSize() const noexcept { return identity.size; }
    [[nodiscard]] std::size_t fragmentationCount() const noexcept { return data.fragmentation.fragmentCount; }
    [[nodiscard]] carving::ValidationStatus validationStatus() const noexcept { return validation.status(); }
    [[nodiscard]] RecoveryMethod recoveryMethod() const noexcept { return data.method; }
    // Filesystem metadata names the file (filesystem, hybrid, and
    // reconstructions of a deleted file).
    [[nodiscard]] bool hasFilesystemEvidence() const noexcept { return filesystemCandidate.has_value(); }
    [[nodiscard]] const carving::SignatureEvidence* signature() const noexcept {
        return carve.has_value() ? &carve->signature : nullptr;
    }
    [[nodiscard]] bool hasWarning(EvaluationWarning warning) const noexcept;
};

// The evidence and the checks of a candidate in words, one line per fact,
// never file content: what the file is and where it comes from, the
// filesystem, carving, MP4 and fragment evidence, each validation level, the
// content identity and the warnings.
[[nodiscard]] std::vector<std::string> explain(const EvaluatedCandidate& candidate);

}  // namespace recovery::evaluation
