#include "session/recovery_session.hpp"

#include "recovery/text.hpp"
#include "recovery/version.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <span>
#include <system_error>
#include <utility>

namespace recovery::session {

namespace {

using diagnostics::field;
using diagnostics::LogLevel;

constexpr std::string_view kComponent = "session";
// Folder names tried for a new session (the id has a random part).
constexpr int kIdAttempts = 16;
// Copies of a damaged journal kept in one folder, at most.
constexpr int kMaxBackups = 1000;
constexpr std::size_t kCopyChunk = 1 * kMiB;

Error refused(std::string message) {
    return makeError(ErrorCode::InvalidInput, "session: " + std::move(message));
}

Error unreadable(std::string message) {
    return makeError(ErrorCode::InvalidFormat, "session: " + std::move(message));
}

// "20261006-101530-3fa94c2e": the time the session was created (UTC), and 32
// random bits.
std::string makeId(SessionTime now) {
    const std::string stamp = formatUtcTimestamp(now);  // "2026-10-06T10:15:30.125Z"
    std::string id = stamp.substr(0, 4) + stamp.substr(5, 2) + stamp.substr(8, 2) + "-" + stamp.substr(11, 2) +
                     stamp.substr(14, 2) + stamp.substr(17, 2) + "-";
    std::random_device random;
    std::uint32_t tag = random();
    std::string hex(8, '0');
    for (std::size_t i = hex.size(); i-- > 0;) {
        hex[i] = "0123456789abcdef"[tag & 0xF];
        tag >>= 4;
    }
    return id + hex;
}

Result<std::filesystem::path> fromUtf8(const std::string& text) {
    try {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    } catch (const std::exception&) {
        return unreadable("a path that is not UTF-8");
    }
}

// The \\?\ form of an absolute path, so that files of deep trees can be
// removed (RecoveryWriter writes them so).
std::filesystem::path withLongPathPrefix(const std::filesystem::path& absolute) {
    const std::wstring& text = absolute.native();
    if (text.starts_with(L"\\\\?\\")) {
        return absolute;
    }
    if (text.size() >= 3 && text[1] == L':' && text[2] == L'\\') {
        return std::filesystem::path(L"\\\\?\\" + text);
    }
    if (text.starts_with(L"\\\\") && text.size() > 2 && text[2] != L'.' && text[2] != L'?') {
        return std::filesystem::path(L"\\\\?\\UNC\\" + text.substr(2));
    }
    return absolute;
}

std::optional<SessionState> lastState(const std::vector<StateChange>& history) {
    return history.empty() ? std::nullopt : std::optional<SessionState>(history.back().state);
}

// What differs between the identity a scan began with and the one a run
// would have, in words; empty when nothing does.
std::string differences(const scan::ScanIdentity& began, const scan::ScanIdentity& now) {
    std::vector<std::string> parts;
    if (began.checkpointVersion != now.checkpointVersion) {
        parts.emplace_back("the checkpoint format");
    }
    if (began.engineVersion != now.engineVersion) {
        parts.emplace_back("the engine");
    }
    if (began.sourceType != now.sourceType || began.sourcePath != now.sourcePath ||
        began.sourceSize != now.sourceSize || began.sectorSize != now.sectorSize) {
        parts.emplace_back("the source");
    }
    if (began.mode != now.mode || began.configuration != now.configuration) {
        parts.emplace_back("the settings");
    }
    if (began.formats != now.formats) {
        parts.emplace_back("the carving formats");
    }
    if (began.mediaValidators != now.mediaValidators) {
        parts.emplace_back("the media validators");
    }
    std::string text;
    for (const std::string& part : parts) {
        text += (text.empty() ? "" : ", ") + part;
    }
    return text;
}

bool knownType(std::uint16_t type) noexcept {
    return type >= static_cast<std::uint16_t>(RecordType::SessionCreated) &&
           type <= static_cast<std::uint16_t>(RecordType::Damage);
}

}  // namespace

std::string_view toString(SessionOperation operation) noexcept {
    switch (operation) {
    case SessionOperation::None:
        return "none";
    case SessionOperation::Scan:
        return "scan";
    case SessionOperation::Recovery:
        return "recovery";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// The session's state
// ---------------------------------------------------------------------------

struct RecoverySession::Impl {
    struct Job {
        JobCreatedRecord created;
        std::set<std::uint64_t> members;
        SessionTime time{};
        std::vector<StateChange> history;
        scan::RecoveryJobCheckpoint checkpoint;
        // When each candidate was reported done.
        std::map<std::uint64_t, SessionTime> doneAt;
        // Files begun that no update says are done: candidate -> path (UTF-8).
        std::map<std::uint64_t, std::string> started;
    };

    struct ReplayEnd {
        JournalEnd end = JournalEnd::Clean;
        std::uint64_t validEnd = 0;
        std::string reason;
    };

    std::filesystem::path folder;
    SessionOptions options;
    std::unique_ptr<JournalFile> journal;

    // Set once, by the session's first record.
    bool hasCreated = false;
    SessionCreatedRecord created;
    SessionTime createdTime{};
    std::uint32_t formatVersion = kJournalFormatVersion;

    // What the records say, under `mutex`: written by the operation's thread
    // (and the job's workers for the files they begin), read by any.
    mutable std::mutex mutex;
    SessionTime updated{};
    scan::ScanCheckpoint checkpoint;
    std::vector<evaluation::EvaluatedCandidate> candidates;
    std::vector<StateChange> scanHistory;
    std::map<std::size_t, SessionTime> volumeTimes;
    std::vector<Job> jobs;
    std::vector<SessionDamage> damage;
    std::uint64_t tornBytes = 0;
    std::uint64_t skipped = 0;

    // The operation under way, under `controlMutex`.
    mutable std::mutex controlMutex;
    std::condition_variable idle;
    SessionOperation operation = SessionOperation::None;
    std::uint32_t operationJob = 0;
    JobControl control;
    bool paused = false;
    // STARTED is recorded: pauses are recorded from now on (one asked for
    // before is recorded right after STARTED).
    bool started = false;
    const scan::ScanCoordinator* liveScan = nullptr;
    const scan::RecoveryJob* liveJob = nullptr;
    // The same, readable without a lock (to tell a running state from an
    // interrupted one).
    std::atomic<SessionOperation> running{SessionOperation::None};
    std::atomic<std::uint32_t> runningJob{0};

    void log(LogLevel level, std::string_view message, std::initializer_list<diagnostics::LogField> fields = {}) const {
        if (options.logger != nullptr) {
            options.logger->log(level, kComponent, message, fields);
        }
    }

    Job* findJob(std::uint32_t id) {
        return id == 0 || id > jobs.size() ? nullptr : &jobs[id - 1];
    }
    const Job* findJob(std::uint32_t id) const {
        return id == 0 || id > jobs.size() ? nullptr : &jobs[id - 1];
    }

    // Why the scan cannot run with this engine; empty when it can. Under `mutex`.
    std::string scanNotRunnable() const {
        if (checkpoint.completed()) {
            return "the scan is complete";
        }
        if (checkpoint.identity().has_value() && checkpoint.identity()->engineVersion != kEngineVersion) {
            return "the scan was started by engine " + checkpoint.identity()->engineVersion + "; engine " +
                   std::string(kEngineVersion) +
                   " does not resume it (the results of two engines would mix): start a new session";
        }
        return {};
    }

    // ---- Applying records (replayed, or live under `mutex`) ----

    Status apply(RecordPayload&& payload, SessionTime time);
    Status applyScanUpdate(const scan::ScanUpdate& update, SessionTime time);
    Status applyJobUpdate(std::uint32_t id, const scan::RecoveryJobUpdate& update, SessionTime time);
    Result<ReplayEnd> replay(JournalReader& reader);

    // ---- Writing records ----

    // Writes a small record, then applies it. A record the session cannot
    // apply after writing it breaks the journal (see JournalFile::markBroken).
    Status record(RecordPayload payload);
    ScanStateRecord scanState(SessionState state, std::optional<Error> error) const;
    JobStateRecord jobState(std::uint32_t id, SessionState state, std::optional<Error> error) const;

    // The scan's sink, a job's sink, and the hook for the files a job begins.
    Status takeScanUpdate(const scan::ScanUpdate& update);
    Status takeJobUpdate(std::uint32_t id, const scan::RecoveryJobUpdate& update);
    Status fileStarted(std::uint32_t id, evaluation::EvaluatedCandidateId candidate,
                       const std::filesystem::path& path);

    // ---- Operations ----

    Result<JobControl> begin(SessionOperation kind, std::uint32_t job);
    // Records the operation as STARTED (and PAUSED after it, when a pause
    // came while it was starting).
    Status markStarted();
    // The state record of the operation under way. Under `controlMutex`.
    RecordPayload operationState(SessionState state) const;
    void setLive(const scan::ScanCoordinator* scan, const scan::RecoveryJob* job);
    // Records `end` (when given) and makes the session idle. The last thing
    // an operation does with the session.
    void finish(std::optional<RecordPayload> end);

    Status removeLeftover(const std::string& path, std::uint64_t expectedSize) const;
    Result<std::string> backupJournal() const;
};

Status RecoverySession::Impl::apply(RecordPayload&& payload, SessionTime time) {
    if (auto* session = std::get_if<SessionCreatedRecord>(&payload)) {
        if (hasCreated) {
            return unreadable("a second session record");
        }
        created = std::move(*session);
        createdTime = time;
        hasCreated = true;
        return success();
    }
    if (!hasCreated) {
        return unreadable("a record before the session's");
    }
    if (auto* state = std::get_if<ScanStateRecord>(&payload)) {
        scanHistory.push_back(
            StateChange{state->state, time, std::move(state->engineVersion), std::move(state->error)});
        return success();
    }
    if (auto* update = std::get_if<scan::ScanUpdate>(&payload)) {
        if (Status applied = checkpoint.apply(*update); !applied.ok()) {
            return applied;
        }
        for (const auto& [index, volume] : update->volumes) {
            volumeTimes[index] = time;
        }
        candidates.insert(candidates.end(), std::make_move_iterator(update->candidates.begin()),
                          std::make_move_iterator(update->candidates.end()));
        return success();
    }
    if (auto* added = std::get_if<JobCreatedRecord>(&payload)) {
        const std::string name = "recovery job " + std::to_string(added->job);
        if (added->job != jobs.size() + 1) {
            return unreadable(name + " out of order");
        }
        if (added->destination.empty() || added->candidates.empty()) {
            return unreadable(name + " without a destination or candidates");
        }
        Job job;
        for (const evaluation::EvaluatedCandidateId id : added->candidates) {
            if (id.value() == 0 || id.value() > candidates.size() || !job.members.insert(id.value()).second) {
                return unreadable(name + " names candidates the session does not have, or one twice");
            }
        }
        job.created = std::move(*added);
        job.time = time;
        jobs.push_back(std::move(job));
        return success();
    }
    if (auto* state = std::get_if<JobStateRecord>(&payload)) {
        Job* job = findJob(state->job);
        if (job == nullptr) {
            return unreadable("the state of recovery job " + std::to_string(state->job) + ", which does not exist");
        }
        job->history.push_back(
            StateChange{state->state, time, std::move(state->engineVersion), std::move(state->error)});
        return success();
    }
    if (auto* update = std::get_if<JobUpdateRecord>(&payload)) {
        return applyJobUpdate(update->job, update->update, time);
    }
    if (auto* begun = std::get_if<FileStartedRecord>(&payload)) {
        Job* job = findJob(begun->job);
        if (job == nullptr || !job->members.contains(begun->candidate.value())) {
            return unreadable("a file begun for a candidate of no recovery job");
        }
        job->started[begun->candidate.value()] = std::move(begun->path);
        return success();
    }
    if (auto* found = std::get_if<DamageRecord>(&payload)) {
        damage.push_back(SessionDamage{time, std::move(*found)});
        return success();
    }
    return unreadable("a record of no known kind");
}

Status RecoverySession::Impl::applyScanUpdate(const scan::ScanUpdate& update, SessionTime time) {
    if (Status applied = checkpoint.apply(update); !applied.ok()) {
        return applied;
    }
    for (const auto& [index, volume] : update.volumes) {
        volumeTimes[index] = time;
    }
    candidates.insert(candidates.end(), update.candidates.begin(), update.candidates.end());
    return success();
}

Status RecoverySession::Impl::applyJobUpdate(std::uint32_t id, const scan::RecoveryJobUpdate& update,
                                             SessionTime time) {
    Job* job = findJob(id);
    if (job == nullptr) {
        return unreadable("an update of recovery job " + std::to_string(id) + ", which does not exist");
    }
    for (const scan::RecoveredItem& item : update.items) {
        if (!job->members.contains(item.candidate.value())) {
            return unreadable("recovery job " + std::to_string(id) + " reports a candidate it was not given");
        }
    }
    if (Status applied = job->checkpoint.apply(update); !applied.ok()) {
        return applied;
    }
    for (const scan::RecoveredItem& item : update.items) {
        job->started.erase(item.candidate.value());
        job->doneAt[item.candidate.value()] = time;
    }
    return success();
}

Result<RecoverySession::Impl::ReplayEnd> RecoverySession::Impl::replay(JournalReader& reader) {
    for (;;) {
        Result<std::optional<JournalRecord>> next = reader.next();
        if (!next.ok()) {
            return next.error();
        }
        if (!next->has_value()) {
            break;
        }
        JournalRecord& record = **next;
        if (!hasCreated && record.type != static_cast<std::uint16_t>(RecordType::SessionCreated)) {
            return unreadable("the journal does not begin with its session's record");
        }
        if (!knownType(record.type)) {
            if (record.optional()) {
                ++skipped;
                continue;
            }
            return unreadable("the journal holds a record of type " + std::to_string(record.type) +
                              ", which this engine does not know: it is of a newer session format");
        }
        Result<RecordPayload> payload = decodePayload(static_cast<RecordType>(record.type), record.payload);
        const Status applied = payload.ok() ? apply(std::move(*payload), record.time) : Status{payload.error()};
        if (!applied.ok()) {
            if (!hasCreated) {
                return unreadable("the session's own record does not decode: " + applied.error().message);
            }
            const std::string kind(toString(static_cast<RecordType>(record.type)));
            return ReplayEnd{JournalEnd::Damaged, record.offset,
                             "a '" + kind + "' record at offset " + std::to_string(record.offset) +
                                 " that cannot be used: " + applied.error().message};
        }
        updated = record.time;
    }
    if (!hasCreated) {
        return unreadable("the journal holds no session record (its creation did not finish)");
    }
    return ReplayEnd{reader.end(), reader.validEnd(), reader.endReason()};
}

Status RecoverySession::Impl::record(RecordPayload payload) {
    const SessionTime time = sessionNow();
    if (Status appended = journal->append(payload, time); !appended.ok()) {
        return appended;
    }
    const std::lock_guard lock(mutex);
    if (Status applied = apply(std::move(payload), time); !applied.ok()) {
        journal->markBroken(applied.error());
        return makeError(ErrorCode::InternalError, "session: a record written could not be applied: " +
                                                       applied.error().message);
    }
    updated = time;
    return success();
}

ScanStateRecord RecoverySession::Impl::scanState(SessionState state, std::optional<Error> error) const {
    ScanStateRecord record;
    record.state = state;
    record.engineVersion = std::string(kEngineVersion);
    record.error = std::move(error);
    const std::lock_guard lock(mutex);
    if (!checkpoint.empty()) {
        record.stage = checkpoint.stage();
        record.metrics = checkpoint.metrics();
    }
    return record;
}

JobStateRecord RecoverySession::Impl::jobState(std::uint32_t id, SessionState state,
                                               std::optional<Error> error) const {
    JobStateRecord record;
    record.job = id;
    record.state = state;
    record.engineVersion = std::string(kEngineVersion);
    record.error = std::move(error);
    const std::lock_guard lock(mutex);
    if (const Job* job = findJob(id); job != nullptr) {
        record.metrics = job->checkpoint.metrics();
    }
    return record;
}

Status RecoverySession::Impl::takeScanUpdate(const scan::ScanUpdate& update) {
    const SessionTime time = sessionNow();
    if (Status appended = journal->append(RecordType::ScanUpdate, 0, time, encodePayload(update)); !appended.ok()) {
        return appended;
    }
    {
        const std::lock_guard lock(mutex);
        if (Status applied = applyScanUpdate(update, time); !applied.ok()) {
            journal->markBroken(applied.error());
            return makeError(ErrorCode::InternalError,
                             "session: the scan's update " + std::to_string(update.sequence) +
                                 " was written but could not be applied: " + applied.error().message);
        }
        updated = time;
    }
    if (options.onScanUpdate) {
        options.onScanUpdate(update);
    }
    return success();
}

Status RecoverySession::Impl::takeJobUpdate(std::uint32_t id, const scan::RecoveryJobUpdate& update) {
    const SessionTime time = sessionNow();
    JobUpdateRecord record;
    record.job = id;
    record.update = update;
    if (Status appended = journal->append(RecordType::JobUpdate, 0, time, encodePayload(record)); !appended.ok()) {
        return appended;
    }
    {
        const std::lock_guard lock(mutex);
        if (Status applied = applyJobUpdate(id, update, time); !applied.ok()) {
            journal->markBroken(applied.error());
            return makeError(ErrorCode::InternalError,
                             "session: recovery job " + std::to_string(id) + "'s update " +
                                 std::to_string(update.sequence) + " was written but could not be applied: " +
                                 applied.error().message);
        }
        updated = time;
    }
    if (options.onJobUpdate) {
        options.onJobUpdate(id, update);
    }
    return success();
}

Status RecoverySession::Impl::fileStarted(std::uint32_t id, evaluation::EvaluatedCandidateId candidate,
                                          const std::filesystem::path& path) {
    const SessionTime time = sessionNow();
    FileStartedRecord record;
    record.job = id;
    record.candidate = candidate;
    record.path = toUtf8(path);
    // Flushed before the file gets any data: a crash from here on leaves a
    // record of the file, which the job's resumption removes.
    if (Status appended = journal->append(RecordType::FileStarted, 0, time, encodePayload(record));
        !appended.ok()) {
        return appended;
    }
    const std::lock_guard lock(mutex);
    if (Job* job = findJob(id); job != nullptr) {
        job->started[candidate.value()] = std::move(record.path);
    }
    updated = time;
    return success();
}

Result<JobControl> RecoverySession::Impl::begin(SessionOperation kind, std::uint32_t job) {
    const std::lock_guard lock(controlMutex);
    if (operation != SessionOperation::None) {
        return refused("busy: a " + std::string(toString(operation)) + " is running");
    }
    if (journal->broken()) {
        return makeError(ErrorCode::DestinationError,
                         "session: its journal could not be written; open the session again to go on");
    }
    operation = kind;
    operationJob = job;
    control = JobControl{};
    paused = false;
    started = false;
    running.store(kind);
    runningJob.store(job);
    return control;
}

RecordPayload RecoverySession::Impl::operationState(SessionState state) const {
    return operation == SessionOperation::Scan ? RecordPayload{scanState(state, std::nullopt)}
                                               : RecordPayload{jobState(operationJob, state, std::nullopt)};
}

Status RecoverySession::Impl::markStarted() {
    const std::lock_guard lock(controlMutex);
    if (Status recorded = record(operationState(SessionState::Started)); !recorded.ok()) {
        return recorded;
    }
    started = true;
    if (paused) {
        return record(operationState(SessionState::Paused));
    }
    return success();
}

void RecoverySession::Impl::setLive(const scan::ScanCoordinator* scan, const scan::RecoveryJob* job) {
    const std::lock_guard lock(controlMutex);
    liveScan = scan;
    liveJob = job;
}

void RecoverySession::Impl::finish(std::optional<RecordPayload> end) {
    const std::lock_guard lock(controlMutex);
    if (end.has_value()) {
        if (Status recorded = record(std::move(*end)); !recorded.ok()) {
            log(LogLevel::Error, "the end of an operation could not be recorded",
                {field("error", describe(recorded.error()))});
        }
    }
    operation = SessionOperation::None;
    operationJob = 0;
    paused = false;
    started = false;
    liveScan = nullptr;
    liveJob = nullptr;
    running.store(SessionOperation::None);
    runningJob.store(0);
    idle.notify_all();
}

Status RecoverySession::Impl::removeLeftover(const std::string& path, std::uint64_t expectedSize) const {
    Result<std::filesystem::path> file = fromUtf8(path);
    if (!file.ok()) {
        return file.error();
    }
    const std::filesystem::path io = withLongPathPrefix(*file);
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(io, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return success();
    }
    if (ec) {
        return makeError(ErrorCode::DestinationError, "session: cannot look at '" + path + "'",
                         static_cast<std::uint32_t>(ec.value()));
    }
    // Only what the job can have left: a file no larger than the one it writes.
    if (status.type() != std::filesystem::file_type::regular) {
        log(LogLevel::Warning, "left alone: what holds the name of an interrupted file is not a file",
            {field("path", path)});
        return success();
    }
    const std::uintmax_t size = std::filesystem::file_size(io, ec);
    if (ec) {
        return makeError(ErrorCode::DestinationError, "session: cannot read the size of '" + path + "'",
                         static_cast<std::uint32_t>(ec.value()));
    }
    if (size > expectedSize) {
        log(LogLevel::Warning, "left alone: the file holding the name of an interrupted file is larger than it",
            {field("path", path), field("size", size), field("expected_size", expectedSize)});
        return success();
    }
    const bool removed = std::filesystem::remove(io, ec);
    if (ec) {
        return makeError(ErrorCode::DestinationError,
                         "session: cannot remove '" + path + "', which an interrupted recovery job left incomplete",
                         static_cast<std::uint32_t>(ec.value()));
    }
    if (removed) {
        log(LogLevel::Info, "removed a file an interrupted recovery job left incomplete", {field("path", path)});
    }
    return success();
}

Result<std::string> RecoverySession::Impl::backupJournal() const {
    const std::filesystem::path journalPath = folder / RecoverySession::kJournalName;
    for (int n = 1; n <= kMaxBackups; ++n) {
        const std::string name = std::string(RecoverySession::kJournalName) + ".damaged-" + std::to_string(n);
        const std::filesystem::path target = folder / name;
        std::error_code ec;
        if (std::filesystem::exists(target, ec) || ec) {
            continue;
        }
        Result<storage::DestinationFile> out =
            storage::DestinationFile::open(target, storage::DestinationFile::OpenMode::CreateNew);
        if (!out.ok()) {
            return out.error();
        }
        const auto discard = [&](Error error) -> Error {
            out->close();
            std::filesystem::remove(target, ec);
            return error;
        };
        std::ifstream in(journalPath, std::ios::binary);
        if (!in.is_open()) {
            return discard(makeError(ErrorCode::DestinationError, "session: cannot read the damaged journal"));
        }
        std::vector<char> buffer(kCopyChunk);
        std::uint64_t offset = 0;
        for (;;) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = in.gcount();
            if (got <= 0) {
                break;
            }
            const auto bytes = std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(got)));
            if (Status written = out->writeAt(offset, bytes); !written.ok()) {
                return discard(written.error());
            }
            offset += static_cast<std::uint64_t>(got);
        }
        if (in.bad()) {
            return discard(makeError(ErrorCode::DestinationError, "session: cannot read the damaged journal"));
        }
        if (Status flushed = out->flush(); !flushed.ok()) {
            return discard(flushed.error());
        }
        return name;
    }
    return makeError(ErrorCode::DestinationError, "session: no free name for a copy of the damaged journal");
}

// ---------------------------------------------------------------------------
// Creating and opening
// ---------------------------------------------------------------------------

RecoverySession::RecoverySession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RecoverySession::~RecoverySession() {
    if (!impl_) {
        return;
    }
    std::unique_lock lock(impl_->controlMutex);
    if (impl_->operation != SessionOperation::None) {
        impl_->control.requestCancellation();
        impl_->idle.wait(lock, [&] { return impl_->operation == SessionOperation::None; });
    }
}

Result<std::unique_ptr<RecoverySession>> RecoverySession::create(const std::filesystem::path& root,
                                                                 storage::IStorageSource& source,
                                                                 scan::ScanConfiguration configuration,
                                                                 SessionOptions options) {
    if (!source.isOpen()) {
        return refused("the source is not open");
    }
    if (Status valid = scan::validate(configuration); !valid.ok()) {
        return valid.error();
    }
    if (root.empty()) {
        return refused("no folder for the session");
    }
    if (!options.diskResolver) {
        options.diskResolver = storage::makePlatformDiskResolver();
    }
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(root, ec).lexically_normal();
    if (ec) {
        return refused("the folder '" + toUtf8(root) + "' cannot be made absolute");
    }
    // The folder must not be on the source: checked before anything is
    // created, and again once it exists.
    const storage::SourceInfo info = source.getInfo();
    if (Status safe = storage::checkDestinationSafety(info, absolute, options.diskResolver); !safe.ok()) {
        return safe.error();
    }
    std::filesystem::create_directories(absolute, ec);
    if (ec) {
        return makeError(ErrorCode::DestinationError, "session: cannot create '" + toUtf8(absolute) + "'",
                         static_cast<std::uint32_t>(ec.value()));
    }
    if (Status safe = storage::checkDestinationSafety(info, absolute, options.diskResolver); !safe.ok()) {
        return safe.error();
    }
    Result<SessionSource> described = describeSource(source);
    if (!described.ok()) {
        return described.error();
    }

    std::string id;
    std::filesystem::path folder;
    for (int attempt = 0;; ++attempt) {
        if (attempt == kIdAttempts) {
            return makeError(ErrorCode::DestinationError,
                             "session: no free folder name for a session in '" + toUtf8(absolute) + "'");
        }
        id = makeId(sessionNow());
        folder = absolute / id;
        if (std::filesystem::create_directory(folder, ec) && !ec) {
            break;
        }
        if (ec) {
            return makeError(ErrorCode::DestinationError, "session: cannot create '" + toUtf8(folder) + "'",
                             static_cast<std::uint32_t>(ec.value()));
        }
    }
    const auto abandon = [&](Error error) -> Error {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        return error;
    };
    Result<std::unique_ptr<JournalFile>> journal = JournalFile::create(folder / kJournalName);
    if (!journal.ok()) {
        return abandon(journal.error());
    }

    auto impl = std::make_unique<Impl>();
    impl->folder = folder;
    impl->options = std::move(options);
    impl->journal = std::move(journal).value();
    SessionCreatedRecord record;
    record.id = id;
    record.engineVersion = std::string(kEngineVersion);
    record.source = std::move(described).value();
    record.playability = configuration.playability != nullptr;
    configuration.playability = nullptr;
    record.configuration = std::move(configuration);
    const SessionTime time = sessionNow();
    if (Status appended = impl->journal->append(RecordPayload{record}, time); !appended.ok()) {
        impl->journal.reset();
        return abandon(appended.error());
    }
    (void)impl->apply(RecordPayload{std::move(record)}, time);
    impl->updated = time;
    impl->log(LogLevel::Info, "session created",
              {field("id", id), field("folder", toUtf8(folder)), field("source", impl->created.source.path)});
    return std::unique_ptr<RecoverySession>(new RecoverySession(std::move(impl)));
}

Result<std::unique_ptr<RecoverySession>> RecoverySession::open(const std::filesystem::path& folder,
                                                               SessionOptions options) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(folder, ec).lexically_normal();
    if (ec) {
        return refused("the folder '" + toUtf8(folder) + "' cannot be made absolute");
    }
    const std::filesystem::path path = absolute / kJournalName;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return refused("there is no session in '" + toUtf8(absolute) + "'");
    }
    // The journal is taken first: nothing else writes it while it is read.
    Result<std::unique_ptr<JournalFile>> journal = JournalFile::open(path);
    if (!journal.ok()) {
        return journal.error();
    }
    Result<JournalReader> reader = JournalReader::open(path);
    if (!reader.ok()) {
        return reader.error();
    }
    auto impl = std::make_unique<Impl>();
    impl->folder = absolute;
    impl->options = std::move(options);
    impl->journal = std::move(journal).value();
    impl->formatVersion = reader->formatVersion();
    Result<Impl::ReplayEnd> replayed = impl->replay(*reader);
    if (!replayed.ok()) {
        return replayed.error();
    }

    // What a crash or damage left. Nothing is changed before this point.
    switch (replayed->end) {
    case JournalEnd::Clean:
        break;
    case JournalEnd::TornTail: {
        impl->tornBytes = reader->fileSize() - replayed->validEnd;
        if (Status cut = impl->journal->truncate(replayed->validEnd); !cut.ok()) {
            return cut.error();
        }
        impl->log(LogLevel::Info, "dropped a record a crash left incomplete",
                  {field("offset", replayed->validEnd), field("bytes", impl->tornBytes),
                   field("reason", replayed->reason)});
        break;
    }
    case JournalEnd::Damaged: {
        Result<std::uint64_t> lost = reader->countIntactRecords(replayed->validEnd);
        if (!lost.ok()) {
            return lost.error();
        }
        Result<std::string> backup = impl->backupJournal();
        if (!backup.ok()) {
            return backup.error();
        }
        DamageRecord damage;
        damage.offset = replayed->validEnd;
        damage.bytesDropped = reader->fileSize() - replayed->validEnd;
        damage.recordsDropped = *lost;
        damage.reason = replayed->reason;
        damage.backup = *backup;
        if (Status cut = impl->journal->truncate(replayed->validEnd); !cut.ok()) {
            return cut.error();
        }
        impl->log(LogLevel::Warning, "the journal was damaged: the session goes on from before the damage",
                  {field("offset", damage.offset), field("bytes", damage.bytesDropped),
                   field("records", damage.recordsDropped), field("reason", damage.reason),
                   field("backup", damage.backup)});
        if (Status recorded = impl->record(RecordPayload{std::move(damage)}); !recorded.ok()) {
            return recorded.error();
        }
        break;
    }
    }
    // A crash between a run's last update and its end: the run is complete,
    // and its end is recorded now.
    if (impl->checkpoint.completed() && lastState(impl->scanHistory) != SessionState::Completed) {
        if (Status recorded = impl->record(RecordPayload{impl->scanState(SessionState::Completed, std::nullopt)});
            !recorded.ok()) {
            return recorded.error();
        }
    }
    for (const Impl::Job& job : impl->jobs) {
        if (job.checkpoint.complete() && lastState(job.history) != SessionState::Completed) {
            const std::uint32_t id = job.created.job;
            if (Status recorded =
                    impl->record(RecordPayload{impl->jobState(id, SessionState::Completed, std::nullopt)});
                !recorded.ok()) {
                return recorded.error();
            }
        }
    }
    impl->log(LogLevel::Info, "session opened",
              {field("id", impl->created.id), field("folder", toUtf8(absolute)),
               field("updates", impl->checkpoint.sequence()), field("candidates", impl->candidates.size())});
    return std::unique_ptr<RecoverySession>(new RecoverySession(std::move(impl)));
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

Result<scan::ScanSummary> RecoverySession::runScan(storage::IStorageSource& source,
                                                   const carving::FormatRegistry& formats,
                                                   const validation::MediaValidatorRegistry& media,
                                                   scan::ScanRunOptions options,
                                                   validation::IPlayabilityChecker* playability) {
    Impl& s = *impl_;
    Result<JobControl> control = s.begin(SessionOperation::Scan, 0);
    if (!control.ok()) {
        return control.error();
    }
    {
        std::string why;
        {
            const std::lock_guard lock(s.mutex);
            why = s.scanNotRunnable();
        }
        if (why.empty() && s.created.playability != (playability != nullptr)) {
            why = s.created.playability ? "the session's scan validates playability: runScan() needs a checker"
                                        : "the session's scan does not validate playability: runScan() takes no "
                                          "checker";
        }
        if (!why.empty()) {
            s.finish(std::nullopt);
            return refused(std::move(why));
        }
    }
    if (Status same = checkSameSource(s.created.source, source, control->token()); !same.ok()) {
        s.finish(std::nullopt);
        return same.error();
    }
    scan::ScanConfiguration configuration = s.created.configuration;
    configuration.playability = playability;
    options.control = *control;
    Result<scan::ScanSummary> result = makeError(ErrorCode::InternalError, "session: the scan did not run");
    {
        scan::ScanCoordinator coordinator(source, formats, media, std::move(configuration), std::move(options));
        // A scan goes on only with what it began with (P15's identity): a run
        // with other formats or validators is refused before anything is
        // recorded, not run and recorded as failed.
        std::string different;
        {
            const std::lock_guard lock(s.mutex);
            if (s.checkpoint.identity().has_value()) {
                different = differences(*s.checkpoint.identity(), coordinator.identity());
            }
        }
        if (!different.empty()) {
            s.finish(std::nullopt);
            return refused("the scan goes on only with what it began with: " + different + " differ");
        }
        if (Status started = s.markStarted(); !started.ok()) {
            s.finish(std::nullopt);
            return started.error();
        }
        s.log(LogLevel::Info, "scan started",
              {field("id", s.created.id), field("updates", s.checkpoint.sequence())});
        s.setLive(&coordinator, nullptr);
        const scan::ScanUpdateSink sink = [&s](const scan::ScanUpdate& update) { return s.takeScanUpdate(update); };
        // The coordinator copies the checkpoint before it hands out an update.
        result = coordinator.run(sink, &s.checkpoint);
        s.setLive(nullptr, nullptr);
    }

    SessionState end = SessionState::Failed;
    std::optional<Error> error;
    if (result.ok()) {
        end = result->outcome == scan::ScanOutcome::Completed ? SessionState::Completed : SessionState::Cancelled;
    } else if (result.error().code == ErrorCode::Cancelled) {
        end = SessionState::Cancelled;
    } else {
        error = result.error();
    }
    s.log(end == SessionState::Failed ? LogLevel::Error : LogLevel::Info, "scan ended",
          {field("state", toString(end)), field("error", error.has_value() ? describe(*error) : std::string())});
    s.finish(RecordPayload{s.scanState(end, std::move(error))});
    return result;
}

Result<std::uint32_t> RecoverySession::addRecoveryJob(const std::filesystem::path& destination,
                                                      std::vector<evaluation::EvaluatedCandidateId> candidates) {
    Impl& s = *impl_;
    if (destination.empty()) {
        return refused("a recovery job needs a destination");
    }
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(destination, ec).lexically_normal();
    if (ec) {
        return refused("the destination '" + toUtf8(destination) + "' cannot be made absolute");
    }
    // One job number at a time.
    const std::lock_guard control(s.controlMutex);
    JobCreatedRecord record;
    record.destination = toUtf8(absolute);
    {
        const std::lock_guard lock(s.mutex);
        record.job = static_cast<std::uint32_t>(s.jobs.size() + 1);
        if (candidates.empty()) {
            if (s.candidates.empty()) {
                return refused("the session has no candidates to recover yet");
            }
            for (std::uint64_t id = 1; id <= s.candidates.size(); ++id) {
                candidates.emplace_back(id);
            }
        }
        std::set<std::uint64_t> seen;
        for (const evaluation::EvaluatedCandidateId id : candidates) {
            if (id.value() == 0 || id.value() > s.candidates.size()) {
                return refused("there is no candidate " + std::to_string(id.value()));
            }
            if (!seen.insert(id.value()).second) {
                return refused("candidate " + std::to_string(id.value()) + " is given twice");
            }
        }
    }
    record.candidates = std::move(candidates);
    const std::uint32_t job = record.job;
    const std::size_t count = record.candidates.size();
    if (Status recorded = s.record(RecordPayload{std::move(record)}); !recorded.ok()) {
        return recorded.error();
    }
    s.log(LogLevel::Info, "recovery job added",
          {field("job", job), field("destination", toUtf8(absolute)), field("candidates", count)});
    return job;
}

Result<scan::RecoveryJobSummary> RecoverySession::runRecovery(std::uint32_t job, storage::IStorageSource& source,
                                                              scan::RecoveryJobOptions options) {
    Impl& s = *impl_;
    Result<JobControl> control = s.begin(SessionOperation::Recovery, job);
    if (!control.ok()) {
        return control.error();
    }
    const auto fail = [&](Error error) -> Error {
        s.finish(std::nullopt);
        return error;
    };
    // The candidates to write: the session's own when the job writes the
    // first ones in order (a scan, the only writer of the list, cannot run
    // while the job does), else copies.
    std::vector<evaluation::EvaluatedCandidate> copies;
    std::span<const evaluation::EvaluatedCandidate> todo;
    std::string destination;
    std::map<std::uint64_t, std::string> leftovers;
    std::optional<scan::RecoveryJobCheckpoint> resumeFrom;
    std::string why;
    {
        const std::lock_guard lock(s.mutex);
        const Impl::Job* found = s.findJob(job);
        if (found == nullptr) {
            why = "there is no recovery job " + std::to_string(job);
        } else if (lastState(found->history) == SessionState::Completed) {
            why = "recovery job " + std::to_string(job) + " is complete";
        } else {
            destination = found->created.destination;
            const std::vector<evaluation::EvaluatedCandidateId>& ids = found->created.candidates;
            bool firstInOrder = true;
            for (std::size_t i = 0; i < ids.size() && firstInOrder; ++i) {
                firstInOrder = ids[i].value() == i + 1;
            }
            if (firstInOrder) {
                todo = std::span(s.candidates).first(ids.size());
            } else {
                for (const evaluation::EvaluatedCandidateId id : ids) {
                    copies.push_back(s.candidates[id.value() - 1]);
                }
                todo = copies;
            }
            leftovers = found->started;
            if (!found->checkpoint.empty()) {
                resumeFrom = found->checkpoint;
            }
        }
    }
    if (!why.empty()) {
        return fail(refused(std::move(why)));
    }
    if (Status same = checkSameSource(s.created.source, source, control->token()); !same.ok()) {
        return fail(same.error());
    }
    Result<std::filesystem::path> destinationPath = fromUtf8(destination);
    if (!destinationPath.ok()) {
        return fail(destinationPath.error());
    }
    // Files an interrupted run began and did not finish: removed, so that the
    // job writes them again under the names they had.
    std::map<std::uint64_t, std::uint64_t> expectedSizes;
    for (const evaluation::EvaluatedCandidate& item : todo) {
        expectedSizes[item.id.value()] = item.data.expectedSize;
    }
    for (const auto& [candidate, path] : leftovers) {
        if (Status removed = s.removeLeftover(path, expectedSizes[candidate]); !removed.ok()) {
            return fail(removed.error());
        }
    }
    if (Status started = s.markStarted(); !started.ok()) {
        return fail(started.error());
    }
    s.log(LogLevel::Info, "recovery job started",
          {field("job", job), field("destination", destination), field("candidates", todo.size()),
           field("leftovers", leftovers.size())});

    options.control = *control;
    options.onFileCreated = [&s, job](evaluation::EvaluatedCandidateId candidate, const std::filesystem::path& path) {
        return s.fileStarted(job, candidate, path);
    };
    Result<scan::RecoveryJobSummary> result = makeError(ErrorCode::InternalError, "session: the job did not run");
    {
        scan::RecoveryJob recovery(source, *destinationPath, std::move(options));
        s.setLive(nullptr, &recovery);
        const scan::RecoveryJobUpdateSink sink = [&s, job](const scan::RecoveryJobUpdate& update) {
            return s.takeJobUpdate(job, update);
        };
        result = recovery.run(todo, sink, resumeFrom.has_value() ? &*resumeFrom : nullptr);
        s.setLive(nullptr, nullptr);
    }

    SessionState end = SessionState::Failed;
    std::optional<Error> error;
    if (result.ok()) {
        end = result->outcome == scan::RecoveryJobOutcome::Completed ? SessionState::Completed
                                                                     : SessionState::Cancelled;
    } else if (result.error().code == ErrorCode::Cancelled) {
        end = SessionState::Cancelled;
    } else {
        error = result.error();
    }
    s.log(end == SessionState::Failed ? LogLevel::Error : LogLevel::Info, "recovery job ended",
          {field("job", job), field("state", toString(end)),
           field("error", error.has_value() ? describe(*error) : std::string())});
    s.finish(RecordPayload{s.jobState(job, end, std::move(error))});
    return result;
}

Status RecoverySession::pause() {
    Impl& s = *impl_;
    const std::lock_guard lock(s.controlMutex);
    if (s.operation == SessionOperation::None) {
        return refused("nothing is running to pause");
    }
    if (s.paused) {
        return success();
    }
    // Before STARTED is recorded, the pause is recorded right after it.
    if (s.started) {
        if (Status recorded = s.record(s.operationState(SessionState::Paused)); !recorded.ok()) {
            return recorded;
        }
    }
    s.control.pause();
    s.paused = true;
    s.log(LogLevel::Info, "paused", {field("operation", toString(s.operation))});
    return success();
}

Status RecoverySession::resume() {
    Impl& s = *impl_;
    const std::lock_guard lock(s.controlMutex);
    if (s.operation == SessionOperation::None) {
        return refused("nothing is running to resume");
    }
    if (!s.paused) {
        return success();
    }
    if (s.started) {
        if (Status recorded = s.record(s.operationState(SessionState::Started)); !recorded.ok()) {
            return recorded;
        }
    }
    s.control.resume();
    s.paused = false;
    s.log(LogLevel::Info, "resumed", {field("operation", toString(s.operation))});
    return success();
}

void RecoverySession::cancel() {
    Impl& s = *impl_;
    const std::lock_guard lock(s.controlMutex);
    if (s.operation != SessionOperation::None) {
        s.control.requestCancellation();
    }
}

// ---------------------------------------------------------------------------
// What the session holds
// ---------------------------------------------------------------------------

SessionProgress RecoverySession::progress() const {
    const Impl& s = *impl_;
    const std::lock_guard lock(s.controlMutex);
    SessionProgress now;
    now.operation = s.operation;
    now.job = s.operationJob;
    now.paused = s.paused;
    if (s.liveScan != nullptr) {
        now.scan = s.liveScan->progress();
    }
    if (s.liveJob != nullptr) {
        now.recovery = s.liveJob->progress();
    }
    return now;
}

SessionInfo RecoverySession::info() const {
    const Impl& s = *impl_;
    const SessionOperation running = s.running.load();
    const std::uint32_t runningJob = s.runningJob.load();
    const std::lock_guard lock(s.mutex);
    SessionInfo about;
    about.id = s.created.id;
    about.folder = s.folder;
    about.formatVersion = s.formatVersion;
    about.engineVersion = s.created.engineVersion;
    about.created = s.createdTime;
    about.updated = s.updated;
    about.source = s.created.source;
    about.configuration = s.created.configuration;
    about.playability = s.created.playability;

    ScanStatus& scan = about.scan;
    scan.history = s.scanHistory;
    scan.state = lastState(s.scanHistory);
    scan.interrupted = scan.state == SessionState::Started && running != SessionOperation::Scan;
    if (!s.checkpoint.empty()) {
        scan.stage = s.checkpoint.stage();
        scan.metrics = s.checkpoint.metrics();
    }
    scan.updates = s.checkpoint.sequence();
    scan.candidates = s.candidates.size();
    scan.notRunnable = s.scanNotRunnable();
    scan.runnable = scan.notRunnable.empty();

    if (s.checkpoint.partitionTable().has_value()) {
        about.partitionScheme = s.checkpoint.partitionTable()->scheme;
    }
    for (const scan::VolumeRecord& record : s.checkpoint.volumes()) {
        VolumeSummary volume;
        volume.offset = record.offset;
        volume.size = record.size;
        if (record.partition.has_value()) {
            volume.partition = record.partition->index;
        }
        volume.scanned = record.scanned;
        volume.filesystem = record.filesystem;
        if (record.candidates.has_value()) {
            volume.label = record.candidates->filesystemInfo.label;
            volume.serialNumber = record.candidates->filesystemInfo.serialNumber;
            volume.clusterSize = record.candidates->filesystemInfo.clusterSize;
            volume.files = record.candidates->candidates.size();
        }
        volume.error = record.error;
        about.volumes.push_back(std::move(volume));
    }
    for (const Impl::Job& job : s.jobs) {
        RecoveryJobStatus status;
        status.id = job.created.job;
        status.destination = job.created.destination;
        status.candidates = job.created.candidates;
        status.created = job.time;
        status.history = job.history;
        status.state = lastState(job.history);
        status.interrupted = status.state == SessionState::Started &&
                             !(running == SessionOperation::Recovery && runningJob == status.id);
        for (const auto& done : job.checkpoint.items()) {
            ++status.done;
            status.recovered += done.second.file.has_value() ? 1 : 0;
            status.failed += done.second.file.has_value() ? 0 : 1;
        }
        status.metrics = job.checkpoint.metrics();
        status.filesInProgress = job.started.size();
        about.jobs.push_back(std::move(status));
    }
    about.damage = s.damage;
    about.tornBytesDropped = s.tornBytes;
    about.recordsSkipped = s.skipped;
    return about;
}

const std::string& RecoverySession::id() const noexcept {
    return impl_->created.id;
}

const std::filesystem::path& RecoverySession::folder() const noexcept {
    return impl_->folder;
}

std::size_t RecoverySession::candidateCount() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->candidates.size();
}

std::vector<evaluation::EvaluatedCandidate> RecoverySession::candidates(std::size_t first, std::size_t count) const {
    const std::lock_guard lock(impl_->mutex);
    const std::vector<evaluation::EvaluatedCandidate>& all = impl_->candidates;
    if (first >= all.size()) {
        return {};
    }
    const std::size_t end = first + std::min(count, all.size() - first);
    return std::vector<evaluation::EvaluatedCandidate>(all.begin() + static_cast<std::ptrdiff_t>(first),
                                                       all.begin() + static_cast<std::ptrdiff_t>(end));
}

std::optional<evaluation::EvaluatedCandidate> RecoverySession::candidate(evaluation::EvaluatedCandidateId id) const {
    const std::lock_guard lock(impl_->mutex);
    if (id.value() == 0 || id.value() > impl_->candidates.size()) {
        return std::nullopt;
    }
    return impl_->candidates[id.value() - 1];
}

std::vector<scan::RecoveredItem> RecoverySession::recoveredItems(std::uint32_t job) const {
    const std::lock_guard lock(impl_->mutex);
    std::vector<scan::RecoveredItem> items;
    if (const Impl::Job* found = impl_->findJob(job); found != nullptr) {
        for (const auto& done : found->checkpoint.items()) {
            items.push_back(done.second);
        }
    }
    return items;
}

std::vector<storage::BadRegion> RecoverySession::unreadableRegions() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->checkpoint.unreadable().regions();
}

std::vector<SessionError> RecoverySession::errors() const {
    const Impl& s = *impl_;
    const std::lock_guard lock(s.mutex);
    std::vector<SessionError> found;
    for (const StateChange& change : s.scanHistory) {
        if (change.error.has_value()) {
            found.push_back(SessionError{change.time, "scan", *change.error});
        }
    }
    const std::vector<scan::VolumeRecord>& volumes = s.checkpoint.volumes();
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        if (volumes[i].error.has_value()) {
            const auto time = s.volumeTimes.find(i);
            found.push_back(SessionError{time != s.volumeTimes.end() ? time->second : s.createdTime,
                                         "volume at " + std::to_string(volumes[i].offset), *volumes[i].error});
        }
    }
    for (const Impl::Job& job : s.jobs) {
        const std::string context = "recovery job " + std::to_string(job.created.job);
        for (const StateChange& change : job.history) {
            if (change.error.has_value()) {
                found.push_back(SessionError{change.time, context, *change.error});
            }
        }
        for (const auto& [id, item] : job.checkpoint.items()) {
            if (item.error.has_value()) {
                const auto time = job.doneAt.find(id);
                found.push_back(SessionError{time != job.doneAt.end() ? time->second : job.time,
                                             context + ", candidate " + std::to_string(id), *item.error});
            }
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const SessionError& a, const SessionError& b) { return a.time < b.time; });
    return found;
}

const scan::ScanCheckpoint& RecoverySession::checkpoint() const noexcept {
    return impl_->checkpoint;
}

// ---------------------------------------------------------------------------
// Sessions without opening them
// ---------------------------------------------------------------------------

Result<SessionSummary> readSessionSummary(const std::filesystem::path& folder) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(folder, ec).lexically_normal();
    if (ec) {
        return refused("the folder '" + toUtf8(folder) + "' cannot be made absolute");
    }
    const std::filesystem::path path = absolute / RecoverySession::kJournalName;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return refused("there is no session in '" + toUtf8(absolute) + "'");
    }
    SessionSummary summary;
    summary.folder = absolute;
    summary.id = toUtf8(absolute.filename());
    Result<JournalReader> reader = JournalReader::open(path);
    if (!reader.ok()) {
        summary.error = reader.error();
        return summary;
    }
    summary.formatVersion = reader->formatVersion();
    // Only the small records are read; the scan's and jobs' updates are skipped.
    const auto small = [](std::uint16_t type) {
        return type == static_cast<std::uint16_t>(RecordType::SessionCreated) ||
               type == static_cast<std::uint16_t>(RecordType::ScanState) ||
               type == static_cast<std::uint16_t>(RecordType::JobCreated);
    };
    bool created = false;
    for (;;) {
        Result<std::optional<JournalRecord>> next = reader->next(small);
        if (!next.ok()) {
            summary.error = next.error();
            return summary;
        }
        if (!next->has_value()) {
            break;
        }
        const JournalRecord& record = **next;
        if (!created && record.type != static_cast<std::uint16_t>(RecordType::SessionCreated)) {
            summary.error = unreadable("the journal does not begin with its session's record");
            return summary;
        }
        if (!knownType(record.type)) {
            if (record.optional()) {
                continue;
            }
            summary.error = unreadable("the journal holds a record of type " + std::to_string(record.type) +
                                       ", which this engine does not know: it is of a newer session format");
            return summary;
        }
        summary.updated = record.time;
        if (!record.payloadRead) {
            continue;
        }
        Result<RecordPayload> payload = decodePayload(static_cast<RecordType>(record.type), record.payload);
        if (!payload.ok()) {
            summary.error = payload.error();
            return summary;
        }
        if (const auto* session = std::get_if<SessionCreatedRecord>(&*payload)) {
            if (created) {
                summary.error = unreadable("a second session record");
                return summary;
            }
            created = true;
            summary.id = session->id;
            summary.engineVersion = session->engineVersion;
            summary.created = record.time;
            summary.sourceType = session->source.type;
            summary.sourcePath = session->source.path;
            summary.sourceSize = session->source.size;
            summary.mode = session->configuration.mode;
        } else if (const auto* state = std::get_if<ScanStateRecord>(&*payload)) {
            summary.state = state->state;
            summary.stage = state->stage;
            summary.metrics = state->metrics;
        } else if (std::holds_alternative<JobCreatedRecord>(*payload)) {
            ++summary.jobs;
        }
    }
    if (!created) {
        summary.error = unreadable("the journal holds no session record (its creation did not finish)");
    }
    return summary;
}

Result<std::vector<SessionSummary>> listSessions(const std::filesystem::path& root) {
    std::vector<SessionSummary> sessions;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) {
        return sessions;
    }
    std::filesystem::directory_iterator entries(root, ec);
    for (; !ec && entries != std::filesystem::directory_iterator(); entries.increment(ec)) {
        const std::filesystem::directory_entry& entry = *entries;
        std::error_code skipped;
        if (!entry.is_directory(skipped) || skipped ||
            !std::filesystem::is_regular_file(entry.path() / RecoverySession::kJournalName, skipped) || skipped) {
            continue;
        }
        Result<SessionSummary> summary = readSessionSummary(entry.path());
        if (summary.ok()) {
            sessions.push_back(std::move(summary).value());
        }
    }
    if (ec) {
        return makeError(ErrorCode::DestinationError, "session: cannot list '" + toUtf8(root) + "'",
                         static_cast<std::uint32_t>(ec.value()));
    }
    std::sort(sessions.begin(), sessions.end(),
              [](const SessionSummary& a, const SessionSummary& b) { return a.folder < b.folder; });
    return sessions;
}

}  // namespace recovery::session
