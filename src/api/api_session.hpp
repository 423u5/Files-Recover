#pragma once

// A session open in the API (P19): the engine's RecoverySession (P16), the
// operation that runs in it on a thread of the API's, and what a user
// interface asks of it, kept ready: every candidate as a list shows it, and
// what recovery jobs did with each, both kept up to date by the session's
// update hooks as the scan and the jobs record their updates.
//
// Locks, in the order they may be taken: opMutex_ (the operation's state),
// then the session's own (pause, resume, cancel record a state change);
// indexMutex_ and previewMutex_ are taken alone. The session's hooks run on
// the operation's thread with no lock of the session held.

#include "api/api_types.hpp"
#include "api_sources.hpp"
#include "carving/format_registry.hpp"
#include "diagnostics/logger.hpp"
#include "event_dispatcher.hpp"
#include "metadata/media_metadata.hpp"
#include "scan/recovery_job.hpp"
#include "scan/scan_coordinator.hpp"
#include "session/recovery_session.hpp"
#include "session/session_types.hpp"
#include "storage/bad_region.hpp"
#include "validation/media_validator.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace recovery::api::detail {

// What every session of an API shares.
struct ApiContext {
    Platform platform;
    std::filesystem::path sessionsRoot;
    // Every format the engine carves and every media validator: the same on
    // every run, so that a resumed scan has the identity it began with.
    carving::FormatRegistry formats;
    validation::MediaValidatorRegistry media;
    diagnostics::Logger* logger = nullptr;
    EventDispatcher* events = nullptr;
    std::chrono::milliseconds progressInterval{250};

    // The OpenSession objects alive: the API waits for none to be left
    // before it goes (a callback or an operation's thread may hold the last
    // reference to one).
    std::mutex liveMutex;
    std::condition_variable liveChanged;
    std::size_t liveSessions = 0;
    // The API is going: no operation starts any more.
    std::atomic<bool> closing{false};
};

// Which candidates a recovery request names.
struct RecoverySelection {
    // Explicit ids; or, when `all`, every candidate that matches `filter`.
    std::vector<CandidateId> ids;
    bool all = false;
    CandidateFilter filter;
};

// Lifetime: shared. The API holds one reference while the session is open,
// each call that uses it holds one, and the thread of an operation holds one
// until it ends; the last one destroys it (on whichever thread drops it: on
// the operation's own thread its std::thread is detached, at its very end).
class OpenSession : public std::enable_shared_from_this<OpenSession> {
public:
    // Creates a session scanning `source` with `configuration` below the
    // sessions folder.
    [[nodiscard]] static Result<std::shared_ptr<OpenSession>> create(ApiContext& context,
                                                                     storage::IStorageSource& source,
                                                                     scan::ScanConfiguration configuration);
    // Opens the session in `folder`.
    [[nodiscard]] static Result<std::shared_ptr<OpenSession>> open(ApiContext& context,
                                                                   const std::filesystem::path& folder);

    // Closes the session (no operation runs: its thread holds a reference).
    ~OpenSession();
    OpenSession(const OpenSession&) = delete;
    OpenSession& operator=(const OpenSession&) = delete;
    OpenSession(OpenSession&&) = delete;
    OpenSession& operator=(OpenSession&&) = delete;

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    // An operation runs (or is starting).
    [[nodiscard]] bool busy() const;
    // Cancels the operation under way, if any (the API is going).
    void cancelAll();
    // Marks the session closed: no operation starts in it any more (a call
    // that found it before it was closed is refused). InvalidInput while an
    // operation runs.
    [[nodiscard]] Status close();
    // Waits for the thread of an operation that ended (it holds a reference
    // to this session until it returns). No-op while one runs.
    void joinFinished();

    // ---- Operations ----

    // Starts the scan (new, or resumed: its source opened by the caller,
    // which checked it is the session's).
    [[nodiscard]] Status startScan(OpenedSource source, std::uint32_t workers);
    // resumeScan(): resumes a paused scan, or runs one that did not complete.
    [[nodiscard]] Status resumeScan(std::uint32_t workers);
    [[nodiscard]] Result<RecoveryStart> recover(const RecoverySelection& selection, const RecoveryOptions& options);
    // resumeRecovery(): resumes a paused recovery, or runs the unfinished jobs.
    [[nodiscard]] Status resumeRecovery(std::uint32_t workers);
    // The operation of `kind` under way.
    [[nodiscard]] Status pause(OperationKind kind);
    [[nodiscard]] Status cancel(OperationKind kind);

    // ---- Queries ----

    [[nodiscard]] Progress progress() const;
    [[nodiscard]] Progress wait(std::chrono::milliseconds timeout) const;
    [[nodiscard]] SessionDetails details() const;
    [[nodiscard]] Result<CandidatePage> candidates(const CandidateQuery& query) const;
    [[nodiscard]] Result<CandidateDetails> candidateDetails(CandidateId candidate, bool readMedia) const;
    [[nodiscard]] Result<std::vector<std::byte>> preview(CandidateId candidate, std::size_t preview,
                                                         std::uint64_t offset, std::uint64_t maxBytes) const;
    [[nodiscard]] Status exportReport(const std::filesystem::path& file, const ReportOptions& options) const;

private:
    enum class Phase : std::uint8_t {
        Idle,
        // Reserved: a request checks what it can and starts the thread.
        Starting,
        Running,
    };

    OpenSession(ApiContext& context, std::string id, recovery::ScanMode mode);

    // Counts the session among the API's live ones while it exists (the
    // first member: it goes last).
    class LiveToken {
    public:
        explicit LiveToken(ApiContext& context);
        ~LiveToken();
        LiveToken(const LiveToken&) = delete;
        LiveToken& operator=(const LiveToken&) = delete;
        LiveToken(LiveToken&&) = delete;
        LiveToken& operator=(LiveToken&&) = delete;

    private:
        ApiContext& context_;
    };

    // The session's options, with the hooks into this object.
    [[nodiscard]] session::SessionOptions sessionOptions();
    // Builds the index from what the session holds (opening it).
    void buildIndex();

    // ---- The session's hooks (operation thread) ----
    void takeScanUpdate(const scan::ScanUpdate& update);
    void takeJobUpdate(std::uint32_t job, const scan::RecoveryJobUpdate& update);
    void addJobToIndex(std::uint32_t job, const std::vector<evaluation::EvaluatedCandidateId>& candidates);

    // ---- Operations ----
    // Reserves the session for an operation of `kind` (InvalidInput when busy).
    [[nodiscard]] Status reserve(OperationKind kind);
    // Gives the reservation back (the request failed before it started).
    void release();
    // Starts the operation's thread; on failure gives the reservation back.
    [[nodiscard]] Status launch(OperationKind kind, std::function<void()> body);
    void runScan(OpenedSource source, std::uint32_t workers);
    void runRecovery(OpenedSource source, std::vector<std::uint32_t> jobs, std::uint32_t workers);
    void onScanProgress(const scan::ScanProgress& progress);
    void onRecoveryProgress(std::size_t index, const scan::RecoveryJobProgress& progress);
    // Applies a pause or a cancellation asked before the engine's operation
    // could take it. Under opMutex_.
    void applyRequestsLocked();
    // The operation ended: records how, wakes waiters, tells the user interface.
    void finish(OperationKind kind, OperationState state, std::optional<Error> error);
    // Opens the session's source and checks it is unchanged.
    [[nodiscard]] Result<OpenedSource> openOwnSource() const;
    void post(EventKind kind, const Progress& progress) const;

    // ---- Queries ----
    [[nodiscard]] Progress idleProgressLocked() const;
    [[nodiscard]] CandidateInfo withRecovery(CandidateInfo info, const std::vector<RecoveryRecord>& records) const;
    [[nodiscard]] Result<metadata::MediaMetadata> mediaOf(const evaluation::EvaluatedCandidate& candidate) const;

    LiveToken live_;
    ApiContext& context_;
    // Set once the session is open, then not changed.
    std::string id_;
    recovery::ScanMode mode_ = recovery::ScanMode::Deep;
    session::SessionSource source_;
    bool playability_ = false;
    std::vector<storage::BadRegion> knownBadRegions_;

    // Every candidate as a list shows it (recovery fields not set), and what
    // each recovery job did with it, by candidate (id - 1). Under indexMutex_.
    mutable std::mutex indexMutex_;
    std::vector<CandidateInfo> index_;
    std::vector<std::vector<RecoveryRecord>> recoveries_;

    // The operation. Under opMutex_.
    mutable std::mutex opMutex_;
    mutable std::condition_variable idle_;
    Phase phase_ = Phase::Idle;
    OperationKind kind_ = OperationKind::None;
    // The user asked to pause (and has not resumed) / to cancel; the pause is
    // in effect in the engine's operation under way.
    bool pauseRequested_ = false;
    bool pauseApplied_ = false;
    bool cancelRequested_ = false;
    // OperationStarted was told: a pause or resume asked before it is told
    // by its state, not by an event of its own.
    bool announced_ = false;
    // closeSession() closed it.
    bool closed_ = false;
    // The latest progress of the operation, or how the last one ended; none
    // before the first.
    std::optional<Progress> last_;
    // While an operation is reserved: last_ before it (restored when the
    // request fails before the operation starts).
    std::optional<Progress> before_;
    // The operation's fraction done, as high as it was told (a stage's total
    // can grow as it is learnt: a progress bar does not go back).
    mutable double highFraction_ = 0.0;
    // One more for each operation (what highFraction_ belongs to).
    std::uint64_t serial_ = 0;
    // Recovery: the operation's jobs, their files, and the job running.
    std::vector<std::uint32_t> jobs_;
    std::vector<std::uint64_t> jobFiles_;
    std::size_t jobIndex_ = 0;
    std::thread thread_;

    // The source for media metadata and previews, opened when first needed,
    // and the metadata read last. Under previewMutex_.
    mutable std::mutex previewMutex_;
    mutable std::optional<OpenedSource> previewSource_;
    mutable std::map<std::uint64_t, metadata::MediaMetadata> mediaCache_;

    // Destroyed first: the hooks above outlive it.
    std::unique_ptr<session::RecoverySession> session_;
};

}  // namespace recovery::api::detail
