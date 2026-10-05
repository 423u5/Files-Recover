#pragma once

// Candidate evaluation (P14): the candidates of every stage, merged into one
// candidate per file, each validated at every level and identified by its
// content.
//
//   filesystem candidates (P7) ─┐
//   carves (P8-P10, P12) ───────┼─► one per file ─► validation levels ─► SHA-256 ─► duplicates ─► sink
//   MP4 candidates (P12) ───────┤
//   fragment candidates (P13) ──┘
//
// One candidate per file, merged by where the file starts on the source:
//
//  * A fragment reconstruction replaces its seed's layout: the deleted
//    file's guessed layout, or the broken carve. AMBIGUOUS gives one
//    candidate per tied layout (AlternativeLayout); UNRECOVERABLE keeps the
//    seed's layout, with the reconstruction's evidence.
//  * An MP4 candidate replaces its filesystem candidate and its carve.
//  * A carve that starts where a filesystem candidate starts is the same
//    file, as P12 rules for MP4 and as is applied here to every format: the
//    metadata's layout stands when its data validates; otherwise a carve that
//    validates gives the layout (HYBRID: the metadata's name, the carve's
//    layout, with the clusters a deleted file's carve covers that are
//    allocated now marked reallocated); otherwise the metadata's layout
//    stands. A deleted file whose first cluster is allocated to other data
//    now takes no carve: what starts there is the new owner's file. Other
//    carves that start at the same place are kept as evidence.
//  * Any other carve is a file of its own (CARVING), with where it lies in
//    the volume's allocation; one inside an active file's data is linked to
//    that file (InsideActiveFile, container).
//
// Each candidate is then validated (validateContent): its format is the
// carve's, MP4 recovery's or the reconstruction's, and for filesystem
// candidates the registered format whose header check accepts the content
// (the one of the file's extension when several do, and when none does: a
// damaged header then fails the structure instead of hiding the format). The format's verdict on
// exactly the candidate's bytes is reused when the evidence holds it (a
// fragment hypothesis, a carve that is the whole layout). Its content is
// hashed (SHA-256 and the preliminary hash), and a later candidate with the
// content of an earlier one is its duplicate (identified, not removed).
//
// Delivery: the candidates of the added volumes first (volume by volume, in
// scan order, each in the place of the filesystem candidate it comes from),
// then the others in source order. Ids follow from EvaluationOptions::firstId.
//
// The source is only read. See docs/recovery/candidate_evaluation.md.

#include "carving/content_reader.hpp"
#include "carving/file_candidate.hpp"
#include "carving/format_registry.hpp"
#include "evaluation/content_identity.hpp"
#include "evaluation/evaluated_candidate.hpp"
#include "recovery/filesystem_recovery.hpp"
#include "recovery/fragment_recovery.hpp"
#include "recovery/mp4_recovery.hpp"
#include "recovery/result.hpp"
#include "recovery/worker_pool.hpp"
#include "storage/storage_source.hpp"
#include "validation/media_validator.hpp"
#include "validation/validation.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace recovery::evaluation {

// What a run needs to know of a candidate an earlier run delivered, to go on
// as if it had delivered it itself (P15): its id, how it counts in the
// report, and its content identity (for duplicates).
struct EvaluationRecord {
    EvaluatedCandidateId id{0};
    RecoveryMethod method = RecoveryMethod::Filesystem;
    carving::ValidationStatus validationStatus = carving::ValidationStatus::NotValidated;
    ContentIdentity identity;
    bool duplicate = false;
    bool alternative = false;
};

[[nodiscard]] EvaluationRecord recordOf(const EvaluatedCandidate& candidate);

struct EvaluationOptions {
    static constexpr std::size_t kMaxWindow = 4096;

    // Id of the first candidate; the others follow in delivery order.
    std::uint64_t firstId = 1;
    // The levels: media on, playability off (and its checker) by default.
    validation::ValidationOptions validation;
    IdentityOptions identity;
    // Reads of the candidates' data (cancellation, known bad regions, retries).
    carving::SourceReadOptions reads;
    std::size_t readCacheSize = carving::SourceContentReader::kDefaultCacheSize;
    // Clusters checked against a volume's allocation per carved file, at most.
    std::uint64_t maxClusterChecks = std::uint64_t{1} << 22;
    // P15: candidates are validated and hashed on this pool, at most `window`
    // at a time (0: twice the pool's threads, at most kMaxWindow), and still
    // delivered one by one, in order, on the thread of run(): what run()
    // delivers does not depend on the pool. Null: all on the thread of run().
    WorkerPool* pool = nullptr;
    std::size_t window = 0;
    // P15: the records of the candidates an earlier run with the same inputs
    // and options delivered, in delivery order: run() does not evaluate or
    // deliver them again, and goes on as if it had delivered them itself (the
    // next ids, duplicates of their content, the report's counts).
    std::vector<EvaluationRecord> resume;
};

// InvalidInput when a limit is 0, the read or validation options are
// invalid, the window is too large, or the resume records are not numbered
// from firstId on.
[[nodiscard]] Status validate(const EvaluationOptions& options);

struct EvaluationReport {
    // Candidates delivered, by recovery method.
    std::uint64_t filesystem = 0;
    std::uint64_t carving = 0;
    std::uint64_t hybrid = 0;
    std::uint64_t fragmented = 0;
    // By validation status.
    std::uint64_t valid = 0;
    std::uint64_t truncated = 0;
    std::uint64_t invalid = 0;
    std::uint64_t notValidated = 0;
    // Candidates whose content an earlier one has.
    std::uint64_t duplicates = 0;
    // Carves attached to a filesystem candidate of the same file (its layout
    // or its evidence), and carves kept as evidence beside another.
    std::uint64_t carvesMerged = 0;
    std::uint64_t carvesCompeting = 0;
    // Inputs replaced by an MP4 candidate or a fragment reconstruction.
    std::uint64_t superseded = 0;
    // Candidates delivered as one of several tied layouts.
    std::uint64_t alternatives = 0;
    // Content bytes hashed.
    std::uint64_t bytesHashed = 0;
    std::chrono::milliseconds elapsed{0};

    [[nodiscard]] std::uint64_t candidates() const noexcept { return filesystem + carving + hybrid + fragmented; }
};

// Receives the candidates in delivery order. An error stops the run and is
// returned by it (Cancelled too).
using EvaluatedCandidateSink = std::function<Status(EvaluatedCandidate&& candidate)>;

// Thread safety: one owner at a time; with a pool, run() validates and
// hashes candidates on its workers (the sink is still called on the thread
// of run()). The source, the registries, the volumes, their scans and the
// pool must outlive the object.
class CandidateEvaluation {
public:
    CandidateEvaluation(storage::IStorageSource& source, const carving::FormatRegistry& formats,
                        const validation::MediaValidatorRegistry& media, EvaluationOptions options = {});

    // A volume of the source: the filesystem recovery that scanned it (for
    // its allocation) and the candidates it found. Fails with InvalidInput for
    // a scan from another volume, or a volume added already.
    [[nodiscard]] Status addVolume(FilesystemRecovery& volume, const CandidateScan& scan);
    // A carve of the source. The same carve added twice counts once. Fails
    // with InvalidInput for a malformed carve (validateFileCandidate).
    [[nodiscard]] Status addCarve(carving::FileCandidate carve);
    // An MP4 recovery candidate (Mp4Recovery's sink).
    [[nodiscard]] Status addMp4Candidate(Mp4Candidate candidate);
    // A fragment reconstruction (FragmentRecovery's sink).
    [[nodiscard]] Status addFragmentCandidate(FragmentCandidate candidate);

    // Merges, validates, hashes and delivers every candidate. Fails with
    // InvalidInput for invalid options or a source that is not open, with
    // Cancelled, with the error of a read that fails for a reason other than
    // an I/O error, and with the sink's error.
    [[nodiscard]] Result<EvaluationReport> run(const EvaluatedCandidateSink& sink);

private:
    struct Volume {
        FilesystemRecovery* recovery;
        const CandidateScan* scan;
    };

    storage::IStorageSource* source_;
    const carving::FormatRegistry* formats_;
    const validation::MediaValidatorRegistry* media_;
    EvaluationOptions options_;
    std::vector<Volume> volumes_;
    std::vector<carving::FileCandidate> carves_;
    std::vector<Mp4Candidate> mp4_;
    std::vector<FragmentCandidate> fragments_;
};

}  // namespace recovery::evaluation
