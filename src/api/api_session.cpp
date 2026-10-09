#include "api_session.hpp"

#include "conversions.hpp"
#include "evaluation/evaluated_candidate.hpp"
#include "recovery/candidate_content.hpp"
#include "recovery/text.hpp"
#include "report/session_report.hpp"
#include "report/text_format.hpp"
#include "storage/bad_region.hpp"
#include "storage/destination_file.hpp"
#include "storage/destination_guard.hpp"
#include "validation/windows_playability.hpp"

#include <algorithm>
#include <cwctype>
#include <exception>
#include <set>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

namespace recovery::api::detail {

namespace {

// Media metadata kept for previews read piece by piece.
constexpr std::size_t kMediaCacheEntries = 8;

std::string_view kindName(OperationKind kind) {
    switch (kind) {
    case OperationKind::Scan:
        return "scan";
    case OperationKind::Recovery:
        return "recovery";
    case OperationKind::Imaging:
        return "imaging";
    case OperationKind::None:
        break;
    }
    return "operation";
}

Error refused(std::string message) {
    return makeError(ErrorCode::InvalidInput, std::move(message));
}

// A folder compared by its text: case folded, '/' as '\', without a trailing
// separator.
std::wstring folderKey(const std::filesystem::path& folder) {
    std::wstring text = folder.lexically_normal().native();
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) {
        text.pop_back();
    }
    for (wchar_t& c : text) {
        c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
    }
    return text;
}

// Whether a job's destination (absolute, normalised UTF-8) is `folder`: the
// same folder on disk when both exist, else the same text.
bool sameFolder(const std::filesystem::path& recorded, const std::filesystem::path& folder) {
    std::error_code ec;
    if (std::filesystem::equivalent(recorded, folder, ec) && !ec) {
        return true;
    }
    return folderKey(recorded) == folderKey(folder);
}

struct RecoveryOf {
    RecoveryState state = RecoveryState::NotRecovered;
    const RecoveryRecord* lastRecovered = nullptr;
    bool complete = false;
};

// Over every job, first match wins: Recovered, Pending, Failed, NotRecovered
// (as metadata::RecoveryJobIndex decides).
RecoveryOf recoveryOf(const std::vector<RecoveryRecord>& records) {
    RecoveryOf out;
    bool pending = false;
    bool failed = false;
    for (const RecoveryRecord& record : records) {
        switch (record.state) {
        case RecoveryState::Recovered:
            out.lastRecovered = &record;
            out.complete = out.complete || record.complete;
            break;
        case RecoveryState::Pending:
            pending = true;
            break;
        case RecoveryState::Failed:
            failed = true;
            break;
        case RecoveryState::NotRecovered:
            break;
        }
    }
    out.state = out.lastRecovered != nullptr ? RecoveryState::Recovered
                : pending                    ? RecoveryState::Pending
                : failed                     ? RecoveryState::Failed
                                             : RecoveryState::NotRecovered;
    return out;
}

template <class T>
bool listed(const std::vector<T>& values, T value) {
    return values.empty() || std::find(values.begin(), values.end(), value) != values.end();
}

bool matches(const CandidateFilter& filter, const CandidateInfo& info, RecoveryState recovery) {
    return listed(filter.kinds, info.kind) && listed(filter.conditions, info.condition) &&
           listed(filter.recovery, recovery) && (!filter.deleted.has_value() || *filter.deleted == info.deleted) &&
           !(filter.skipDuplicates && info.duplicateOf.has_value());
}

// What job `job` did with a candidate it is done with.
RecoveryRecord recordOf(std::uint32_t job, const scan::RecoveredItem& item) {
    const RecoveredFile file = recoveredFileOf(job, item);
    RecoveryRecord record;
    record.job = job;
    record.state = file.file.has_value() ? RecoveryState::Recovered : RecoveryState::Failed;
    record.file = file.file.value_or(std::filesystem::path());
    record.size = file.size;
    record.missingBytes = file.missingBytes;
    record.unreadableBytes = file.unreadableBytes;
    record.complete = file.complete;
    record.error = file.error;
    return record;
}

void applyItem(std::vector<std::vector<RecoveryRecord>>& recoveries, std::uint32_t job,
               const scan::RecoveredItem& item) {
    const std::uint64_t id = item.candidate.value();
    if (id == 0 || id > recoveries.size()) {
        return;
    }
    std::vector<RecoveryRecord>& records = recoveries[id - 1];
    const auto found =
        std::find_if(records.begin(), records.end(), [job](const RecoveryRecord& r) { return r.job == job; });
    if (found == records.end()) {
        records.push_back(recordOf(job, item));
    } else {
        *found = recordOf(job, item);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

OpenSession::LiveToken::LiveToken(ApiContext& context) : context_(context) {
    const std::lock_guard lock(context_.liveMutex);
    ++context_.liveSessions;
}

OpenSession::LiveToken::~LiveToken() {
    {
        const std::lock_guard lock(context_.liveMutex);
        --context_.liveSessions;
    }
    context_.liveChanged.notify_all();
}

OpenSession::OpenSession(ApiContext& context, std::string id, recovery::ScanMode mode)
    : live_(context), context_(context), id_(std::move(id)), mode_(mode) {}

session::SessionOptions OpenSession::sessionOptions() {
    session::SessionOptions options;
    options.logger = context_.logger;
    options.diskResolver = context_.platform.diskResolver;
    options.onScanUpdate = [this](const scan::ScanUpdate& update) { takeScanUpdate(update); };
    options.onJobUpdate = [this](std::uint32_t job, const scan::RecoveryJobUpdate& update) {
        takeJobUpdate(job, update);
    };
    return options;
}

Result<std::shared_ptr<OpenSession>> OpenSession::create(ApiContext& context, storage::IStorageSource& source,
                                                         scan::ScanConfiguration configuration) {
    std::shared_ptr<OpenSession> open(new OpenSession(context, {}, configuration.mode));
    Result<std::unique_ptr<session::RecoverySession>> made = session::RecoverySession::create(
        context.sessionsRoot, source, std::move(configuration), open->sessionOptions());
    if (!made.ok()) {
        return made.error();
    }
    open->session_ = std::move(made).value();
    open->id_ = open->session_->id();
    open->buildIndex();
    return open;
}

Result<std::shared_ptr<OpenSession>> OpenSession::open(ApiContext& context, const std::filesystem::path& folder) {
    std::shared_ptr<OpenSession> open(new OpenSession(context, {}, recovery::ScanMode::Deep));
    Result<std::unique_ptr<session::RecoverySession>> opened =
        session::RecoverySession::open(folder, open->sessionOptions());
    if (!opened.ok()) {
        return opened.error();
    }
    open->session_ = std::move(opened).value();
    open->id_ = open->session_->id();
    open->buildIndex();
    return open;
}

OpenSession::~OpenSession() {
    // No operation runs: its thread would hold a reference. The last
    // operation's thread may be the one destroying the session, at its end.
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            thread_.detach();
        } else {
            thread_.join();
        }
    }
}

Status OpenSession::close() {
    const std::lock_guard lock(opMutex_);
    if (phase_ != Phase::Idle) {
        return refused("session " + id_ + " is busy: stop its operation before closing it");
    }
    closed_ = true;
    return success();
}

void OpenSession::cancelAll() {
    const std::lock_guard lock(opMutex_);
    if (phase_ != Phase::Idle) {
        cancelRequested_ = true;
        session_->cancel();
    }
}

void OpenSession::joinFinished() {
    std::thread finished;
    {
        const std::lock_guard lock(opMutex_);
        if (phase_ != Phase::Idle) {
            return;
        }
        finished = std::move(thread_);
    }
    if (finished.joinable()) {
        if (finished.get_id() == std::this_thread::get_id()) {
            finished.detach();
        } else {
            finished.join();
        }
    }
}

void OpenSession::buildIndex() {
    const session::SessionInfo info = session_->info();
    mode_ = info.configuration.mode;
    source_ = info.source;
    playability_ = info.playability;
    knownBadRegions_ = info.configuration.knownBadRegions;

    const std::vector<evaluation::EvaluatedCandidate> candidates = session_->candidates();
    std::vector<CandidateInfo> index;
    index.reserve(candidates.size());
    for (const evaluation::EvaluatedCandidate& candidate : candidates) {
        index.push_back(describeCandidate(candidate));
    }
    std::vector<std::vector<RecoveryRecord>> recoveries(candidates.size());
    for (const session::RecoveryJobStatus& job : info.jobs) {
        for (const evaluation::EvaluatedCandidateId id : job.candidates) {
            if (id.value() != 0 && id.value() <= recoveries.size()) {
                RecoveryRecord pending;
                pending.job = job.id;
                recoveries[id.value() - 1].push_back(std::move(pending));
            }
        }
        for (const scan::RecoveredItem& item : session_->recoveredItems(job.id)) {
            applyItem(recoveries, job.id, item);
        }
    }
    const std::lock_guard lock(indexMutex_);
    index_ = std::move(index);
    recoveries_ = std::move(recoveries);
}

// ---------------------------------------------------------------------------
// The session's hooks
// ---------------------------------------------------------------------------

void OpenSession::takeScanUpdate(const scan::ScanUpdate& update) {
    if (update.candidates.empty()) {
        return;
    }
    std::uint64_t first = 0;
    std::uint64_t count = 0;
    {
        const std::lock_guard lock(indexMutex_);
        for (const evaluation::EvaluatedCandidate& candidate : update.candidates) {
            // The scan delivers ids in order, from 1: anything else is not
            // the session's (it records nothing it cannot apply).
            if (candidate.id.value() != index_.size() + 1) {
                continue;
            }
            if (count == 0) {
                first = candidate.id.value();
            }
            index_.push_back(describeCandidate(candidate));
            recoveries_.emplace_back();
            ++count;
        }
    }
    if (count == 0) {
        return;
    }
    Event event;
    event.kind = EventKind::CandidatesFound;
    event.session = id_;
    {
        const std::lock_guard lock(opMutex_);
        event.progress = last_.value_or(Progress{});
    }
    event.firstCandidate = CandidateId{first};
    event.candidateCount = count;
    context_.events->post(std::move(event));
}

void OpenSession::takeJobUpdate(std::uint32_t job, const scan::RecoveryJobUpdate& update) {
    if (update.items.empty()) {
        return;
    }
    Event event;
    event.kind = EventKind::FilesRecovered;
    event.session = id_;
    {
        const std::lock_guard lock(indexMutex_);
        for (const scan::RecoveredItem& item : update.items) {
            applyItem(recoveries_, job, item);
            event.files.push_back(recoveredFileOf(job, item));
        }
    }
    {
        const std::lock_guard lock(opMutex_);
        event.progress = last_.value_or(Progress{});
    }
    context_.events->post(std::move(event));
}

void OpenSession::addJobToIndex(std::uint32_t job, const std::vector<evaluation::EvaluatedCandidateId>& candidates) {
    const std::lock_guard lock(indexMutex_);
    for (const evaluation::EvaluatedCandidateId id : candidates) {
        if (id.value() != 0 && id.value() <= recoveries_.size()) {
            RecoveryRecord pending;
            pending.job = job;
            recoveries_[id.value() - 1].push_back(std::move(pending));
        }
    }
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

bool OpenSession::busy() const {
    const std::lock_guard lock(opMutex_);
    return phase_ != Phase::Idle;
}

Status OpenSession::reserve(OperationKind kind) {
    if (context_.closing.load()) {
        return refused("the API is closing: no operation starts");
    }
    const std::lock_guard lock(opMutex_);
    if (closed_) {
        return refused("session " + id_ + " is closed (openSession() opens it again)");
    }
    if (phase_ != Phase::Idle) {
        return refused("session " + id_ + " is busy: a " + std::string(kindName(kind_)) +
                       " is running (one operation at a time)");
    }
    phase_ = Phase::Starting;
    kind_ = kind;
    pauseRequested_ = false;
    pauseApplied_ = false;
    cancelRequested_ = false;
    announced_ = false;
    jobs_.clear();
    jobFiles_.clear();
    jobIndex_ = 0;
    highFraction_ = 0.0;
    ++serial_;
    before_ = last_;
    Progress starting;
    starting.operation = kind;
    starting.state = OperationState::Running;
    last_ = starting;
    return success();
}

void OpenSession::release() {
    {
        const std::lock_guard lock(opMutex_);
        phase_ = Phase::Idle;
        last_ = before_;
        kind_ = last_.has_value() ? last_->operation : OperationKind::None;
    }
    idle_.notify_all();
}

Status OpenSession::launch(OperationKind kind, std::function<void()> body) {
    Progress started;
    started.operation = kind;
    started.state = OperationState::Running;
    {
        const std::lock_guard lock(opMutex_);
        // The previous operation's thread has recorded its end: it is done.
        if (thread_.joinable()) {
            thread_.join();
        }
        if (pauseRequested_) {
            started.state = OperationState::Paused;
        }
        last_ = started;
        try {
            // The thread keeps the session alive until it returns.
            thread_ = std::thread([this, self = shared_from_this(), kind, run = std::move(body)] {
                try {
                    run();
                } catch (const std::exception& e) {
                    finish(kind, OperationState::Failed,
                           makeError(ErrorCode::InternalError, std::string("the operation stopped: ") + e.what()));
                } catch (...) {
                    finish(kind, OperationState::Failed,
                           makeError(ErrorCode::InternalError, "the operation stopped on an unknown exception"));
                }
            });
        } catch (const std::system_error& e) {
            last_ = before_;
            phase_ = Phase::Idle;
            kind_ = before_.has_value() ? before_->operation : OperationKind::None;
            idle_.notify_all();
            return makeError(ErrorCode::InternalError, std::string("cannot start a thread: ") + e.what());
        }
        // Before the thread can report anything (it needs opMutex_ first).
        post(EventKind::OperationStarted, started);
        announced_ = true;
    }
    return success();
}

Status OpenSession::startScan(OpenedSource source, std::uint32_t workers) {
    if (Status reserved = reserve(OperationKind::Scan); !reserved.ok()) {
        return reserved;
    }
    auto shared = std::make_shared<OpenedSource>(std::move(source));
    return launch(OperationKind::Scan, [this, shared, workers] { runScan(std::move(*shared), workers); });
}

Status OpenSession::resumeScan(std::uint32_t workers) {
    {
        std::unique_lock lock(opMutex_);
        if (phase_ != Phase::Idle) {
            if (kind_ != OperationKind::Scan) {
                return refused("session " + id_ + " is busy: a " + std::string(kindName(kind_)) + " is running");
            }
            if (!pauseRequested_) {
                return success();
            }
            pauseRequested_ = false;
            if (pauseApplied_) {
                pauseApplied_ = false;
                if (Status resumed = session_->resume(); !resumed.ok()) {
                    return resumed;
                }
            }
            if (last_.has_value()) {
                last_->state = OperationState::Running;
            }
            const Progress snapshot = last_.value_or(Progress{});
            const bool announce = announced_;
            lock.unlock();
            if (announce) {
                post(EventKind::Resumed, snapshot);
            }
            return success();
        }
    }
    if (Status reserved = reserve(OperationKind::Scan); !reserved.ok()) {
        return reserved;
    }
    const session::SessionInfo info = session_->info();
    if (info.scan.state == session::SessionState::Completed) {
        release();
        return refused("the scan of session " + id_ + " is complete: there is nothing to resume");
    }
    if (!info.scan.runnable) {
        release();
        return refused("the scan of session " + id_ + " cannot be resumed: " + info.scan.notRunnable);
    }
    Result<OpenedSource> source = openOwnSource();
    if (!source.ok()) {
        release();
        return source.error();
    }
    auto shared = std::make_shared<OpenedSource>(std::move(source).value());
    return launch(OperationKind::Scan, [this, shared, workers] { runScan(std::move(*shared), workers); });
}

Status OpenSession::pause(OperationKind kind) {
    std::unique_lock lock(opMutex_);
    if (phase_ == Phase::Idle || kind_ != kind) {
        return refused("no " + std::string(kindName(kind)) + " is running in session " + id_);
    }
    if (pauseRequested_) {
        return success();
    }
    pauseRequested_ = true;
    // Not running yet, or between two recovery jobs: the pause takes effect
    // at the engine's first report.
    if (phase_ == Phase::Running && session_->pause().ok()) {
        pauseApplied_ = true;
    }
    if (last_.has_value()) {
        last_->state = OperationState::Paused;
    }
    const Progress snapshot = last_.value_or(Progress{});
    const bool announce = announced_;
    lock.unlock();
    if (announce) {
        post(EventKind::Paused, snapshot);
    }
    return success();
}

Status OpenSession::cancel(OperationKind kind) {
    const std::lock_guard lock(opMutex_);
    if (phase_ == Phase::Idle || kind_ != kind) {
        return refused("no " + std::string(kindName(kind)) + " is running in session " + id_);
    }
    cancelRequested_ = true;
    // A no-op until the engine's operation runs: then applied at its first report.
    session_->cancel();
    return success();
}

void OpenSession::applyRequestsLocked() {
    if (cancelRequested_) {
        session_->cancel();
    }
    if (pauseRequested_ && !pauseApplied_ && session_->pause().ok()) {
        pauseApplied_ = true;
    }
}

Result<OpenedSource> OpenSession::openOwnSource() const {
    const std::string note = ": the session's source (" + report::describeSource(source_.type, source_.path,
                                                                                  source_.diskNumber, source_.vendor,
                                                                                  source_.product) +
                             ") must be attached and unchanged";
    Result<OpenedSource> opened = openRecordedSource(source_, context_.platform);
    if (!opened.ok()) {
        const Error& error = opened.error();
        return makeError(error.code, "cannot read the session's source: " + error.message + note,
                         error.systemErrorCode);
    }
    if (Status same = session::checkSameSource(source_, *opened->source); !same.ok()) {
        const Error& error = same.error();
        return makeError(error.code, "the source is not the session's: " + error.message + note,
                         error.systemErrorCode);
    }
    return opened;
}

void OpenSession::post(EventKind kind, const Progress& progress) const {
    Event event;
    event.kind = kind;
    event.session = id_;
    event.progress = progress;
    context_.events->post(std::move(event));
}

void OpenSession::runScan(OpenedSource source, std::uint32_t workers) {
    std::unique_ptr<validation::IPlayabilityChecker> checker;
    if (playability_) {
        checker = context_.platform.playability ? context_.platform.playability()
                                                : std::make_unique<validation::WindowsPlayabilityChecker>();
    }
    scan::ScanRunOptions run;
    run.workerThreads = workers;
    run.logger = context_.logger;
    run.progressInterval = context_.progressInterval;
    run.onProgress = [this](const scan::ScanProgress& progress) { onScanProgress(progress); };
    Result<scan::ScanSummary> result =
        session_->runScan(*source.source, context_.formats, context_.media, std::move(run), checker.get());

    OperationState state = OperationState::Completed;
    std::optional<Error> error;
    std::optional<ScanProgress> final;
    if (result.ok()) {
        state = result->outcome == scan::ScanOutcome::Completed ? OperationState::Completed
                                                                : OperationState::Cancelled;
        ScanProgress scan;
        scan.stage = toApi(result->stage);
        const auto [number, count] = stageNumber(result->stage, mode_);
        scan.stageNumber = number;
        scan.stageCount = count;
        scan.fraction = scanFraction(result->stage, 0, 0, mode_);
        scan.metrics = toApi(result->metrics);
        final = scan;
    } else if (result.error().code == ErrorCode::Cancelled) {
        state = OperationState::Cancelled;
    } else {
        state = OperationState::Failed;
        error = result.error();
    }
    {
        const std::lock_guard lock(opMutex_);
        if (final.has_value() && last_.has_value()) {
            final->fraction = std::max(final->fraction, highFraction_);
            highFraction_ = final->fraction;
            last_->scan = final;
        }
    }
    finish(OperationKind::Scan, state, std::move(error));
}

void OpenSession::onScanProgress(const scan::ScanProgress& progress) {
    Progress snapshot;
    {
        const std::lock_guard lock(opMutex_);
        phase_ = Phase::Running;
        applyRequestsLocked();
        ScanProgress scan = scanProgressOf(progress, mode_);
        scan.fraction = std::max(scan.fraction, highFraction_);
        highFraction_ = scan.fraction;
        Progress now = last_.value_or(Progress{});
        now.operation = OperationKind::Scan;
        now.state = pauseRequested_ ? OperationState::Paused : OperationState::Running;
        now.scan = scan;
        last_ = now;
        snapshot = std::move(now);
    }
    if (context_.platform.onOperationProgress) {
        context_.platform.onOperationProgress(id_, snapshot);
    }
    post(EventKind::Progress, snapshot);
}

Result<RecoveryStart> OpenSession::recover(const RecoverySelection& selection, const RecoveryOptions& options) {
    if (options.destination.empty()) {
        return refused("a recovery needs a destination folder");
    }
    if (!selection.all && selection.ids.empty()) {
        return refused("no candidates were given to recover");
    }
    if (Status reserved = reserve(OperationKind::Recovery); !reserved.ok()) {
        return reserved.error();
    }
    const auto fail = [this](Error error) -> Error {
        release();
        return error;
    };

    // The candidates asked for, by id.
    std::vector<std::uint64_t> requested;
    {
        const std::lock_guard lock(indexMutex_);
        if (index_.empty()) {
            return fail(refused("session " + id_ + " has no candidates to recover (yet)"));
        }
        if (selection.all) {
            for (std::size_t i = 0; i < index_.size(); ++i) {
                if (matches(selection.filter, index_[i], recoveryOf(recoveries_[i]).state)) {
                    requested.push_back(index_[i].id.value);
                }
            }
        } else {
            std::set<std::uint64_t> seen;
            for (const CandidateId id : selection.ids) {
                if (id.value == 0 || id.value > index_.size()) {
                    return fail(refused("there is no candidate " + std::to_string(id.value) + ": session " + id_ +
                                        " has " + std::to_string(index_.size())));
                }
                if (seen.insert(id.value).second) {
                    requested.push_back(id.value);
                }
            }
        }
    }
    RecoveryStart start;
    start.requested = requested.size();
    if (requested.empty()) {
        release();
        return start;
    }

    std::error_code ec;
    std::filesystem::path destination = std::filesystem::absolute(options.destination, ec).lexically_normal();
    if (ec) {
        return fail(refused("the folder '" + report::displayPath(options.destination) + "' cannot be made absolute"));
    }
    if (destination.has_relative_path() && !destination.has_filename()) {
        destination = destination.parent_path();
    }
    if (const std::filesystem::file_status status = std::filesystem::status(destination, ec);
        std::filesystem::exists(status) && !std::filesystem::is_directory(status)) {
        return fail(makeError(ErrorCode::DestinationError,
                              "'" + report::displayPath(destination) + "' exists and is not a folder"));
    }
    Result<OpenedSource> source = openOwnSource();
    if (!source.ok()) {
        return fail(source.error());
    }
    if (Status safe = storage::checkDestinationSafety(source->info, destination, context_.platform.diskResolver);
        !safe.ok()) {
        return fail(safe.error());
    }

    // What jobs did and do in this folder.
    const session::SessionInfo info = session_->info();
    std::vector<std::uint32_t> unfinished;
    std::vector<std::uint64_t> unfinishedFiles;
    std::set<std::uint64_t> recoveredHere;
    std::set<std::uint64_t> pendingHere;
    for (const session::RecoveryJobStatus& job : info.jobs) {
        Result<std::filesystem::path> folder = report::pathFromUtf8(job.destination);
        if (!folder.ok() || !sameFolder(*folder, destination)) {
            continue;
        }
        std::set<std::uint64_t> done;
        for (const scan::RecoveredItem& item : session_->recoveredItems(job.id)) {
            done.insert(item.candidate.value());
            if (item.file.has_value()) {
                recoveredHere.insert(item.candidate.value());
            }
        }
        if (job.state != session::SessionState::Completed) {
            unfinished.push_back(job.id);
            unfinishedFiles.push_back(job.candidates.size());
            for (const evaluation::EvaluatedCandidateId candidate : job.candidates) {
                if (!done.contains(candidate.value())) {
                    pendingHere.insert(candidate.value());
                }
            }
        }
    }
    std::vector<evaluation::EvaluatedCandidateId> remaining;
    for (const std::uint64_t id : requested) {
        if (pendingHere.contains(id)) {
            ++start.inUnfinishedJobs;
            continue;
        }
        if (recoveredHere.contains(id) && !options.again) {
            ++start.alreadyRecovered;
            continue;
        }
        remaining.emplace_back(id);
    }
    start.jobs = unfinished;
    std::vector<std::uint64_t> files = unfinishedFiles;
    if (!remaining.empty()) {
        Result<std::uint32_t> job = session_->addRecoveryJob(destination, remaining);
        if (!job.ok()) {
            return fail(job.error());
        }
        addJobToIndex(*job, remaining);
        start.newJob = *job;
        start.jobs.push_back(*job);
        files.push_back(remaining.size());
    }
    if (start.jobs.empty()) {
        release();
        return start;
    }
    {
        const std::lock_guard lock(opMutex_);
        jobs_ = start.jobs;
        jobFiles_ = files;
    }
    auto shared = std::make_shared<OpenedSource>(std::move(source).value());
    const std::vector<std::uint32_t> jobs = start.jobs;
    const std::uint32_t workers = options.workerThreads;
    if (Status launched = launch(OperationKind::Recovery,
                                 [this, shared, jobs, workers] { runRecovery(std::move(*shared), jobs, workers); });
        !launched.ok()) {
        return launched.error();
    }
    return start;
}

Status OpenSession::resumeRecovery(std::uint32_t workers) {
    {
        std::unique_lock lock(opMutex_);
        if (phase_ != Phase::Idle) {
            if (kind_ != OperationKind::Recovery) {
                return refused("session " + id_ + " is busy: a " + std::string(kindName(kind_)) + " is running");
            }
            if (!pauseRequested_) {
                return success();
            }
            pauseRequested_ = false;
            if (pauseApplied_) {
                pauseApplied_ = false;
                if (Status resumed = session_->resume(); !resumed.ok()) {
                    return resumed;
                }
            }
            if (last_.has_value()) {
                last_->state = OperationState::Running;
            }
            const Progress snapshot = last_.value_or(Progress{});
            const bool announce = announced_;
            lock.unlock();
            if (announce) {
                post(EventKind::Resumed, snapshot);
            }
            return success();
        }
    }
    if (Status reserved = reserve(OperationKind::Recovery); !reserved.ok()) {
        return reserved;
    }
    const session::SessionInfo info = session_->info();
    std::vector<std::uint32_t> unfinished;
    std::vector<std::uint64_t> files;
    std::vector<std::filesystem::path> destinations;
    for (const session::RecoveryJobStatus& job : info.jobs) {
        if (job.state == session::SessionState::Completed) {
            continue;
        }
        unfinished.push_back(job.id);
        files.push_back(job.candidates.size());
        if (Result<std::filesystem::path> folder = report::pathFromUtf8(job.destination); folder.ok()) {
            destinations.push_back(std::move(folder).value());
        }
    }
    if (unfinished.empty()) {
        release();
        return refused("session " + id_ + " has no recovery job to resume: every job completed");
    }
    Result<OpenedSource> source = openOwnSource();
    if (!source.ok()) {
        release();
        return source.error();
    }
    for (const std::filesystem::path& destination : destinations) {
        if (Status safe = storage::checkDestinationSafety(source->info, destination, context_.platform.diskResolver);
            !safe.ok()) {
            release();
            return safe;
        }
    }
    {
        const std::lock_guard lock(opMutex_);
        jobs_ = unfinished;
        jobFiles_ = files;
    }
    auto shared = std::make_shared<OpenedSource>(std::move(source).value());
    return launch(OperationKind::Recovery,
                  [this, shared, unfinished, workers] { runRecovery(std::move(*shared), unfinished, workers); });
}

void OpenSession::runRecovery(OpenedSource source, std::vector<std::uint32_t> jobs, std::uint32_t workers) {
    storage::BadRegionMap knownBad;
    for (const storage::BadRegion& region : knownBadRegions_) {
        if (Status added = knownBad.add(region); !added.ok()) {
            finish(OperationKind::Recovery, OperationState::Failed, added.error());
            return;
        }
    }
    OperationState state = OperationState::Completed;
    std::optional<Error> error;
    std::optional<scan::RecoveryJobSummary> last;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        bool cancelled = false;
        {
            const std::lock_guard lock(opMutex_);
            cancelled = cancelRequested_;
            jobIndex_ = i;
            pauseApplied_ = false;
        }
        if (cancelled) {
            state = OperationState::Cancelled;
            break;
        }
        scan::RecoveryJobOptions options;
        options.workerThreads = workers;
        options.writer.reconstruction.knownBadRegions = knownBad.empty() ? nullptr : &knownBad;
        options.writer.diskResolver = context_.platform.diskResolver;
        options.writer.logger = context_.logger;
        options.logger = context_.logger;
        options.progressInterval = context_.progressInterval;
        options.onProgress = [this, i](const scan::RecoveryJobProgress& progress) { onRecoveryProgress(i, progress); };
        Result<scan::RecoveryJobSummary> result = session_->runRecovery(jobs[i], *source.source, std::move(options));
        if (!result.ok()) {
            if (result.error().code == ErrorCode::Cancelled) {
                state = OperationState::Cancelled;
            } else {
                state = OperationState::Failed;
                error = result.error();
            }
            break;
        }
        last = *result;
        if (result->outcome == scan::RecoveryJobOutcome::Cancelled) {
            state = OperationState::Cancelled;
            break;
        }
    }
    {
        const std::lock_guard lock(opMutex_);
        if (last.has_value() && last_.has_value() && last_->recovery.has_value()) {
            last_->recovery->metrics = toApi(last->metrics);
            if (state == OperationState::Completed) {
                last_->recovery->done = last_->recovery->total;
                last_->recovery->fraction = 1.0;
            }
        }
    }
    finish(OperationKind::Recovery, state, std::move(error));
}

void OpenSession::onRecoveryProgress(std::size_t index, const scan::RecoveryJobProgress& progress) {
    Progress snapshot;
    {
        const std::lock_guard lock(opMutex_);
        phase_ = Phase::Running;
        applyRequestsLocked();
        RecoveryProgress recovery;
        recovery.job = index < jobs_.size() ? jobs_[index] : 0;
        recovery.jobNumber = static_cast<std::uint32_t>(index + 1);
        recovery.jobCount = static_cast<std::uint32_t>(jobs_.size());
        recovery.done = progress.done;
        recovery.total = progress.total;
        recovery.metrics = toApi(progress.metrics);
        std::uint64_t before = 0;
        std::uint64_t all = 0;
        for (std::size_t j = 0; j < jobFiles_.size(); ++j) {
            all += jobFiles_[j];
            before += j < index ? jobFiles_[j] : 0;
        }
        const double part = progress.total == 0 ? 0.0
                                                : static_cast<double>(std::min(progress.done, progress.total)) /
                                                      static_cast<double>(progress.total);
        const double current = index < jobFiles_.size() ? static_cast<double>(jobFiles_[index]) : 0.0;
        const double share = all == 0 ? 0.0 : (static_cast<double>(before) + part * current) / static_cast<double>(all);
        recovery.fraction = std::clamp(share, 0.0, 1.0);
        recovery.fraction = std::max(recovery.fraction, highFraction_);
        highFraction_ = recovery.fraction;
        Progress now = last_.value_or(Progress{});
        now.operation = OperationKind::Recovery;
        now.state = pauseRequested_ ? OperationState::Paused : OperationState::Running;
        now.recovery = recovery;
        last_ = now;
        snapshot = std::move(now);
    }
    if (context_.platform.onOperationProgress) {
        context_.platform.onOperationProgress(id_, snapshot);
    }
    post(EventKind::Progress, snapshot);
}

void OpenSession::finish(OperationKind kind, OperationState state, std::optional<Error> error) {
    Progress final;
    {
        const std::lock_guard lock(opMutex_);
        Progress now = last_.value_or(Progress{});
        now.operation = kind;
        now.state = state;
        now.error = std::move(error);
        last_ = now;
        final = std::move(now);
        phase_ = Phase::Idle;
        kind_ = kind;
        pauseRequested_ = false;
        pauseApplied_ = false;
        cancelRequested_ = false;
    }
    idle_.notify_all();
    post(EventKind::OperationFinished, final);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Progress OpenSession::progress() const {
    std::optional<Progress> snapshot;
    Phase phase = Phase::Idle;
    std::uint64_t serial = 0;
    {
        const std::lock_guard lock(opMutex_);
        phase = phase_;
        snapshot = last_;
        serial = serial_;
    }
    if (!snapshot.has_value()) {
        // Nothing ran since the session was opened: what its scan recorded.
        const session::SessionInfo info = session_->info();
        Progress idle;
        idle.scan = scanProgressOf(info.scan, mode_);
        return idle;
    }
    if (phase != Phase::Idle) {
        // Live figures from the engine, between its reports.
        const session::SessionProgress live = session_->progress();
        if (snapshot->operation == OperationKind::Scan && live.scan.has_value()) {
            snapshot->scan = scanProgressOf(*live.scan, mode_);
        }
        if (snapshot->operation == OperationKind::Recovery && live.recovery.has_value() &&
            snapshot->recovery.has_value() && live.job == snapshot->recovery->job) {
            snapshot->recovery->done = live.recovery->done;
            snapshot->recovery->total = live.recovery->total;
            snapshot->recovery->metrics = toApi(live.recovery->metrics);
        }
    }
    double* fraction = snapshot->operation == OperationKind::Scan && snapshot->scan.has_value()
                           ? &snapshot->scan->fraction
                       : snapshot->operation == OperationKind::Recovery && snapshot->recovery.has_value()
                           ? &snapshot->recovery->fraction
                           : nullptr;
    if (fraction != nullptr) {
        const std::lock_guard lock(opMutex_);
        // Another operation began since: this answer is about the one before.
        if (serial == serial_) {
            *fraction = std::max(*fraction, highFraction_);
            highFraction_ = *fraction;
        }
    }
    return *snapshot;
}

Progress OpenSession::wait(std::chrono::milliseconds timeout) const {
    {
        std::unique_lock lock(opMutex_);
        idle_.wait_for(lock, timeout, [this] { return phase_ == Phase::Idle; });
    }
    return progress();
}

SessionDetails OpenSession::details() const {
    const session::SessionInfo info = session_->info();
    SessionDetails details = sessionDetailsOf(info, session_->errors(), session_->unreadableRegions());
    const std::lock_guard lock(opMutex_);
    if (phase_ != Phase::Idle) {
        details.running = kind_;
        details.paused = pauseRequested_;
    }
    return details;
}

CandidateInfo OpenSession::withRecovery(CandidateInfo info, const std::vector<RecoveryRecord>& records) const {
    const RecoveryOf recovery = recoveryOf(records);
    info.recovery = recovery.state;
    info.recoveredComplete = recovery.complete;
    if (recovery.lastRecovered != nullptr) {
        info.recoveredFile = recovery.lastRecovered->file;
    }
    return info;
}

Result<CandidatePage> OpenSession::candidates(const CandidateQuery& query) const {
    const std::uint64_t count = std::min(query.count, CandidateQuery::kMaxPage);
    CandidatePage page;
    const std::lock_guard lock(indexMutex_);
    page.total = index_.size();
    for (std::size_t i = 0; i < index_.size(); ++i) {
        const RecoveryState state = recoveryOf(recoveries_[i]).state;
        if (!matches(query.filter, index_[i], state)) {
            continue;
        }
        if (page.matching >= query.first && page.candidates.size() < count) {
            page.candidates.push_back(withRecovery(index_[i], recoveries_[i]));
        }
        ++page.matching;
    }
    return page;
}

Result<metadata::MediaMetadata> OpenSession::mediaOf(const evaluation::EvaluatedCandidate& candidate) const {
    // Under previewMutex_.
    if (const auto found = mediaCache_.find(candidate.id.value()); found != mediaCache_.end()) {
        return found->second;
    }
    if (!previewSource_.has_value()) {
        Result<OpenedSource> source = openOwnSource();
        if (!source.ok()) {
            return source.error();
        }
        previewSource_ = std::move(source).value();
    }
    Result<metadata::MediaMetadata> media = metadata::readMediaMetadata(*previewSource_->source, candidate);
    if (!media.ok()) {
        return media.error();
    }
    if (mediaCache_.size() >= kMediaCacheEntries) {
        mediaCache_.clear();
    }
    mediaCache_.emplace(candidate.id.value(), *media);
    return media;
}

Result<CandidateDetails> OpenSession::candidateDetails(CandidateId candidate, bool readMedia) const {
    CandidateDetails details;
    {
        const std::lock_guard lock(indexMutex_);
        if (candidate.value == 0 || candidate.value > index_.size()) {
            return refused("there is no candidate " + std::to_string(candidate.value) + ": session " + id_ + " has " +
                           std::to_string(index_.size()));
        }
        details.candidate = withRecovery(index_[candidate.value - 1], recoveries_[candidate.value - 1]);
        details.recoveries = recoveries_[candidate.value - 1];
    }
    const std::optional<evaluation::EvaluatedCandidate> evaluated =
        session_->candidate(evaluation::EvaluatedCandidateId{candidate.value});
    if (!evaluated.has_value()) {
        return makeError(ErrorCode::InternalError, "candidate " + std::to_string(candidate.value) +
                                                       " is listed but the session does not have it");
    }
    details.explanation = evaluation::explain(*evaluated);
    if (readMedia) {
        const std::lock_guard lock(previewMutex_);
        Result<metadata::MediaMetadata> media = mediaOf(*evaluated);
        if (media.ok()) {
            details.media = toApi(*media);
        } else {
            details.mediaError = media.error();
        }
    }
    return details;
}

Result<std::vector<std::byte>> OpenSession::preview(CandidateId candidate, std::size_t preview, std::uint64_t offset,
                                                    std::uint64_t maxBytes) const {
    std::size_t candidates = 0;
    {
        const std::lock_guard lock(indexMutex_);
        candidates = index_.size();
    }
    if (candidate.value == 0 || candidate.value > candidates) {
        return refused("there is no candidate " + std::to_string(candidate.value) + ": session " + id_ + " has " +
                       std::to_string(candidates));
    }
    const std::optional<evaluation::EvaluatedCandidate> evaluated =
        session_->candidate(evaluation::EvaluatedCandidateId{candidate.value});
    if (!evaluated.has_value()) {
        return makeError(ErrorCode::InternalError, "candidate " + std::to_string(candidate.value) +
                                                       " is listed but the session does not have it");
    }
    const std::lock_guard lock(previewMutex_);
    Result<metadata::MediaMetadata> media = mediaOf(*evaluated);
    if (!media.ok()) {
        return media.error();
    }
    if (preview >= media->previews.size()) {
        return refused("candidate " + std::to_string(candidate.value) + " has " +
                       std::to_string(media->previews.size()) + " previews: there is no preview " +
                       std::to_string(preview));
    }
    const metadata::PreviewSource& source = media->previews[preview];
    if (offset > source.length) {
        return refused("offset " + std::to_string(offset) + " is beyond the preview's " +
                       std::to_string(source.length) + " bytes");
    }
    const std::uint64_t length =
        std::min({maxBytes, RecoveryApi::kMaxPreviewRead, source.length - offset});
    std::vector<std::byte> bytes;
    if (length == 0) {
        return bytes;
    }
    Result<std::unique_ptr<CandidateContentReader>> content =
        CandidateContentReader::open(*previewSource_->source, evaluated->data);
    if (!content.ok()) {
        return content.error();
    }
    Result<std::unique_ptr<carving::IContentReader>> view = metadata::openPreview(**content, source);
    if (!view.ok()) {
        return view.error();
    }
    bytes.reserve(static_cast<std::size_t>(length));
    for (std::uint64_t done = 0; done < length;) {
        const auto piece =
            static_cast<std::size_t>(std::min<std::uint64_t>(length - done, carving::IContentReader::kMaxReadLength));
        Result<std::span<const std::byte>> read = (*view)->read(offset + done, piece);
        if (!read.ok()) {
            return read.error();
        }
        bytes.insert(bytes.end(), read->begin(), read->end());
        done += piece;
    }
    return bytes;
}

Status OpenSession::exportReport(const std::filesystem::path& file, const ReportOptions& options) const {
    if (file.empty()) {
        return refused("a report needs a file name");
    }
    const report::SessionReport gathered = report::gatherReport(*session_);
    const std::string text = options.format == ReportFormat::Json ? report::jsonReport(gathered)
                                                                  : report::textReport(gathered, options.details);
    if (Status safe = storage::checkDestinationSafety(sourceInfoOf(source_), file, context_.platform.diskResolver);
        !safe.ok()) {
        return safe;
    }
    Result<storage::DestinationFile> created =
        storage::DestinationFile::open(file, storage::DestinationFile::OpenMode::CreateNew);
    if (!created.ok()) {
        const Error& error = created.error();
        return makeError(error.code,
                         "cannot create the report '" + report::displayPath(file) +
                             "' (an existing file is never overwritten): " + error.message,
                         error.systemErrorCode);
    }
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    Status written = created->writeAt(0, bytes);
    if (written.ok()) {
        written = created->flush();
    }
    created->close();
    if (!written.ok()) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
        return written;
    }
    return success();
}

}  // namespace recovery::api::detail
