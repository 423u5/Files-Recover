#include "scan/recovery_job.hpp"

#include "recovery/config.hpp"
#include "recovery/output_names.hpp"
#include "recovery/text.hpp"
#include "recovery/worker_pool.hpp"
#include "scan/scan_source.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <set>
#include <string_view>
#include <utility>

namespace recovery::scan {

namespace {

using Clock = std::chrono::steady_clock;
using diagnostics::field;
using diagnostics::LogLevel;

constexpr std::string_view kComponent = "recovery_job";
// Units of a name's stem that keys compare: a name too long for Windows has
// its stem shortened when a " (n)" suffix is added.
constexpr std::size_t kKeyStem = 200;

// Windows compares names without case. ASCII letters are folded; anything
// beyond ASCII counts as one same character, so names that might be equal
// without case are taken as equal (more files are ordered, never fewer).
wchar_t fold(wchar_t c) noexcept {
    if (c >= L'A' && c <= L'Z') {
        return static_cast<wchar_t>(c - L'A' + L'a');
    }
    return c >= 0x80 ? L'?' : c;
}

// A safe name as a key: folded, without a " (n)" suffix, its stem cut short.
std::wstring nameKey(std::wstring_view name) {
    std::wstring_view stem = name;
    std::wstring_view extension;
    if (const std::size_t dot = name.rfind(L'.'); dot != std::wstring_view::npos && dot > 0) {
        stem = name.substr(0, dot);
        extension = name.substr(dot);
    }
    if (stem.size() >= 4 && stem.back() == L')') {
        const std::size_t open = stem.rfind(L" (");
        if (open != std::wstring_view::npos && open + 3 < stem.size()) {
            const std::wstring_view digits = stem.substr(open + 2, stem.size() - open - 3);
            if (std::all_of(digits.begin(), digits.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
                stem = stem.substr(0, open);
            }
        }
    }
    std::wstring key;
    for (const wchar_t c : stem.substr(0, std::min(stem.size(), kKeyStem))) {
        key.push_back(fold(c));
    }
    for (const wchar_t c : extension) {
        key.push_back(fold(c));
    }
    return key;
}

// The directory part of a candidate's original path, as RecoveryWriter takes
// it: the name is removed as a whole, since a damaged name may hold '/'.
std::string_view parentPathOf(const RecoveryCandidate& candidate) {
    const std::string_view path = candidate.filesystemEvidence.path;
    const std::string_view name = candidate.filename;
    if (path.size() > name.size() && path.ends_with(name) && path[path.size() - name.size() - 1] == '/') {
        return path.substr(0, path.size() - name.size() - 1);
    }
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

// Where a candidate's file goes, as keys: each directory on the way, and the file.
struct NameKeys {
    std::vector<std::wstring> directories;
    std::wstring file;
};

NameKeys keysOf(const RecoveryCandidate& candidate, bool preserveDirectories) {
    NameKeys keys;
    std::wstring prefix;
    if (preserveDirectories) {
        const std::string_view parent = parentPathOf(candidate);
        std::size_t start = 0;
        while (start <= parent.size()) {
            std::size_t slash = parent.find('/', start);
            if (slash == std::string_view::npos) {
                slash = parent.size();
            }
            const std::string_view component = parent.substr(start, slash - start);
            start = slash + 1;
            if (component.empty()) {
                continue;
            }
            prefix += L"/" + nameKey(safeFileName(component));
            keys.directories.push_back(prefix);
        }
    }
    keys.file = prefix + L"/" + nameKey(safeFileName(candidate.filename));
    return keys;
}

// Two files whose names could meet: the same name, or one's name that is a
// directory on the other's way, at the same place.
bool conflict(const NameKeys& a, const NameKeys& b) {
    if (a.file == b.file) {
        return true;
    }
    return std::find(b.directories.begin(), b.directories.end(), a.file) != b.directories.end() ||
           std::find(a.directories.begin(), a.directories.end(), b.file) != a.directories.end();
}

}  // namespace

// ---------------------------------------------------------------------------
// RecoveryJobCheckpoint
// ---------------------------------------------------------------------------

Status RecoveryJobCheckpoint::apply(const RecoveryJobUpdate& update) {
    const auto refused = [](std::string message) -> Status {
        return makeError(ErrorCode::InvalidInput, "recovery checkpoint: " + std::move(message));
    };
    if (update.sequence != sequence_ + 1) {
        return refused("update " + std::to_string(update.sequence) + " does not follow update " +
                       std::to_string(sequence_));
    }
    if ((sequence_ == 0) != update.destination.has_value()) {
        return refused("a job's first update, and only it, names the destination");
    }
    if (complete_) {
        return refused("the job is complete");
    }
    std::set<std::uint64_t> seen;
    for (const RecoveredItem& item : update.items) {
        if (items_.contains(item.candidate.value()) || !seen.insert(item.candidate.value()).second) {
            return refused("candidate " + std::to_string(item.candidate.value()) + " is done already");
        }
        if (item.file.has_value() == item.error.has_value()) {
            return refused("a candidate is either written or not");
        }
    }
    sequence_ = update.sequence;
    if (update.destination.has_value()) {
        destination_ = update.destination;
    }
    for (const RecoveredItem& item : update.items) {
        items_.emplace(item.candidate.value(), item);
    }
    complete_ = update.complete;
    metrics_ = update.metrics;
    return success();
}

// ---------------------------------------------------------------------------
// One run
// ---------------------------------------------------------------------------

struct RecoveryJob::Run {
    struct Entry {
        std::size_t index = 0;
        NameKeys keys;
        bool running = false;
        bool done = false;
        // Set when done, unless the write was cancelled.
        std::optional<RecoveredItem> result;
    };

    Run(RecoveryJob& owner, std::span<const evaluation::EvaluatedCandidate> candidates,
        const RecoveryJobUpdateSink& sink)
        : owner(owner), candidates(candidates), sink(sink), options(owner.options_) {}
    ~Run() {
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] { return running == 0; });
        }
        const std::lock_guard lock(owner.progressMutex_);
        owner.liveSource_ = nullptr;
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;
    Run(Run&&) = delete;
    Run& operator=(Run&&) = delete;

    [[nodiscard]] Status prepare(const RecoveryJobCheckpoint* resume);
    [[nodiscard]] Result<RecoveryJobSummary> execute();
    [[nodiscard]] Result<std::unique_ptr<RecoveryWriter>> takeWriter();
    void returnWriter(std::unique_ptr<RecoveryWriter> writer);
    void dispatch(const std::shared_ptr<Entry>& entry);
    // Takes the files done out of the window, whatever their order; true when there were some.
    bool collect();
    [[nodiscard]] Status emit(bool complete);
    [[nodiscard]] RecoveryJobMetrics metrics();
    void progress(bool force);

    RecoveryJob& owner;
    std::span<const evaluation::EvaluatedCandidate> candidates;
    const RecoveryJobUpdateSink& sink;
    const RecoveryJobOptions& options;
    RecoveryWriterOptions writerOptions;
    std::string destination;
    std::unique_ptr<ScanSource> source;
    std::size_t window = 1;

    std::mutex mutex;
    std::condition_variable changed;
    std::size_t running = 0;
    std::vector<std::unique_ptr<RecoveryWriter>> writers;

    std::vector<std::size_t> todo;
    std::size_t next = 0;
    std::deque<std::shared_ptr<Entry>> entries;
    std::vector<RecoveredItem> batch;
    Clock::time_point batchSince = Clock::now();

    RecoveryJobMetrics base;
    std::uint64_t done = 0;
    std::uint64_t recovered = 0;
    std::uint64_t failed = 0;
    std::uint64_t bytesRecovered = 0;
    std::uint64_t unreadable = 0;
    Clock::time_point started = Clock::now();
    Clock::duration pausedFor{0};
    bool paused = false;
    Clock::time_point lastProgress = Clock::now();
    // Last, so that it is destroyed first: its workers use everything above.
    std::unique_ptr<WorkerPool> pool;
};

Status RecoveryJob::Run::prepare(const RecoveryJobCheckpoint* resume) {
    if (!owner.source_.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "recovery job: the source is not open");
    }
    if (options.workerThreads > EngineConfig::kMaxWorkerThreads) {
        return makeError(ErrorCode::InvalidInput, "recovery job: too many workers");
    }
    if (options.checkpointItems == 0) {
        return makeError(ErrorCode::InvalidInput, "recovery job: checkpoints need a number of items");
    }
    std::set<std::uint64_t> ids;
    for (const evaluation::EvaluatedCandidate& candidate : candidates) {
        if (!ids.insert(candidate.id.value()).second) {
            return makeError(ErrorCode::InvalidInput,
                             "recovery job: candidate " + std::to_string(candidate.id.value()) + " is given twice");
        }
    }
    std::error_code ec;
    destination = toUtf8(std::filesystem::absolute(owner.destination_, ec).lexically_normal());
    if (resume != nullptr && !resume->empty()) {
        if (resume->destination() != destination) {
            return makeError(ErrorCode::InvalidInput,
                             "recovery job: the checkpoint is of a job to another destination");
        }
        for (const auto& [id, item] : resume->items()) {
            if (!ids.contains(id)) {
                return makeError(ErrorCode::InvalidInput,
                                 "recovery job: the checkpoint names a candidate that is not given");
            }
        }
        if (resume != &owner.checkpoint_) {
            owner.checkpoint_ = *resume;
        }
    } else {
        owner.checkpoint_ = RecoveryJobCheckpoint{};
    }
    base = owner.checkpoint_.metrics();
    for (const auto& [id, item] : owner.checkpoint_.items()) {
        ++done;
        if (item.file.has_value()) {
            ++recovered;
            bytesRecovered += item.file->report.outputSize;
            unreadable += item.file->report.unreadableBytes;
        } else {
            ++failed;
        }
    }
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!owner.checkpoint_.done(candidates[i].id)) {
            todo.push_back(i);
        }
    }

    const std::uint32_t threads = effectiveWorkerThreads(EngineConfig{IoConfig{}, options.workerThreads});
    window = options.window != 0 ? options.window : std::size_t{2} * threads;
    Result<std::unique_ptr<WorkerPool>> created = WorkerPool::create(threads, std::max<std::size_t>(64, window));
    if (!created.ok()) {
        return created.error();
    }
    pool = std::move(created).value();
    ScanSourceOptions sourceOptions;
    sourceOptions.cacheBlocks = 0;
    sourceOptions.blockSize = owner.source_.sectorSize() > 0 ? owner.source_.sectorSize() : 512;
    sourceOptions.control = options.control;
    source = std::make_unique<ScanSource>(owner.source_, sourceOptions);
    if (Status opened = source->open(); !opened.ok()) {
        return opened;
    }
    {
        const std::lock_guard lock(owner.progressMutex_);
        owner.liveSource_ = source.get();
    }
    writerOptions = options.writer;
    writerOptions.reconstruction.cancellation = options.control.token();
    if (writerOptions.logger == nullptr) {
        writerOptions.logger = options.logger;
    }
    // The destination is checked now, before anything is written.
    if (!todo.empty()) {
        Result<std::unique_ptr<RecoveryWriter>> first = takeWriter();
        if (!first.ok()) {
            return first.error();
        }
        returnWriter(std::move(*first));
    }
    return success();
}

Result<std::unique_ptr<RecoveryWriter>> RecoveryJob::Run::takeWriter() {
    {
        const std::lock_guard lock(mutex);
        if (!writers.empty()) {
            std::unique_ptr<RecoveryWriter> writer = std::move(writers.back());
            writers.pop_back();
            return writer;
        }
    }
    Result<RecoveryWriter> created = RecoveryWriter::create(*source, owner.destination_, writerOptions);
    if (!created.ok()) {
        return created.error();
    }
    return std::make_unique<RecoveryWriter>(std::move(created).value());
}

void RecoveryJob::Run::returnWriter(std::unique_ptr<RecoveryWriter> writer) {
    const std::lock_guard lock(mutex);
    writers.push_back(std::move(writer));
}

void RecoveryJob::Run::dispatch(const std::shared_ptr<Entry>& entry) {
    entry->running = true;
    {
        const std::lock_guard lock(mutex);
        ++running;
    }
    const auto task = [this, entry] {
        std::optional<RecoveredItem> result;
        const evaluation::EvaluatedCandidate& candidate = candidates[entry->index];
        try {
            RecoveredItem item;
            item.candidate = candidate.id;
            Result<std::unique_ptr<RecoveryWriter>> writer = takeWriter();
            if (!writer.ok()) {
                item.error = writer.error();
                result = std::move(item);
            } else {
                FileCreatedCallback onCreated;
                if (options.onFileCreated) {
                    onCreated = [this, &candidate](const std::filesystem::path& path) {
                        return options.onFileCreated(candidate.id, path);
                    };
                }
                Result<RecoveredFile> written = (*writer)->recover(candidate.data, onCreated);
                returnWriter(std::move(*writer));
                if (written.ok()) {
                    item.file = std::move(*written);
                    result = std::move(item);
                } else if (written.error().code != ErrorCode::Cancelled) {
                    item.error = written.error();
                    result = std::move(item);
                }
            }
        } catch (const std::exception& error) {
            RecoveredItem item;
            item.candidate = candidate.id;
            item.error = makeError(ErrorCode::InternalError, std::string("writing failed: ") + error.what());
            result = std::move(item);
        }
        {
            const std::lock_guard lock(mutex);
            entry->result = std::move(result);
            entry->done = true;
            --running;
        }
        changed.notify_all();
    };
    if (Status submitted = pool->submit(task); !submitted.ok()) {
        const std::lock_guard lock(mutex);
        --running;
        entry->done = true;
        RecoveredItem item;
        item.candidate = candidates[entry->index].id;
        item.error = submitted.error();
        entry->result = std::move(item);
    }
}

bool RecoveryJob::Run::collect() {
    bool any = false;
    const std::lock_guard lock(mutex);
    for (auto it = entries.begin(); it != entries.end();) {
        if (!(*it)->done) {
            ++it;
            continue;
        }
        if ((*it)->result.has_value()) {
            const RecoveredItem& item = *(*it)->result;
            ++done;
            if (item.file.has_value()) {
                ++recovered;
                bytesRecovered += item.file->report.outputSize;
                unreadable += item.file->report.unreadableBytes;
            } else {
                ++failed;
            }
            batch.push_back(std::move(*(*it)->result));
        }
        it = entries.erase(it);
        any = true;
    }
    return any;
}

RecoveryJobMetrics RecoveryJob::Run::metrics() {
    RecoveryJobMetrics metrics;
    metrics.files = candidates.size();
    metrics.recoveredFiles = recovered;
    metrics.failedFiles = failed;
    metrics.bytesRecovered = bytesRecovered;
    metrics.unreadableBytes = unreadable;
    const ScanSourceStats stats = source != nullptr ? source->stats() : ScanSourceStats{};
    metrics.bytesRead = base.bytesRead + stats.bytesRead;
    const auto runningMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started - pausedFor);
    metrics.elapsed = base.elapsed + runningMs;
    const std::uint64_t writtenNow = bytesRecovered - std::min(bytesRecovered, base.bytesRecovered);
    metrics.speed = runningMs.count() > 0 ? writtenNow * 1000 / static_cast<std::uint64_t>(runningMs.count()) : 0;
    return metrics;
}

void RecoveryJob::Run::progress(bool force) {
    const auto now = Clock::now();
    if (!force && now - lastProgress < options.progressInterval) {
        const std::lock_guard lock(owner.progressMutex_);
        owner.progress_.done = done;
        owner.progress_.total = candidates.size();
        owner.progress_.paused = paused;
        return;
    }
    lastProgress = now;
    RecoveryJobProgress snapshot;
    snapshot.done = done;
    snapshot.total = candidates.size();
    snapshot.paused = paused;
    snapshot.metrics = metrics();
    {
        const std::lock_guard lock(owner.progressMutex_);
        owner.progress_ = snapshot;
    }
    if (options.onProgress) {
        try {
            options.onProgress(snapshot);
        } catch (...) {
            if (options.logger != nullptr) {
                options.logger->log(LogLevel::Warning, kComponent, "progress callback threw an exception", {});
            }
        }
    }
}

Status RecoveryJob::Run::emit(bool complete) {
    RecoveryJobUpdate update;
    update.sequence = owner.checkpoint_.sequence() + 1;
    if (owner.checkpoint_.empty()) {
        update.destination = destination;
    }
    update.items = std::exchange(batch, {});
    update.complete = complete;
    update.metrics = metrics();
    batchSince = Clock::now();
    if (sink) {
        if (Status sent = sink(update); !sent.ok()) {
            return sent;
        }
    }
    if (Status applied = owner.checkpoint_.apply(update); !applied.ok()) {
        return makeError(ErrorCode::InternalError, "recovery job: an update its own checkpoint refuses: " +
                                                       applied.error().message);
    }
    progress(complete);
    return success();
}

Result<RecoveryJobSummary> RecoveryJob::Run::execute() {
    started = Clock::now();
    if (options.logger != nullptr) {
        options.logger->log(LogLevel::Info, kComponent, "recovery job started",
                            {field("destination", destination), field("candidates", candidates.size()),
                             field("to_write", todo.size()), field("workers", pool->threadCount())});
    }
    progress(true);
    Status outcome = success();
    bool cancelled = false;
    for (;;) {
        cancelled = cancelled || options.control.isCancellationRequested();
        bool progressed = false;
        if (!cancelled) {
            while (entries.size() < window && next < todo.size()) {
                auto entry = std::make_shared<Entry>();
                entry->index = todo[next++];
                entry->keys = keysOf(candidates[entry->index].data, writerOptions.preserveDirectories);
                entries.push_back(std::move(entry));
                progressed = true;
            }
            // A file is written once no earlier file not written yet could take its name.
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const std::shared_ptr<Entry>& entry = entries[i];
                if (entry->running) {
                    continue;
                }
                bool blocked = false;
                for (std::size_t j = 0; j < i && !blocked; ++j) {
                    bool earlierDone = false;
                    {
                        const std::lock_guard lock(mutex);
                        earlierDone = entries[j]->done;
                    }
                    blocked = !earlierDone && conflict(entries[j]->keys, entry->keys);
                }
                if (!blocked) {
                    dispatch(entry);
                    progressed = true;
                }
            }
        }
        progressed = collect() || progressed;
        if (cancelled) {
            // Files not started yet are not written in this run: a resumed run writes them.
            const auto before = entries.size();
            std::erase_if(entries, [](const std::shared_ptr<Entry>& entry) { return !entry->running; });
            progressed = progressed || entries.size() != before;
        }
        if (progressed) {
            progress(false);
        }
        const bool finished = entries.empty() && (cancelled || next >= todo.size());
        if (finished) {
            break;
        }
        const bool due = batch.size() >= options.checkpointItems ||
                         (!batch.empty() && Clock::now() - batchSince >= options.checkpointInterval);
        if (due) {
            if (Status sent = emit(false); !sent.ok()) {
                outcome = sent;
                cancelled = true;
                break;
            }
        }
        if (!cancelled && options.control.isPauseRequested()) {
            if (!batch.empty()) {
                if (Status sent = emit(false); !sent.ok()) {
                    outcome = sent;
                    cancelled = true;
                    break;
                }
            }
            paused = true;
            progress(true);
            const auto since = Clock::now();
            const Status waited = options.control.waitWhilePaused();
            pausedFor += Clock::now() - since;
            paused = false;
            progress(true);
            if (!waited.ok()) {
                cancelled = true;
            }
            continue;
        }
        if (!progressed) {
            std::unique_lock lock(mutex);
            changed.wait_for(lock, std::chrono::milliseconds{50});
        }
    }
    // Wait for the files being written, then hand out what is done.
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return running == 0; });
    }
    (void)collect();
    entries.clear();
    if (!outcome.ok()) {
        return outcome.error();
    }
    const bool complete = !cancelled && next >= todo.size();
    if (!batch.empty() || complete) {
        if (Status sent = emit(complete); !sent.ok()) {
            return sent.error();
        }
    }
    RecoveryJobSummary summary;
    summary.outcome = complete ? RecoveryJobOutcome::Completed : RecoveryJobOutcome::Cancelled;
    summary.metrics = metrics();
    if (options.logger != nullptr) {
        options.logger->log(LogLevel::Info, kComponent, "recovery job ended",
                            {field("outcome", complete ? "completed" : "cancelled"),
                             field("recovered", summary.metrics.recoveredFiles),
                             field("failed", summary.metrics.failedFiles),
                             field("bytes", summary.metrics.bytesRecovered)});
    }
    return summary;
}

// ---------------------------------------------------------------------------
// RecoveryJob
// ---------------------------------------------------------------------------

RecoveryJob::RecoveryJob(storage::IStorageSource& source, std::filesystem::path destination,
                         RecoveryJobOptions options)
    : source_(source), destination_(std::move(destination)), options_(std::move(options)) {}

RecoveryJob::~RecoveryJob() = default;

RecoveryJobProgress RecoveryJob::progress() const {
    const std::lock_guard lock(progressMutex_);
    RecoveryJobProgress snapshot = progress_;
    if (!snapshot.paused && liveSource_ != nullptr && options_.control.isPauseRequested() &&
        liveSource_->activeReads() == 0) {
        snapshot.paused = true;
    }
    return snapshot;
}

Result<RecoveryJobSummary> RecoveryJob::run(std::span<const evaluation::EvaluatedCandidate> candidates,
                                            const RecoveryJobUpdateSink& sink, const RecoveryJobCheckpoint* resume) {
    if (running_.exchange(true)) {
        return makeError(ErrorCode::InvalidInput, "recovery job: the job is running already");
    }
    struct Done {
        std::atomic<bool>& flag;
        ~Done() { flag = false; }
    } finished{running_};
    if (resume != nullptr && resume->complete()) {
        RecoveryJobSummary summary;
        summary.metrics = resume->metrics();
        if (resume != &checkpoint_) {
            checkpoint_ = *resume;
        }
        return summary;
    }
    Run run(*this, candidates, sink);
    if (Status prepared = run.prepare(resume); !prepared.ok()) {
        return prepared.error();
    }
    return run.execute();
}

}  // namespace recovery::scan
