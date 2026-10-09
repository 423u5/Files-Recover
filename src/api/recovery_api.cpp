#include "api/recovery_api.hpp"

#include "api/api_platform.hpp"
#include "api_session.hpp"
#include "api_sources.hpp"
#include "conversions.hpp"
#include "event_dispatcher.hpp"
#include "formats/audio_formats.hpp"
#include "formats/image_formats.hpp"
#include "formats/video_formats.hpp"
#include "imaging/image_metadata.hpp"
#include "imaging/image_writer.hpp"
#include "recovery/cancellation.hpp"
#include "recovery/text.hpp"
#include "recovery/version.hpp"
#include "report/text_format.hpp"
#include "storage/destination_guard.hpp"
#include "validation/media_validator.hpp"
#include "validation/windows_playability.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace recovery::api {

namespace detail {
namespace {

// The longest session id taken (the engine's are 24 characters).
constexpr std::size_t kMaxSessionId = 128;

// Forwards the engine's log records to the user interface's callback.
class CallbackSink final : public diagnostics::ILogSink {
public:
    explicit CallbackSink(LogCallback callback) : callback_(std::move(callback)) {}

    void write(const diagnostics::LogRecord& record) override {
        LogEntry entry;
        entry.time = std::chrono::floor<std::chrono::milliseconds>(record.timestamp);
        entry.level = toApi(record.level);
        entry.component = record.component;
        entry.message = record.message;
        for (const diagnostics::LogField& field : record.fields) {
            entry.fields.emplace_back(field.key, field.value);
        }
        entry.line = diagnostics::formatRecord(record);
        try {
            callback_(entry);
        } catch (...) {
            // The callback must not throw; the log goes on without it.
        }
    }

private:
    LogCallback callback_;
};

// An imaging under way, or ended.
struct ImagingOperation {
    ImagingId id;
    CancellationSource cancel;
    mutable std::mutex mutex;
    mutable std::condition_variable ended;
    bool finished = false;
    Progress progress;
    std::thread thread;
};

ImagingProgress imagingProgressOf(const imaging::ImagingProgress& progress, ImagingProgress out) {
    out.bytesImaged = progress.bytesCompleted;
    out.totalBytes = progress.totalBytes;
    out.resumedFrom = progress.resumedFrom;
    out.unreadableBytes = progress.unreadableBytes;
    out.unreadableRegions = progress.badRegionCount;
    out.elapsed = progress.elapsed;
    const auto milliseconds = progress.elapsed.count();
    const std::uint64_t imaged = progress.bytesCompleted - std::min(progress.bytesCompleted, progress.resumedFrom);
    out.bytesPerSecond = milliseconds > 0 ? imaged * 1000 / static_cast<std::uint64_t>(milliseconds) : 0;
    const std::uint64_t done = std::min(progress.bytesCompleted, progress.totalBytes);
    out.fraction = progress.totalBytes == 0 ? 0.0
                                            : static_cast<double>(done) / static_cast<double>(progress.totalBytes);
    return out;
}

Status checkSessionId(std::string_view id) {
    const bool valid = !id.empty() && id.size() <= kMaxSessionId &&
                       std::all_of(id.begin(), id.end(), [](char c) {
                           return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                  c == '-' || c == '_';
                       });
    if (!valid) {
        return makeError(ErrorCode::InvalidInput,
                         "'" + report::printable(id) + "' is not a session id (letters, digits, '-' and '_')");
    }
    return success();
}

}  // namespace
}  // namespace detail

// ---------------------------------------------------------------------------
// The API's state
// ---------------------------------------------------------------------------

struct RecoveryApi::Impl {
    Impl(ApiOptions apiOptions, detail::Platform platform, std::filesystem::path root)
        : options(std::move(apiOptions)),
          events(options.onEvent, platform.maxQueuedFiles != 0 ? platform.maxQueuedFiles
                                                               : detail::EventDispatcher::kDefaultMaxQueuedFiles) {
        if (options.onLog) {
            logger = std::make_unique<diagnostics::Logger>(detail::toEngine(options.logLevel));
            logger->addSink(std::make_shared<detail::CallbackSink>(options.onLog));
        }
        context.platform = std::move(platform);
        context.sessionsRoot = std::move(root);
        context.logger = logger.get();
        context.events = &events;
        context.progressInterval = options.progressInterval;
    }

    // Cancels every operation and waits for it (its events still reach the
    // user interface), closes the sessions (waiting for the last reference a
    // callback may hold), then stops the events.
    ~Impl() {
        context.closing = true;
        std::map<std::string, std::shared_ptr<detail::OpenSession>> open;
        std::map<std::uint32_t, std::shared_ptr<detail::ImagingOperation>> imaging;
        {
            const std::lock_guard lock(mutex);
            open.swap(sessions);
            imaging.swap(imagings);
        }
        for (auto& [id, operation] : imaging) {
            operation->cancel.requestCancellation();
        }
        for (auto& [id, session] : open) {
            session->cancelAll();
        }
        for (auto& [id, operation] : imaging) {
            if (operation->thread.joinable()) {
                operation->thread.join();
            }
        }
        for (auto& [id, session] : open) {
            while (session->busy()) {
                (void)session->wait(std::chrono::seconds(1));
            }
            session->joinFinished();
        }
        open.clear();
        {
            std::unique_lock lock(context.liveMutex);
            context.liveChanged.wait(lock, [this] { return context.liveSessions == 0; });
        }
        events.stop();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    [[nodiscard]] Result<std::shared_ptr<detail::OpenSession>> find(std::string_view id) const {
        if (Status valid = detail::checkSessionId(id); !valid.ok()) {
            return valid.error();
        }
        const std::lock_guard lock(mutex);
        const auto found = sessions.find(std::string(id));
        if (found == sessions.end()) {
            return makeError(ErrorCode::InvalidInput,
                             "session " + std::string(id) + " is not open (openSession() opens it)");
        }
        return found->second;
    }

    [[nodiscard]] Result<std::shared_ptr<detail::ImagingOperation>> findImaging(ImagingId id) const {
        const std::lock_guard lock(mutex);
        const auto found = imagings.find(id.value);
        if (found == imagings.end()) {
            return makeError(ErrorCode::InvalidInput, "there is no imaging " + std::to_string(id.value));
        }
        return found->second;
    }

    ApiOptions options;
    std::unique_ptr<diagnostics::Logger> logger;
    detail::EventDispatcher events;
    detail::ApiContext context;
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<detail::OpenSession>> sessions;
    std::map<std::uint32_t, std::shared_ptr<detail::ImagingOperation>> imagings;
    std::uint32_t nextImaging = 1;
};

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

Result<std::unique_ptr<RecoveryApi>> createRecoveryApi(ApiOptions options, PlatformHooks hooks) {
    if (options.progressInterval.count() < 0) {
        return makeError(ErrorCode::InvalidInput, "the progress interval must not be negative");
    }
    std::filesystem::path root = options.sessionsRoot;
    if (root.empty()) {
        const std::optional<std::filesystem::path> local = detail::environmentFolder(L"LOCALAPPDATA");
        if (!local.has_value()) {
            return makeError(ErrorCode::DestinationError,
                             "the sessions folder is not known: %LOCALAPPDATA% is not set (give "
                             "ApiOptions::sessionsRoot)");
        }
        root = *local / "RecoveryEngine" / "Sessions";
    }
    std::error_code ec;
    root = std::filesystem::absolute(root, ec).lexically_normal();
    if (ec) {
        return makeError(ErrorCode::DestinationError,
                         "the sessions folder '" + report::displayPath(options.sessionsRoot) +
                             "' cannot be made absolute");
    }
    auto impl = std::make_unique<RecoveryApi::Impl>(std::move(options), detail::platformOf(std::move(hooks)),
                                                    std::move(root));
    for (Status registered : {formats::registerImageFormats(impl->context.formats),
                              formats::registerAudioFormats(impl->context.formats),
                              formats::registerVideoFormats(impl->context.formats),
                              validation::registerMediaValidators(impl->context.media)}) {
        if (!registered.ok()) {
            return makeError(ErrorCode::InternalError, "cannot register the formats: " + describe(registered.error()));
        }
    }
    return std::unique_ptr<RecoveryApi>(new RecoveryApi(std::move(impl)));
}

Result<std::unique_ptr<RecoveryApi>> RecoveryApi::create(ApiOptions options) {
    return createRecoveryApi(std::move(options), PlatformHooks{});
}

RecoveryApi::RecoveryApi(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RecoveryApi::~RecoveryApi() = default;

std::string RecoveryApi::engineVersion() {
    return std::string(kEngineName) + " " + std::string(kEngineVersion);
}

const std::filesystem::path& RecoveryApi::sessionsRoot() const noexcept {
    return impl_->context.sessionsRoot;
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

Result<std::vector<DiskInfo>> RecoveryApi::listSources() const {
    return detail::listDisks(impl_->context.platform, impl_->context.sessionsRoot);
}

Result<SourceInspection> RecoveryApi::inspectSource(const SourceRef& source) const {
    Result<detail::OpenedSource> opened = detail::openSource(source, impl_->context.platform);
    if (!opened.ok()) {
        return opened.error();
    }
    return detail::inspect(*opened, impl_->context.logger);
}

// ---------------------------------------------------------------------------
// Imaging
// ---------------------------------------------------------------------------

Result<ImagingId> RecoveryApi::createImage(const SourceRef& source, const ImagingOptions& options) {
    if (options.image.empty()) {
        return makeError(ErrorCode::InvalidInput, "an image needs a file name");
    }
    if (options.sectorRetries > imaging::ImagingOptions::kMaxSectorRetries) {
        return makeError(ErrorCode::InvalidInput,
                         "at most " + std::to_string(imaging::ImagingOptions::kMaxSectorRetries) + " sector retries");
    }
    if (options.blockSize == 0 || options.blockSize > storage::kMaxReadSize) {
        return makeError(ErrorCode::InvalidInput, "the block size must be between 1 byte and " +
                                                      report::formatSize(storage::kMaxReadSize));
    }
    Result<detail::OpenedSource> opened = detail::openSource(source, impl_->context.platform);
    if (!opened.ok()) {
        return opened.error();
    }
    if (const std::uint32_t sector = opened->info.logicalSectorSize;
        sector != 0 && options.blockSize % sector != 0) {
        return makeError(ErrorCode::InvalidInput, "the block size must be a multiple of the source's " +
                                                      std::to_string(sector) + "-byte sectors");
    }
    std::error_code ec;
    const bool exists = std::filesystem::exists(options.image, ec);
    if (options.resume && !exists) {
        return makeError(ErrorCode::InvalidInput,
                         "there is no image '" + report::displayPath(options.image) + "' to resume");
    }
    if (!options.resume && exists) {
        return makeError(ErrorCode::DestinationError, "the file '" + report::displayPath(options.image) +
                                                          "' exists: a file is never overwritten (an unfinished "
                                                          "image goes on with ImagingOptions::resume)");
    }
    if (Status safe =
            storage::checkDestinationSafety(opened->info, options.image, impl_->context.platform.diskResolver);
        !safe.ok()) {
        return safe.error();
    }

    auto operation = std::make_shared<detail::ImagingOperation>();
    {
        const std::lock_guard lock(impl_->mutex);
        operation->id = ImagingId{impl_->nextImaging++};
    }
    ImagingProgress initial;
    initial.image = options.image;
    initial.metadataFile = imaging::metadataPathFor(options.image);
    initial.totalBytes = opened->info.sizeBytes;
    operation->progress.operation = OperationKind::Imaging;
    operation->progress.state = OperationState::Running;
    operation->progress.imaging = initial;

    imaging::ImagingOptions imagingOptions;
    imagingOptions.blockSize = static_cast<std::size_t>(options.blockSize);
    imagingOptions.sectorRetryCount = options.sectorRetries;
    imagingOptions.resume = options.resume;
    imagingOptions.progressInterval = impl_->options.progressInterval;
    imagingOptions.cancellation = operation->cancel.token();
    imagingOptions.logger = impl_->context.logger;
    imagingOptions.diskResolver = impl_->context.platform.diskResolver;
    detail::EventDispatcher* events = &impl_->events;
    detail::ImagingOperation* raw = operation.get();
    const auto post = [events, raw](EventKind kind, const Progress& progress) {
        Event event;
        event.kind = kind;
        event.imaging = raw->id;
        event.progress = progress;
        events->post(std::move(event));
    };
    imagingOptions.onProgress = [raw, post, hook = impl_->context.platform.onOperationProgress](
                                    const imaging::ImagingProgress& progress) {
        Progress snapshot;
        {
            const std::lock_guard lock(raw->mutex);
            raw->progress.imaging =
                detail::imagingProgressOf(progress, raw->progress.imaging.value_or(ImagingProgress{}));
            snapshot = raw->progress;
        }
        if (hook) {
            hook(std::string_view(), snapshot);
        }
        post(EventKind::Progress, snapshot);
    };

    auto shared = std::make_shared<detail::OpenedSource>(std::move(opened).value());
    const std::filesystem::path image = options.image;
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->imagings.emplace(operation->id.value, operation);
        const std::lock_guard operationLock(operation->mutex);
        try {
            operation->thread = std::thread([raw, shared, image, imagingOptions, post] {
                OperationState state = OperationState::Completed;
                std::optional<Error> error;
                std::optional<imaging::ImagingSummary> summary;
                try {
                    imaging::ImageWriter writer(*shared->source, image, imagingOptions);
                    Result<imaging::ImagingSummary> result = writer.run();
                    if (result.ok()) {
                        state = result->outcome == imaging::ImagingOutcome::Completed ? OperationState::Completed
                                                                                      : OperationState::Cancelled;
                        summary = std::move(result).value();
                    } else if (result.error().code == ErrorCode::Cancelled) {
                        state = OperationState::Cancelled;
                    } else {
                        state = OperationState::Failed;
                        error = result.error();
                    }
                } catch (const std::exception& e) {
                    state = OperationState::Failed;
                    error = makeError(ErrorCode::InternalError, std::string("the imaging stopped: ") + e.what());
                }
                Progress final;
                {
                    const std::lock_guard lock(raw->mutex);
                    raw->progress.state = state;
                    raw->progress.error = error;
                    if (summary.has_value() && raw->progress.imaging.has_value()) {
                        ImagingProgress& out = *raw->progress.imaging;
                        out.image = summary->imagePath;
                        out.metadataFile = summary->metadataPath;
                        out.bytesImaged = summary->bytesCompleted;
                        out.totalBytes = summary->totalBytes;
                        out.resumedFrom = summary->resumedFrom;
                        out.unreadableBytes = summary->unreadableBytes;
                        out.unreadableRegions = summary->badRegions.size();
                        out.fraction = summary->totalBytes == 0
                                           ? 1.0
                                           : static_cast<double>(std::min(summary->bytesCompleted,
                                                                          summary->totalBytes)) /
                                                 static_cast<double>(summary->totalBytes);
                    }
                    raw->finished = true;
                    final = raw->progress;
                }
                raw->ended.notify_all();
                post(EventKind::OperationFinished, final);
            });
        } catch (const std::system_error& e) {
            impl_->imagings.erase(operation->id.value);
            return makeError(ErrorCode::InternalError, std::string("cannot start a thread: ") + e.what());
        }
        // Before the thread can report anything (it needs the operation's lock).
        post(EventKind::OperationStarted, operation->progress);
    }
    return operation->id;
}

Result<Progress> RecoveryApi::getImagingProgress(ImagingId imaging) const {
    Result<std::shared_ptr<detail::ImagingOperation>> operation = impl_->findImaging(imaging);
    if (!operation.ok()) {
        return operation.error();
    }
    const std::lock_guard lock((*operation)->mutex);
    return (*operation)->progress;
}

Status RecoveryApi::cancelImaging(ImagingId imaging) {
    Result<std::shared_ptr<detail::ImagingOperation>> operation = impl_->findImaging(imaging);
    if (!operation.ok()) {
        return operation.error();
    }
    {
        const std::lock_guard lock((*operation)->mutex);
        if ((*operation)->finished) {
            return makeError(ErrorCode::InvalidInput, "imaging " + std::to_string(imaging.value) + " has ended");
        }
    }
    (*operation)->cancel.requestCancellation();
    return success();
}

Result<Progress> RecoveryApi::waitForImaging(ImagingId imaging, std::chrono::milliseconds timeout) const {
    Result<std::shared_ptr<detail::ImagingOperation>> operation = impl_->findImaging(imaging);
    if (!operation.ok()) {
        return operation.error();
    }
    const detail::ImagingOperation& op = **operation;
    std::unique_lock lock(op.mutex);
    op.ended.wait_for(lock, timeout, [&op] { return op.finished; });
    return op.progress;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

Result<std::vector<SessionListing>> RecoveryApi::listSessions() const {
    Result<std::vector<session::SessionSummary>> summaries = session::listSessions(impl_->context.sessionsRoot);
    if (!summaries.ok()) {
        return summaries.error();
    }
    std::vector<SessionListing> listings;
    listings.reserve(summaries->size());
    for (const session::SessionSummary& summary : *summaries) {
        SessionListing listing = detail::listingOf(summary);
        std::shared_ptr<detail::OpenSession> open;
        {
            const std::lock_guard lock(impl_->mutex);
            if (const auto found = impl_->sessions.find(summary.id); found != impl_->sessions.end()) {
                open = found->second;
            }
        }
        if (open) {
            const SessionDetails details = open->details();
            listing.open = true;
            listing.scanState = details.scan.state;
            listing.stage = details.scan.stage;
            listing.candidates = details.scan.candidates;
            listing.jobs = details.jobs.size();
        }
        listings.push_back(std::move(listing));
    }
    return listings;
}

Result<SessionDetails> RecoveryApi::openSession(std::string_view id) {
    if (Status valid = detail::checkSessionId(id); !valid.ok()) {
        return valid.error();
    }
    {
        const std::lock_guard lock(impl_->mutex);
        if (const auto found = impl_->sessions.find(std::string(id)); found != impl_->sessions.end()) {
            const std::shared_ptr<detail::OpenSession> open = found->second;
            return open->details();
        }
    }
    Result<std::shared_ptr<detail::OpenSession>> opened =
        detail::OpenSession::open(impl_->context, impl_->context.sessionsRoot / std::filesystem::path(std::string(id)));
    if (!opened.ok()) {
        const Error& error = opened.error();
        return makeError(error.code, "cannot open session " + std::string(id) + ": " + error.message,
                         error.systemErrorCode);
    }
    std::shared_ptr<detail::OpenSession> open = std::move(opened).value();
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->sessions.emplace(open->id(), open);
    }
    return open->details();
}

Status RecoveryApi::closeSession(std::string_view id) {
    std::shared_ptr<detail::OpenSession> closing;
    {
        if (Status valid = detail::checkSessionId(id); !valid.ok()) {
            return valid;
        }
        const std::lock_guard lock(impl_->mutex);
        const auto found = impl_->sessions.find(std::string(id));
        if (found == impl_->sessions.end()) {
            return makeError(ErrorCode::InvalidInput, "session " + std::string(id) + " is not open");
        }
        // Refused while an operation runs; from now on none starts in it.
        if (Status closed = found->second->close(); !closed.ok()) {
            return closed;
        }
        closing = std::move(found->second);
        impl_->sessions.erase(found);
    }
    // Closed here (its last operation's thread is joined first), unless a
    // call that uses it is under way: then by that call.
    closing->joinFinished();
    closing.reset();
    return success();
}

Result<SessionDetails> RecoveryApi::getSession(std::string_view id) const {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(id);
    if (!open.ok()) {
        return open.error();
    }
    return (*open)->details();
}

// ---------------------------------------------------------------------------
// Scans
// ---------------------------------------------------------------------------

Result<std::string> RecoveryApi::startScan(const SourceRef& source, const ScanSettings& settings) {
    Result<scan::ScanConfiguration> configuration = detail::configurationOf(settings);
    if (!configuration.ok()) {
        return configuration.error();
    }
    Result<detail::OpenedSource> opened = detail::openSource(source, impl_->context.platform);
    if (!opened.ok()) {
        return opened.error();
    }
    configuration->knownBadRegions = detail::knownBadRegions(*opened);
    // The session records only whether the scan validates playability; each
    // run makes its own checker.
    validation::WindowsPlayabilityChecker marker;
    configuration->playability = settings.playability ? &marker : nullptr;
    Result<std::shared_ptr<detail::OpenSession>> created =
        detail::OpenSession::create(impl_->context, *opened->source, std::move(configuration).value());
    if (!created.ok()) {
        const Error& error = created.error();
        return makeError(error.code,
                         "cannot create a session in '" + report::displayPath(impl_->context.sessionsRoot) +
                             "': " + error.message,
                         error.systemErrorCode);
    }
    std::shared_ptr<detail::OpenSession> open = std::move(created).value();
    const std::string id = open->id();
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->sessions.emplace(id, open);
    }
    if (Status started = open->startScan(std::move(opened).value(), settings.workerThreads); !started.ok()) {
        const Error& error = started.error();
        return makeError(error.code,
                         "session " + id + " was created but its scan did not start (resumeScan() starts it): " +
                             error.message,
                         error.systemErrorCode);
    }
    return id;
}

Status RecoveryApi::pauseScan(std::string_view session) {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    return open.ok() ? (*open)->pause(OperationKind::Scan) : Status(open.error());
}

Status RecoveryApi::resumeScan(std::string_view session, std::uint32_t workerThreads) {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    return open.ok() ? (*open)->resumeScan(workerThreads) : Status(open.error());
}

Status RecoveryApi::cancelScan(std::string_view session) {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    return open.ok() ? (*open)->cancel(OperationKind::Scan) : Status(open.error());
}

Result<Progress> RecoveryApi::getProgress(std::string_view session) const {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    if (!open.ok()) {
        return open.error();
    }
    return (*open)->progress();
}

Result<Progress> RecoveryApi::waitForOperation(std::string_view session, std::chrono::milliseconds timeout) const {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    if (!open.ok()) {
        return open.error();
    }
    return (*open)->wait(timeout);
}

// ---------------------------------------------------------------------------
// Candidates
// ---------------------------------------------------------------------------

Result<CandidatePage> RecoveryApi::getCandidates(std::string_view session, const CandidateQuery& query) const {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    if (!open.ok()) {
        return open.error();
    }
    return (*open)->candidates(query);
}

Result<CandidateDetails> RecoveryApi::getCandidateDetails(std::string_view session, CandidateId candidate,
                                                          bool readMedia) const {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    if (!open.ok()) {
        return open.error();
    }
    return (*open)->candidateDetails(candidate, readMedia);
}

Result<std::vector<std::byte>> RecoveryApi::readPreview(std::string_view session, CandidateId candidate,
                                                        std::size_t preview, std::uint64_t offset,
                                                        std::uint64_t maxBytes) const {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    if (!open.ok()) {
        return open.error();
    }
    return (*open)->preview(candidate, preview, offset, maxBytes);
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

Result<RecoveryStart> RecoveryApi::recoverCandidate(std::string_view session, CandidateId candidate,
                                                    const RecoveryOptions& options) {
    const CandidateId one[] = {candidate};
    return recoverCandidates(session, one, options);
}

Result<RecoveryStart> RecoveryApi::recoverCandidates(std::string_view session,
                                                     std::span<const CandidateId> candidates,
                                                     const RecoveryOptions& options) {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    if (!open.ok()) {
        return open.error();
    }
    detail::RecoverySelection selection;
    selection.ids.assign(candidates.begin(), candidates.end());
    return (*open)->recover(selection, options);
}

Result<RecoveryStart> RecoveryApi::recoverAll(std::string_view session, const RecoveryOptions& options,
                                              const CandidateFilter& filter) {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    if (!open.ok()) {
        return open.error();
    }
    detail::RecoverySelection selection;
    selection.all = true;
    selection.filter = filter;
    return (*open)->recover(selection, options);
}

Status RecoveryApi::pauseRecovery(std::string_view session) {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    return open.ok() ? (*open)->pause(OperationKind::Recovery) : Status(open.error());
}

Status RecoveryApi::resumeRecovery(std::string_view session, std::uint32_t workerThreads) {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    return open.ok() ? (*open)->resumeRecovery(workerThreads) : Status(open.error());
}

Status RecoveryApi::cancelRecovery(std::string_view session) {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    return open.ok() ? (*open)->cancel(OperationKind::Recovery) : Status(open.error());
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

Status RecoveryApi::exportReport(std::string_view session, const std::filesystem::path& file,
                                 const ReportOptions& options) const {
    Result<std::shared_ptr<detail::OpenSession>> open = impl_->find(session);
    if (!open.ok()) {
        return open.error();
    }
    return (*open)->exportReport(file, options);
}

}  // namespace recovery::api
