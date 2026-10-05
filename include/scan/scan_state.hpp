#pragma once

// A scan's progress as data (P15): the updates a scan hands out at each
// consistent point, and the checkpoint they add up to, from which a later
// scan resumes without doing the work again.
//
// A scan has one output channel: its updates. Each holds the work done since
// the update before it (the volumes scanned, the carves committed, the
// candidates evaluated, ...) and the state reached, and each is a consistent
// point: everything before it is complete, nothing after it has begun. The
// evaluated candidates, the scan's results, come in the updates of the
// evaluation stage. Applying every update in order to a ScanCheckpoint gives
// what a scan needs to resume; P16 keeps the updates on disk.

#include "carving/file_candidate.hpp"
#include "carving/file_carver.hpp"
#include "carving/signature_scanner.hpp"
#include "evaluation/candidate_evaluation.hpp"
#include "evaluation/evaluated_candidate.hpp"
#include "filesystem/filesystem.hpp"
#include "partition/partition_table.hpp"
#include "recovery/config.hpp"
#include "recovery/filesystem_recovery.hpp"
#include "recovery/fragment_recovery.hpp"
#include "recovery/mp4_recovery.hpp"
#include "recovery/result.hpp"
#include "storage/bad_region.hpp"
#include "storage/storage_source.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recovery::scan {

// The stages of a scan, in the order they run. A Quick scan runs Volumes and
// Evaluation, a Deep scan all of them (a stage the configuration turns off
// completes at once, with an update of its own all the same).
enum class ScanStage : std::uint8_t {
    // Partitions, filesystems and their candidates, volume by volume.
    Volumes,
    // MP4 recovery examines the filesystem candidates (P12).
    Mp4Examination,
    // Fragment reconstruction examines deleted files with a guessed layout (P13).
    FragmentSeeds,
    // One pass over the source: carving, MP4 carving, fragment carving.
    SourcePass,
    // MP4 recovery's candidates.
    Mp4Delivery,
    // Fragment reconstruction, seed by seed.
    Fragments,
    // One candidate per file: validation levels, SHA-256, duplicates (P14).
    Evaluation,
    Completed,
};

inline constexpr std::size_t kScanStageCount = 8;

[[nodiscard]] std::string_view toString(ScanStage stage) noexcept;
// The stage after `stage` in a scan of `mode` (Completed after the last).
[[nodiscard]] ScanStage nextStage(ScanStage stage, ScanMode mode) noexcept;

// What a checkpoint belongs to: a scan of one source, with one configuration
// and one set of formats, by one engine. A scan resumes only from a
// checkpoint of the same identity.
struct ScanIdentity {
    static constexpr std::uint32_t kCheckpointVersion = 1;

    std::uint32_t checkpointVersion = kCheckpointVersion;
    std::string engineVersion;
    storage::SourceType sourceType = storage::SourceType::Synthetic;
    // UTF-8.
    std::string sourcePath;
    std::uint64_t sourceSize = 0;
    std::uint32_t sectorSize = 0;
    ScanMode mode = ScanMode::Deep;
    // The settings that decide what the scan finds, one "key=value" per line.
    std::string configuration;
    // The carving formats and the formats with a media validator, by id, in
    // registry order.
    std::vector<std::string> formats;
    std::vector<std::string> mediaValidators;

    friend bool operator==(const ScanIdentity&, const ScanIdentity&) = default;
};

// A volume of the source, as the scan found it.
struct VolumeRecord {
    // Where it lies on the source, and its size.
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    // Its partition table entry; none for a device that is one volume (or
    // whose partition table is not recognised).
    std::optional<partition::Partition> partition;
    // Set once the volume is done: the filesystem found in it and its
    // candidates, or why it could not be used (no filesystem the engine
    // reads, a damaged one).
    bool scanned = false;
    std::optional<filesystem::FilesystemType> filesystem;
    std::optional<CandidateScan> candidates;
    std::optional<Error> error;
};

// The source pass so far.
struct PassState {
    // Every file start before this offset has been examined, and the hits
    // there committed by every stage that takes part.
    std::uint64_t position = 0;
    // The signature scan, added up over every run of the pass. Hits per
    // format are by the pass's registry: the carving formats in registry
    // order, then formats the stages scan for besides (a separate MP4 format
    // when the registry has none, the moov probe of fragment reconstruction).
    carving::ScanReport scan;
    // Carving: its counts (as FileCarver::run reports them; carveBytesRead is
    // not counted, ScanMetrics::bytesRead counts every read), the state of
    // its skip rules and the id of the next carve.
    carving::CarveReport carving;
    carving::CarveSkipState skip;
    std::uint64_t nextCarveId = 1;
};

// The metrics of a scan. Counts and times add up over every run of the scan
// (a resumed scan goes on from the checkpoint's).
struct ScanMetrics {
    std::uint64_t sourceSize = 0;
    // Source bytes the source pass has examined (its position); 0 in a Quick
    // scan, which has no pass.
    std::uint64_t bytesScanned = 0;
    // Bytes read from the source, by every stage (a part read twice counts
    // twice; parts served again from the scan's cache do not).
    std::uint64_t bytesRead = 0;
    // Bytes read per second of running time in this run.
    std::uint64_t scanSpeed = 0;
    // Files the filesystems know (active and deleted), carves made by the
    // pass, MP4 candidates, fragment reconstructions.
    std::uint64_t filesFound = 0;
    std::uint64_t carves = 0;
    std::uint64_t mp4Candidates = 0;
    std::uint64_t fragmentCandidates = 0;
    // Candidates delivered (one per file), those that failed validation
    // (Invalid) and those whose content an earlier one has.
    std::uint64_t candidates = 0;
    std::uint64_t validationFailures = 0;
    std::uint64_t duplicates = 0;
    // Source bytes that could not be read.
    std::uint64_t unreadableBytes = 0;
    // Running time: time paused is not counted.
    std::chrono::milliseconds elapsed{0};
};

// One consistent point of a scan: what it did since the update before.
// Which parts are filled depends on the stage.
struct ScanUpdate {
    // 1 for a scan's first update, then one more each time, across resumes.
    std::uint64_t sequence = 0;
    ScanStage stage = ScanStage::Volumes;
    // The stage is done with this update.
    bool stageComplete = false;
    // The first update of a scan only.
    std::optional<ScanIdentity> identity;

    // Volumes: the partition table and the volumes to scan (the stage's first
    // update), then volumes done (their index in that list, their record).
    std::optional<partition::PartitionTable> partitionTable;
    std::optional<std::vector<VolumeRecord>> volumePlan;
    std::vector<std::pair<std::size_t, VolumeRecord>> volumes;

    // Mp4Examination and FragmentSeeds: candidates examined, in scan order.
    std::vector<Mp4Examination> mp4Examinations;
    std::vector<FragmentSeedExamination> seedExaminations;

    // SourcePass: the pass's state now; the carves committed since; what MP4
    // recovery's and fragment reconstruction's commits changed since.
    std::optional<PassState> pass;
    std::vector<carving::FileCandidate> carves;
    std::optional<Mp4StepsChanges> mp4Changes;
    std::vector<FragmentPassEvent> fragmentEvents;

    // Mp4Delivery: every MP4 candidate, and MP4 recovery's report.
    std::vector<Mp4Candidate> mp4Candidates;
    std::optional<Mp4RecoveryReport> mp4Report;

    // Fragments: reconstructions delivered since; the counts so far.
    std::vector<FragmentCandidate> fragmentCandidates;
    std::optional<FragmentRecoveryReport> fragmentReport;

    // Evaluation: the scan's results, delivered since, in order; the report
    // with the stage's last update.
    std::vector<evaluation::EvaluatedCandidate> candidates;
    std::optional<evaluation::EvaluationReport> evaluationReport;

    // Source ranges found unreadable since the update before.
    std::vector<storage::BadRegion> unreadable;
    ScanMetrics metrics;
};

// Receives a scan's updates in order, on the scan's thread. An error stops
// the scan, which returns it; the update was not applied to the scan's own
// checkpoint either, so the next one resumes from before it.
using ScanUpdateSink = std::function<Status(const ScanUpdate& update)>;

// The updates of a scan, applied in order: what a scan resumes from. It keeps
// every stage's results, except the evaluated candidates, of which it keeps
// records (EvaluationRecord): the candidates themselves are the caller's.
//
// Thread safety: none; one owner at a time.
class ScanCheckpoint {
public:
    // Applies `update`, all of it or nothing: fails with InvalidInput,
    // leaving the checkpoint as it was, when the update does not follow the
    // ones applied (sequence, stage, identity, parts that do not belong to
    // its stage, volumes or positions that do not fit).
    [[nodiscard]] Status apply(const ScanUpdate& update);

    // No update applied yet.
    [[nodiscard]] bool empty() const noexcept { return sequence_ == 0; }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
    [[nodiscard]] const std::optional<ScanIdentity>& identity() const noexcept { return identity_; }
    // The stage to run (or in progress); Completed once the scan is.
    [[nodiscard]] ScanStage stage() const noexcept { return stage_; }
    [[nodiscard]] bool completed() const noexcept { return stage_ == ScanStage::Completed; }

    [[nodiscard]] const std::optional<partition::PartitionTable>& partitionTable() const noexcept {
        return partitionTable_;
    }
    // Empty until the volume stage's first update.
    [[nodiscard]] const std::vector<VolumeRecord>& volumes() const noexcept { return volumes_; }
    [[nodiscard]] bool volumesPlanned() const noexcept { return volumesPlanned_; }
    [[nodiscard]] const std::vector<Mp4Examination>& mp4Examinations() const noexcept { return mp4Examinations_; }
    [[nodiscard]] const std::vector<FragmentSeedExamination>& seedExaminations() const noexcept {
        return seedExaminations_;
    }
    [[nodiscard]] const std::optional<PassState>& pass() const noexcept { return pass_; }
    [[nodiscard]] const std::vector<carving::FileCandidate>& carves() const noexcept { return carves_; }
    // MP4 recovery's state: the examined candidates (with the carves merged
    // into them) and the pass's carving candidates.
    [[nodiscard]] const Mp4StepsState& mp4State() const noexcept { return mp4State_; }
    [[nodiscard]] const std::vector<FragmentPassEvent>& fragmentEvents() const noexcept { return fragmentEvents_; }
    [[nodiscard]] const std::vector<Mp4Candidate>& mp4Candidates() const noexcept { return mp4Candidates_; }
    [[nodiscard]] const std::optional<Mp4RecoveryReport>& mp4Report() const noexcept { return mp4Report_; }
    [[nodiscard]] const std::vector<FragmentCandidate>& fragmentCandidates() const noexcept {
        return fragmentCandidates_;
    }
    [[nodiscard]] const std::optional<FragmentRecoveryReport>& fragmentReport() const noexcept {
        return fragmentReport_;
    }
    [[nodiscard]] const std::vector<evaluation::EvaluationRecord>& evaluated() const noexcept { return evaluated_; }
    [[nodiscard]] const std::optional<evaluation::EvaluationReport>& evaluationReport() const noexcept {
        return evaluationReport_;
    }
    [[nodiscard]] const storage::BadRegionMap& unreadable() const noexcept { return unreadable_; }
    [[nodiscard]] const ScanMetrics& metrics() const noexcept { return metrics_; }

private:
    [[nodiscard]] Status check(const ScanUpdate& update) const;

    std::uint64_t sequence_ = 0;
    std::optional<ScanIdentity> identity_;
    ScanStage stage_ = ScanStage::Volumes;
    std::optional<partition::PartitionTable> partitionTable_;
    bool volumesPlanned_ = false;
    std::vector<VolumeRecord> volumes_;
    std::vector<Mp4Examination> mp4Examinations_;
    std::vector<FragmentSeedExamination> seedExaminations_;
    std::optional<PassState> pass_;
    std::vector<carving::FileCandidate> carves_;
    Mp4StepsState mp4State_;
    std::vector<FragmentPassEvent> fragmentEvents_;
    std::vector<Mp4Candidate> mp4Candidates_;
    std::optional<Mp4RecoveryReport> mp4Report_;
    std::vector<FragmentCandidate> fragmentCandidates_;
    std::optional<FragmentRecoveryReport> fragmentReport_;
    std::vector<evaluation::EvaluationRecord> evaluated_;
    std::optional<evaluation::EvaluationReport> evaluationReport_;
    storage::BadRegionMap unreadable_;
    ScanMetrics metrics_;
};

}  // namespace recovery::scan
