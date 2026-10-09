#pragma once

// The GUI-facing API (P19): everything a user interface needs to find a
// source, image it, scan it, list and preview what the scan found, recover
// it and report on it, without any knowledge of filesystems, carving or the
// engine's types (api_types.hpp holds the values it hands out and takes).
//
//   listSources() / inspectSource()           which disks there are; what a source holds
//   createImage()                              a raw image of a disk, unreadable sectors zero-filled
//   startScan()                                a new session scanning a source (Quick or Deep)
//   pauseScan() / resumeScan() / cancelScan()  and a stopped scan resumed where it stopped
//   getProgress()                              what runs and how far it is, at any time
//   getCandidates() / getCandidateDetails() / readPreview()
//   recoverCandidate() / recoverCandidates() / recoverAll()
//   pauseRecovery() / resumeRecovery() / cancelRecovery()
//   listSessions() / openSession() / getSession() / closeSession()
//   exportReport()                             a session's report, as JSON or text
//
// The engine does the work: the user interface never reads the source, never
// writes a recovered file, and never touches a session's journal. The source
// is only ever read; recovered files, images, reports and sessions go
// elsewhere, never onto the source.
//
// Sessions (P16) keep a scan and its recovery jobs on disk as they go, below
// the sessions folder (ApiOptions::sessionsRoot): after the program closes,
// crashes or loses power, listSessions() finds them, openSession() opens one
// as it was at its last record, and resumeScan() / resumeRecovery() go on
// where they stopped. A session is open in one program at a time.
//
// Operations: a scan, a recovery or an imaging runs on a thread of the API's;
// the call that starts it returns at once (after checking what it can: the
// source opens, the destination is safe). Each session runs one operation at
// a time (another request fails with InvalidInput while one runs); several
// sessions and imagings may run at once.
//
// Events: ApiOptions::onEvent is told what happens (operations starting,
// progress, pauses, candidates found, files recovered, operations ending) on
// one thread of the API's, one event at a time, in order, with no lock of the
// API held: it may call any member function (it must not destroy the API).
// Progress events are merged (a slow user interface gets the latest, never a
// backlog), and the scan never waits for the user interface. A user interface
// hands events to its own thread (post, do not send). getProgress() gives the
// same state on request.
//
// Thread safety: every member function may be called from any thread at any
// time, also from the event callback. The destructor cancels the operations
// under way (their sessions record them as cancelled: they resume later),
// waits for them to stop, and closes the sessions; events still queued then
// are not delivered.
//
// Errors are the engine's (recovery/error.hpp): InvalidInput for a request
// that cannot be done now or at all (an unknown session or candidate, a
// session busy, settings out of range), IoError for a source that cannot be
// read, DestinationError for a destination or sessions folder that cannot be
// used (on the source, a file in the way), and so on. Error messages are for
// people.

#include "api/api_types.hpp"
#include "recovery/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::api {

// The platform's parts, replaced in tests (api_platform.hpp).
struct PlatformHooks;

struct ApiOptions {
    // Where sessions are kept; empty: %LOCALAPPDATA%\RecoveryEngine\Sessions.
    // A session cannot be kept on the disk it scans.
    std::filesystem::path sessionsRoot;
    // Told every event (see above). Empty: no events (getProgress() only).
    // The callbacks, and what they refer to, must stay valid until the
    // destructor returns: the API's threads may call them until then.
    EventCallback onEvent;
    // Progress events: at most one per operation per interval.
    std::chrono::milliseconds progressInterval{250};
    // Told the engine's log records at `logLevel` and above, on the thread
    // that logs (any of the engine's): it must be quick, must not throw and
    // must not call the API. Empty: no log.
    LogCallback onLog;
    LogLevel logLevel = LogLevel::Info;
};

class RecoveryApi {
public:
    // The largest piece of a preview readPreview() hands out at once.
    static constexpr std::uint64_t kMaxPreviewRead = 64ull * 1024 * 1024;

    // Fails with InvalidInput for invalid options and with DestinationError
    // when the sessions folder cannot be known (no %LOCALAPPDATA%).
    [[nodiscard]] static Result<std::unique_ptr<RecoveryApi>> create(ApiOptions options = {});

    ~RecoveryApi();
    RecoveryApi(const RecoveryApi&) = delete;
    RecoveryApi& operator=(const RecoveryApi&) = delete;
    RecoveryApi(RecoveryApi&&) = delete;
    RecoveryApi& operator=(RecoveryApi&&) = delete;

    // "RecoveryEngine 0.1.0"
    [[nodiscard]] static std::string engineVersion();
    [[nodiscard]] const std::filesystem::path& sessionsRoot() const noexcept;

    // ---- Sources ----

    // The physical disks attached, by number. Nothing is read from them, and
    // no administrator rights are needed (reading one does need them).
    [[nodiscard]] Result<std::vector<DiskInfo>> listSources() const;
    // What a source is: its size and sectors, an image's metadata, its
    // partitions, and the filesystem of each volume a scan would read. Reads
    // partition tables and boot records only.
    [[nodiscard]] Result<SourceInspection> inspectSource(const SourceRef& source) const;

    // ---- Imaging ----

    // Starts imaging `source` into options.image: the source is read from
    // start to end, unreadable sectors are narrowed down, zero-filled and
    // listed in the image's metadata; a scan of the image treats them as
    // unreadable. Fails at once when the source cannot be opened or the image
    // cannot be written there (it exists and options.resume is not set, it is
    // on the source).
    [[nodiscard]] Result<ImagingId> createImage(const SourceRef& source, const ImagingOptions& options);
    [[nodiscard]] Result<Progress> getImagingProgress(ImagingId imaging) const;
    // Stops it; what was imaged is kept (resume with ImagingOptions::resume).
    [[nodiscard]] Status cancelImaging(ImagingId imaging);
    // Waits until it ends or `timeout` passes; its progress then.
    [[nodiscard]] Result<Progress> waitForImaging(ImagingId imaging, std::chrono::milliseconds timeout) const;

    // ---- Sessions ----

    // The sessions in the sessions folder, by id (oldest first).
    [[nodiscard]] Result<std::vector<SessionListing>> listSessions() const;
    // Opens session `id`, repairing what a crash or damage left (see
    // SessionDetails::repairs). Fails when another program has it open. A
    // session already open here is not opened again.
    [[nodiscard]] Result<SessionDetails> openSession(std::string_view id);
    // Closes it (another program may then open it). InvalidInput while an
    // operation runs.
    [[nodiscard]] Status closeSession(std::string_view id);
    // What the session holds now (an open session).
    [[nodiscard]] Result<SessionDetails> getSession(std::string_view id) const;

    // ---- Scans ----

    // Creates a session for `source` and starts its scan. Returns the
    // session's id (it is open). The regions an image's metadata lists as
    // unreadable are treated as unreadable.
    [[nodiscard]] Result<std::string> startScan(const SourceRef& source, const ScanSettings& settings = {});
    // Pauses the scan under way (the session records it: a paused scan that
    // the program leaves resumes later). The scan stops at its next safe
    // point; no source read is under way while it is paused.
    [[nodiscard]] Status pauseScan(std::string_view session);
    // Resumes a paused scan; or runs again a scan that did not complete
    // (cancelled, failed, interrupted, or paused in an earlier run of the
    // program), from where it stopped, with the settings it began with. The
    // source must be attached and unchanged.
    [[nodiscard]] Status resumeScan(std::string_view session, std::uint32_t workerThreads = 0);
    // Stops the scan under way; what it did is kept (resumeScan() goes on).
    [[nodiscard]] Status cancelScan(std::string_view session);
    // The session's operation now, or how the last one ended.
    [[nodiscard]] Result<Progress> getProgress(std::string_view session) const;
    // Waits until the session's operation ends or `timeout` passes; its
    // progress then.
    [[nodiscard]] Result<Progress> waitForOperation(std::string_view session, std::chrono::milliseconds timeout) const;

    // ---- Candidates ----

    // A page of the candidates delivered so far that match the query (a scan
    // delivers candidates in its last stage).
    [[nodiscard]] Result<CandidatePage> getCandidates(std::string_view session, const CandidateQuery& query = {}) const;
    // Everything about one candidate; with `readMedia`, its media metadata
    // and previews too, read from the source (which must be attached).
    [[nodiscard]] Result<CandidateDetails> getCandidateDetails(std::string_view session, CandidateId candidate,
                                                               bool readMedia = true) const;
    // Bytes of a preview (CandidateDetails::media->previews[preview]): from
    // `offset`, at most `maxBytes` (at most kMaxPreviewRead); a large preview
    // (a video) is read piece by piece. Read from the source, never written
    // anywhere.
    [[nodiscard]] Result<std::vector<std::byte>> readPreview(std::string_view session, CandidateId candidate,
                                                             std::size_t preview, std::uint64_t offset = 0,
                                                             std::uint64_t maxBytes = kMaxPreviewRead) const;

    // ---- Recovery ----

    // Writes candidates to options.destination, as recovery jobs of the
    // session (see RecoveryStart): jobs to that folder that did not end are
    // finished first, then the candidates asked for that no job wrote there.
    // Runs whenever no other operation runs in the session, also when its
    // scan did not complete (the candidates delivered so far). Fails at once
    // for an unknown candidate, a destination on the source or that cannot
    // be used, or a source that is not attached and unchanged.
    [[nodiscard]] Result<RecoveryStart> recoverCandidate(std::string_view session, CandidateId candidate,
                                                         const RecoveryOptions& options);
    [[nodiscard]] Result<RecoveryStart> recoverCandidates(std::string_view session,
                                                          std::span<const CandidateId> candidates,
                                                          const RecoveryOptions& options);
    // Every candidate delivered that matches `filter` (damaged ones and
    // duplicates too, unless the filter leaves them out).
    [[nodiscard]] Result<RecoveryStart> recoverAll(std::string_view session, const RecoveryOptions& options,
                                                   const CandidateFilter& filter = {});
    [[nodiscard]] Status pauseRecovery(std::string_view session);
    // Resumes a paused recovery; or runs the session's recovery jobs that did
    // not end (each to its own folder), where they stopped.
    [[nodiscard]] Status resumeRecovery(std::string_view session, std::uint32_t workerThreads = 0);
    [[nodiscard]] Status cancelRecovery(std::string_view session);

    // ---- Reports ----

    // Writes the session's report to the new file `file` (never overwritten,
    // never on the source): JSON for programs ("recovery-session-report", as
    // `recovery report --format json` writes it) or text for people. Also
    // while an operation runs (the report is as of the call).
    [[nodiscard]] Status exportReport(std::string_view session, const std::filesystem::path& file,
                                      const ReportOptions& options = {}) const;

    struct Impl;

private:
    friend Result<std::unique_ptr<RecoveryApi>> createRecoveryApi(ApiOptions options, PlatformHooks hooks);

    explicit RecoveryApi(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace recovery::api
