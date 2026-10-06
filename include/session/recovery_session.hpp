#pragma once

// Recovery sessions (P16): a scan of one source and the recovery jobs that
// write its candidates, kept on disk as they go, so that both survive the
// application: closed, crashed or killed, a session is opened again as it
// was at its last record, and a scan or a job that did not end resumes from
// there instead of starting again.
//
//   <root>/<id>/session.journal          the session (session_journal.hpp)
//   <root>/<id>/session.journal.damaged-<n>   copies of a journal found damaged
//
// A session holds what the plan lists: the source (its information, type and
// fingerprint), the filesystems found (the scan's volumes), the scan's
// configuration, its progress (stage, updates, metrics), the candidates, the
// recovered files (where each went and its reconstruction report), the
// errors, the unreadable regions, the times of every state change and the
// engine versions that created and ran it.
//
// The scan and each recovery job have a state: STARTED, PAUSED, CANCELLED,
// COMPLETED or FAILED (SessionState). Every state but COMPLETED resumes: a
// paused, cancelled or failed one, and a started one whose process ended
// without recording an end (reported as interrupted). Resuming is running
// again: runScan() and runRecovery() go on from the last update recorded.
//
// Every update of the scan (P15's ScanUpdate) and of a job (RecoveryJobUpdate)
// is written to the journal and flushed before the scan or job goes on. A
// crash loses at most the work after the last one, which a resumed run does
// again (nothing an update recorded is done twice). A file a crashed job left
// half written is removed when the job resumes, and written again under the
// same name.
//
// Versions: a session in a newer journal format is refused and left as it
// is; a session another engine version created opens (its candidates and
// recovered files can be read, and recovery jobs run), but an unfinished scan
// resumes only with the engine that started it (L141).
//
// Damage: a torn last record (a crash while writing it) is dropped. Damage
// before intact records: the session opens as it was before the damage, a
// copy of the damaged journal is kept, and the work lost is done again.

#include "carving/format_registry.hpp"
#include "diagnostics/logger.hpp"
#include "evaluation/evaluated_candidate.hpp"
#include "recovery/result.hpp"
#include "scan/recovery_job.hpp"
#include "scan/scan_coordinator.hpp"
#include "scan/scan_state.hpp"
#include "session/session_journal.hpp"
#include "session/session_types.hpp"
#include "storage/bad_region.hpp"
#include "storage/destination_guard.hpp"
#include "storage/storage_source.hpp"
#include "validation/media_validator.hpp"
#include "validation/playability.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::session {

// One state change.
struct StateChange {
    SessionState state = SessionState::Started;
    SessionTime time{};
    // The engine of the run that started or ended.
    std::string engineVersion;
    // Failed: why.
    std::optional<Error> error;
};

struct ScanStatus {
    // None until the scan first runs.
    std::optional<SessionState> state;
    // Started, but not running: its process ended without recording an end.
    bool interrupted = false;
    std::vector<StateChange> history;
    // The stage reached and the metrics (as of the last update).
    scan::ScanStage stage = scan::ScanStage::Volumes;
    scan::ScanMetrics metrics;
    // Updates recorded, and candidates delivered.
    std::uint64_t updates = 0;
    std::uint64_t candidates = 0;
    // Whether runScan() can start or resume it, and why not.
    bool runnable = true;
    std::string notRunnable;
};

struct RecoveryJobStatus {
    std::uint32_t id = 0;
    // Absolute and normalised, UTF-8.
    std::string destination;
    std::vector<evaluation::EvaluatedCandidateId> candidates;
    SessionTime created{};
    std::optional<SessionState> state;
    bool interrupted = false;
    std::vector<StateChange> history;
    // Candidates done: written, or reported as failed.
    std::uint64_t done = 0;
    std::uint64_t recovered = 0;
    std::uint64_t failed = 0;
    scan::RecoveryJobMetrics metrics;
    // Files the job began to write that no update says are done: removed
    // before the job resumes, and written again.
    std::uint64_t filesInProgress = 0;
};

// A volume of the source, as the scan found it.
struct VolumeSummary {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    // Its index in the partition table, if any.
    std::optional<std::uint32_t> partition;
    bool scanned = false;
    std::optional<filesystem::FilesystemType> filesystem;
    std::string label;
    std::uint64_t serialNumber = 0;
    std::uint32_t clusterSize = 0;
    // Files its metadata knows (candidates).
    std::uint64_t files = 0;
    // Why it could not be used.
    std::optional<Error> error;
};

// Damage found in the session's journal, and what was dropped.
struct SessionDamage {
    // When the session found it.
    SessionTime time{};
    DamageRecord damage;
};

struct SessionInfo {
    std::string id;
    std::filesystem::path folder;
    // The journal format, and the engine that created the session.
    std::uint32_t formatVersion = 0;
    std::string engineVersion;
    SessionTime created{};
    // The last record's time.
    SessionTime updated{};
    SessionSource source;
    // The playability checker is not kept: `playability` says whether the
    // scan runs that level (runScan() needs a checker then).
    scan::ScanConfiguration configuration;
    bool playability = false;
    ScanStatus scan;
    std::optional<partition::PartitionScheme> partitionScheme;
    std::vector<VolumeSummary> volumes;
    std::vector<RecoveryJobStatus> jobs;
    std::vector<SessionDamage> damage;
    // Dropped when the session was opened: the bytes of a torn last record,
    // and records of types this engine does not know that it may skip.
    std::uint64_t tornBytesDropped = 0;
    std::uint64_t recordsSkipped = 0;
};

// Something that went wrong, as the session recorded it.
struct SessionError {
    // When it was recorded (the time of its record).
    SessionTime time{};
    // "scan", "volume at <offset>", "recovery job <n>", "recovery job <n>, candidate <id>".
    std::string context;
    Error error;
};

enum class SessionOperation : std::uint8_t {
    None,
    Scan,
    Recovery,
};

[[nodiscard]] std::string_view toString(SessionOperation operation) noexcept;

struct SessionProgress {
    SessionOperation operation = SessionOperation::None;
    // Recovery: the job.
    std::uint32_t job = 0;
    // Paused through the session.
    bool paused = false;
    std::optional<scan::ScanProgress> scan;
    std::optional<scan::RecoveryJobProgress> recovery;
};

struct SessionOptions {
    diagnostics::Logger* logger = nullptr;
    // Tells which physical disk a session folder is on, for the check that it
    // is not the source's (as for a recovery destination). Empty: the platform's.
    storage::DiskResolver diskResolver;
};

// Thread safety: runScan() and runRecovery() run on the calling thread, one
// operation at a time (another call while one runs fails as busy). pause(),
// resume(), cancel(), progress() and every accessor but checkpoint() may be
// called from any thread at any time; the accessors return copies. The
// destructor cancels an operation under way and waits for it to end.
class RecoverySession {
public:
    static constexpr std::string_view kJournalName = "session.journal";

    // Creates a session for `source` (open) in a new folder below `root`
    // (created when needed; it must not be on the source, as for a recovery
    // destination). `configuration.playability` only says whether the scan
    // validates playability; the checker is given to runScan(). Fails with
    // InvalidInput for an invalid configuration or a source that is not open,
    // with DestinationError for a root that cannot be used, and with the
    // source's errors while fingerprinting it.
    [[nodiscard]] static Result<std::unique_ptr<RecoverySession>> create(const std::filesystem::path& root,
                                                                         storage::IStorageSource& source,
                                                                         scan::ScanConfiguration configuration,
                                                                         SessionOptions options = {});
    // Opens the session in `folder`, alone (another process or session that
    // has it open makes this fail). Repairs what a crash or damage left: a
    // torn last record is dropped, damage is cut off after a copy is kept.
    // Fails with InvalidInput when there is no session there, with
    // InvalidFormat for a session this engine cannot read (newer format, no
    // session record), and with DestinationError.
    [[nodiscard]] static Result<std::unique_ptr<RecoverySession>> open(const std::filesystem::path& folder,
                                                                       SessionOptions options = {});

    ~RecoverySession();
    RecoverySession(const RecoverySession&) = delete;
    RecoverySession& operator=(const RecoverySession&) = delete;
    RecoverySession(RecoverySession&&) = delete;
    RecoverySession& operator=(RecoverySession&&) = delete;

    // Runs the scan, or resumes it from its last update. The source must be
    // the session's (same type, path, size, sector size and fingerprint);
    // the formats and validators those of the scan's first run (P15's scan
    // identity); `playability` a checker when the session's scan validates
    // playability, else null. options.control is replaced by the session's
    // own: pause, resume and cancel through the session. Fails with
    // InvalidInput when busy, when the scan is complete or was started by
    // another engine, or for another source; with the scan's errors; and
    // with DestinationError when the journal cannot be written (the scan
    // stops: what was recorded stays).
    [[nodiscard]] Result<scan::ScanSummary> runScan(storage::IStorageSource& source,
                                                    const carving::FormatRegistry& formats,
                                                    const validation::MediaValidatorRegistry& media,
                                                    scan::ScanRunOptions options = {},
                                                    validation::IPlayabilityChecker* playability = nullptr);

    // Adds a recovery job: `candidates` (by id, among those delivered so far;
    // none: all of them) to be written below `destination`. Returns its
    // number. It runs with runRecovery().
    [[nodiscard]] Result<std::uint32_t> addRecoveryJob(const std::filesystem::path& destination,
                                                       std::vector<evaluation::EvaluatedCandidateId> candidates = {});
    // Runs job `job`, or resumes it: the candidates not done yet are written
    // (files an interrupted run left half written are removed first). The
    // source must be the session's. options.control and
    // options.onFileCreated are the session's. Fails with InvalidInput when
    // busy, for an unknown or completed job or another source; with the
    // job's errors; and with DestinationError.
    [[nodiscard]] Result<scan::RecoveryJobSummary> runRecovery(std::uint32_t job, storage::IStorageSource& source,
                                                               scan::RecoveryJobOptions options = {});

    // Pauses the operation under way (recorded as PAUSED), and resumes it
    // (STARTED). InvalidInput when nothing runs.
    [[nodiscard]] Status pause();
    [[nodiscard]] Status resume();
    // Cancels the operation under way; it ends CANCELLED.
    void cancel();

    [[nodiscard]] SessionProgress progress() const;
    [[nodiscard]] SessionInfo info() const;
    [[nodiscard]] const std::string& id() const noexcept;
    [[nodiscard]] const std::filesystem::path& folder() const noexcept;

    // The scan's candidates, in delivery order (id 1 first).
    [[nodiscard]] std::size_t candidateCount() const;
    [[nodiscard]] std::vector<evaluation::EvaluatedCandidate> candidates(
        std::size_t first = 0, std::size_t count = std::numeric_limits<std::size_t>::max()) const;
    [[nodiscard]] std::optional<evaluation::EvaluatedCandidate> candidate(evaluation::EvaluatedCandidateId id) const;
    // The candidates job `job` is done with, by candidate id: the file
    // written (with its reconstruction report) or why not.
    [[nodiscard]] std::vector<scan::RecoveredItem> recoveredItems(std::uint32_t job) const;
    // The source ranges the scan found unreadable, merged.
    [[nodiscard]] std::vector<storage::BadRegion> unreadableRegions() const;
    // Every error recorded: runs that failed, volumes that could not be used,
    // files that could not be written.
    [[nodiscard]] std::vector<SessionError> errors() const;
    // The scan's checkpoint (P15). Only while no operation runs.
    [[nodiscard]] const scan::ScanCheckpoint& checkpoint() const noexcept;

    struct Impl;

private:
    explicit RecoverySession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// What a session folder holds, read without opening the session (a session
// in use can be read; its state is then the one it is running in).
struct SessionSummary {
    std::filesystem::path folder;
    std::string id;
    // Set when the session cannot be read: why (another format, no session).
    std::optional<Error> error;
    std::uint32_t formatVersion = 0;
    std::string engineVersion;
    SessionTime created{};
    SessionTime updated{};
    storage::SourceType sourceType = storage::SourceType::Synthetic;
    std::string sourcePath;
    std::uint64_t sourceSize = 0;
    ScanMode mode = ScanMode::Deep;
    // The scan's last recorded state, and where it was then.
    std::optional<SessionState> state;
    scan::ScanStage stage = scan::ScanStage::Volumes;
    scan::ScanMetrics metrics;
    std::size_t jobs = 0;
};

// Reads the summary of the session in `folder`. Fails with InvalidInput when
// there is no journal there; a journal that cannot be read gives a summary
// with its error.
[[nodiscard]] Result<SessionSummary> readSessionSummary(const std::filesystem::path& folder);
// The sessions in the folders below `root`, by folder name (none when `root`
// does not exist).
[[nodiscard]] Result<std::vector<SessionSummary>> listSessions(const std::filesystem::path& root);

}  // namespace recovery::session
