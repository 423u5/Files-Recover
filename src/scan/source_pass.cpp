#include "source_pass.hpp"

#include "storage/bad_region.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace recovery::scan::detail {

namespace {

using carving::CarveOutcome;
using carving::FileCandidate;
using carving::SignatureHit;
using Clock = std::chrono::steady_clock;

// What the scanner thread, the workers and the pass's thread share.
struct Shared {
    std::mutex mutex;
    std::condition_variable changed;
    // Hits the scanner found that the pass has not taken yet.
    std::deque<SignatureHit> hits;
    std::size_t capacity = 0;
    // Every file start before this offset was examined by the scanner, and
    // its hits are queued (or taken).
    std::uint64_t scanned = 0;
    // The pass asks the scanner to end.
    bool stop = false;
    // The scanner has ended, with this report.
    bool finished = false;
    std::optional<Result<carving::ScanReport>> report;
    // Tasks queued or running.
    std::size_t inFlight = 0;
};

// What the workers prepare for one hit.
struct Work {
    std::optional<Result<CarveOutcome>> carve;
    std::optional<Result<Mp4HitWork>> mp4;
    bool done = false;
};

struct Entry {
    SignatureHit hit;
    // The stages the hit is for.
    bool general = false;
    bool mp4 = false;
    bool fragment = false;
    // A hit of a self-synchronizing format waits until the hits of its format
    // before it are committed: one of them may hold it (a frame of a stream).
    bool held = false;
    // Null while nothing is prepared.
    std::shared_ptr<Work> work;
};

class Pass {
public:
    Pass(const PassSetup& setup, PassState& state, const PassCheckpoint& checkpoint, const PassProgress& progress)
        : setup_(setup),
          state_(state),
          checkpoint_(checkpoint),
          progress_(progress),
          shared_(std::make_shared<Shared>()),
          carveOptions_(std::make_shared<const carving::CarveOptions>(setup.carving)) {}

    ~Pass() { stopScanner(); }
    Pass(const Pass&) = delete;
    Pass& operator=(const Pass&) = delete;
    Pass(Pass&&) = delete;
    Pass& operator=(Pass&&) = delete;

    Status run();

private:
    [[nodiscard]] Status prepareRegistry();
    void startScanner();
    void stopScanner();
    void admit(const SignatureHit& hit);
    void decide(Entry& entry, const std::set<const carving::IFileFormat*>& earlier);
    void dispatch(Entry& entry, bool carve, bool mp4);
    void releaseHeld();
    [[nodiscard]] bool frontGroupReady();
    [[nodiscard]] Status commit(Entry& entry);
    [[nodiscard]] std::uint64_t cut();
    [[nodiscard]] Status consistentPoint(bool final);
    // Waits for the tasks in flight, ends the scanner, adds its report.
    void wind();
    [[nodiscard]] Status fail(Status error, bool reportPoint);

    const PassSetup& setup_;
    PassState& state_;
    const PassCheckpoint& checkpoint_;
    const PassProgress& progress_;
    std::shared_ptr<Shared> shared_;
    std::shared_ptr<const carving::CarveOptions> carveOptions_;

    carving::FormatRegistry registry_;
    std::optional<carving::SignatureScanner> scanner_;
    std::thread scannerThread_;
    // The carving formats, the formats only fragment reconstruction scans
    // for, and the id of MP4 recovery's format.
    std::set<const carving::IFileFormat*> carving_;
    std::set<const carving::IFileFormat*> fragmentOnly_;
    std::string mp4Id_;

    std::deque<Entry> window_;
    std::vector<FileCandidate> carves_;
    std::uint64_t lastPointPosition_ = 0;
    Clock::time_point lastPointTime_;
    Clock::time_point lastProgress_;
    bool reportAdded_ = false;
};

Status Pass::prepareRegistry() {
    const bool carvingFormats = setup_.carve || setup_.fragments != nullptr;
    if (carvingFormats) {
        for (const std::shared_ptr<const carving::IFileFormat>& format : setup_.formats->formats()) {
            if (Status added = registry_.add(format); !added.ok()) {
                return added;
            }
            carving_.insert(format.get());
        }
    }
    if (setup_.mp4 != nullptr) {
        const carving::IFileFormat& own = setup_.mp4->format();
        mp4Id_ = own.descriptor().id;
        if (registry_.find(mp4Id_) == nullptr) {
            // The steps' own format, not owned by the registry: it outlives the pass.
            std::shared_ptr<const carving::IFileFormat> borrowed(std::shared_ptr<const carving::IFileFormat>{}, &own);
            if (Status added = registry_.add(borrowed); !added.ok()) {
                return added;
            }
        }
    }
    if (setup_.fragments != nullptr) {
        for (const std::shared_ptr<const carving::IFileFormat>& format : setup_.fragments->extraFormats()) {
            if (Status added = registry_.add(format); !added.ok()) {
                return added;
            }
            fragmentOnly_.insert(format.get());
        }
    }
    if (registry_.empty()) {
        return success();
    }
    Result<carving::SignatureScanner> scanner = carving::SignatureScanner::create(registry_);
    if (!scanner.ok()) {
        return scanner.error();
    }
    scanner_.emplace(std::move(*scanner));
    return success();
}

void Pass::startScanner() {
    shared_->capacity = std::max<std::size_t>(setup_.window, 64);
    shared_->scanned = state_.position;
    const std::uint64_t hits = state_.scan.hits;
    if (!scanner_.has_value() || hits >= setup_.maxHits) {
        // Nothing to scan for, or the hit limit was reached before.
        carving::ScanReport report;
        report.outcome = scanner_.has_value() ? carving::ScanOutcome::HitLimitReached : carving::ScanOutcome::Completed;
        report.startOffset = state_.position;
        report.endOffset = setup_.source->size();
        report.nextOffset = scanner_.has_value() ? state_.position : setup_.source->size();
        report.hitsPerFormat.assign(registry_.size(), 0);
        shared_->scanned = report.nextOffset;
        shared_->report.emplace(std::move(report));
        shared_->finished = true;
        return;
    }
    carving::ScanOptions options = setup_.scan;
    options.startOffset = state_.position;
    options.endOffset.reset();
    options.maxHits = setup_.maxHits - hits;
    options.progressInterval = std::chrono::milliseconds{0};
    std::shared_ptr<Shared> shared = shared_;
    options.onProgress = [shared](const carving::ScanProgress& progress) {
        {
            const std::lock_guard lock(shared->mutex);
            shared->scanned = std::max(shared->scanned, progress.position);
        }
        shared->changed.notify_all();
    };
    scannerThread_ = std::thread([this, shared, options = std::move(options)] {
        const carving::HitSink sink = [&shared](const SignatureHit& hit) -> Status {
            std::unique_lock lock(shared->mutex);
            shared->changed.wait(lock, [&] { return shared->stop || shared->hits.size() < shared->capacity; });
            if (shared->stop) {
                return makeError(ErrorCode::Cancelled, "the source pass ended");
            }
            shared->hits.push_back(hit);
            lock.unlock();
            shared->changed.notify_all();
            return success();
        };
        std::optional<Result<carving::ScanReport>> report;
        try {
            report.emplace(scanner_->scan(*setup_.source, sink, options));
        } catch (const std::exception& error) {
            report.emplace(makeError(ErrorCode::InternalError, std::string("the source pass failed: ") + error.what()));
        }
        {
            const std::lock_guard lock(shared->mutex);
            shared->report = std::move(report);
            shared->finished = true;
        }
        shared->changed.notify_all();
    });
}

void Pass::stopScanner() {
    {
        const std::lock_guard lock(shared_->mutex);
        shared_->stop = true;
    }
    shared_->changed.notify_all();
    if (scannerThread_.joinable()) {
        scannerThread_.join();
    }
}

void Pass::admit(const SignatureHit& hit) {
    Entry entry;
    entry.hit = hit;
    entry.general = setup_.carve && carving_.contains(hit.format);
    entry.mp4 = setup_.mp4 != nullptr && hit.format->descriptor().id == mp4Id_;
    entry.fragment =
        setup_.fragments != nullptr && (carving_.contains(hit.format) || fragmentOnly_.contains(hit.format));
    std::set<const carving::IFileFormat*> earlier;
    for (const Entry& before : window_) {
        earlier.insert(before.hit.format);
    }
    decide(entry, earlier);
    window_.push_back(std::move(entry));
}

// Prepares what the stages will need for the hit, as far as the commits so
// far tell (a commit before it may still make a stage skip it; commit()
// decides, and prepares what is missing then).
void Pass::decide(Entry& entry, const std::set<const carving::IFileFormat*>& earlier) {
    const SignatureHit& hit = entry.hit;
    const bool general = entry.general && !state_.skip.skips(hit);
    const bool fragment = entry.fragment && !fragmentOnly_.contains(hit.format) && setup_.fragments->wants(hit);
    const bool mp4 = entry.mp4 && !setup_.mp4->skips(hit.fileOffset);
    entry.held = false;
    if (!general && !fragment && !mp4) {
        return;
    }
    if (hit.format->descriptor().selfSynchronizing && earlier.contains(hit.format)) {
        entry.held = true;
        return;
    }
    dispatch(entry, general || fragment, mp4);
}

void Pass::dispatch(Entry& entry, bool carve, bool mp4) {
    auto work = std::make_shared<Work>();
    entry.work = work;
    {
        const std::lock_guard lock(shared_->mutex);
        ++shared_->inFlight;
    }
    const SignatureHit hit = entry.hit;
    storage::IStorageSource* source = setup_.source;
    Mp4RecoverySteps* steps = mp4 ? setup_.mp4 : nullptr;
    const std::shared_ptr<Shared> shared = shared_;
    const std::shared_ptr<const carving::CarveOptions> options = carveOptions_;
    const auto task = [work, hit, source, steps, carve, shared, options] {
        try {
            if (carve) {
                carving::FileCarver carver(*source, *options);
                work->carve.emplace(carver.carve(hit));
            }
            if (steps != nullptr) {
                work->mp4.emplace(steps->prepare(hit));
            }
        } catch (const std::exception& error) {
            work->carve.emplace(makeError(ErrorCode::InternalError, std::string("a carve failed: ") + error.what()));
        }
        {
            const std::lock_guard lock(shared->mutex);
            work->done = true;
            --shared->inFlight;
        }
        shared->changed.notify_all();
    };
    if (Status submitted = setup_.pool->submit(task); !submitted.ok()) {
        // The pool is shut down: commit() prepares the hit itself.
        entry.work.reset();
        const std::lock_guard lock(shared_->mutex);
        --shared_->inFlight;
    }
}

// Held hits whose format has no hit before them any more are decided again.
void Pass::releaseHeld() {
    std::set<const carving::IFileFormat*> earlier;
    for (Entry& entry : window_) {
        if (entry.held && !earlier.contains(entry.hit.format)) {
            decide(entry, earlier);
        }
        earlier.insert(entry.hit.format);
    }
}

// The hits at the front's offset are all admitted, and their work is done.
bool Pass::frontGroupReady() {
    if (window_.empty()) {
        return false;
    }
    const std::uint64_t offset = window_.front().hit.fileOffset;
    bool complete = false;
    for (const Entry& entry : window_) {
        if (entry.hit.fileOffset != offset) {
            complete = true;
            break;
        }
    }
    const std::lock_guard lock(shared_->mutex);
    if (!complete) {
        // Every hit at the offset is admitted once the scanner has moved past it.
        complete = (!shared_->hits.empty() && shared_->hits.front().fileOffset > offset) ||
                   (shared_->hits.empty() && (shared_->finished || shared_->scanned > offset));
    }
    if (!complete) {
        return false;
    }
    for (const Entry& entry : window_) {
        if (entry.hit.fileOffset != offset) {
            break;
        }
        if (entry.held || (entry.work != nullptr && !entry.work->done)) {
            return false;
        }
    }
    return true;
}

Status Pass::commit(Entry& entry) {
    const SignatureHit& hit = entry.hit;
    const std::shared_ptr<Work> work = entry.work;
    // The carve with validation, as carving and fragment reconstruction both
    // make it: prepared by a worker, or made here when a stage needs it.
    CarveOutcome* prepared = nullptr;
    if (work != nullptr && work->carve.has_value() && work->carve->ok()) {
        prepared = &work->carve->value();
    }
    std::optional<CarveOutcome> made;
    if (entry.general) {
        if (state_.skip.skips(hit)) {
            ++state_.carving.skippedInsideCandidates;
        } else {
            if (prepared == nullptr) {
                carving::FileCarver carver(*setup_.source, *carveOptions_);
                Result<CarveOutcome> outcome = carver.carve(hit);
                if (!outcome.ok()) {
                    return outcome.error();
                }
                made.emplace(std::move(*outcome));
                prepared = &*made;
            }
            if (const carving::CarveRejection* rejection = std::get_if<carving::CarveRejection>(prepared)) {
                ++state_.carving.rejected[static_cast<std::size_t>(rejection->reason)];
            } else {
                FileCandidate candidate = std::get<FileCandidate>(*prepared);
                candidate.id = carving::FileCandidateId{state_.nextCarveId++};
                ++state_.carving.candidates;
                ++state_.carving.validation[static_cast<std::size_t>(candidate.validation.status)];
                state_.skip.record(hit, candidate);
                carves_.push_back(std::move(candidate));
            }
        }
    }
    if (entry.mp4) {
        Mp4HitWork* mp4Work = nullptr;
        if (work != nullptr && work->mp4.has_value() && work->mp4->ok()) {
            mp4Work = &work->mp4->value();
        }
        if (Status committed = setup_.mp4->commit(hit, mp4Work); !committed.ok()) {
            return committed;
        }
    }
    if (entry.fragment) {
        if (Status committed = setup_.fragments->commit(hit, prepared); !committed.ok()) {
            return committed;
        }
    }
    ++state_.scan.hits;
    if (hit.formatIndex < state_.scan.hitsPerFormat.size()) {
        ++state_.scan.hitsPerFormat[hit.formatIndex];
    }
    return success();
}

std::uint64_t Pass::cut() {
    if (!window_.empty()) {
        return window_.front().hit.fileOffset;
    }
    const std::lock_guard lock(shared_->mutex);
    if (!shared_->hits.empty()) {
        return shared_->hits.front().fileOffset;
    }
    return shared_->scanned;
}

Status Pass::consistentPoint(bool final) {
    state_.position = std::max(state_.position, cut());
    lastPointPosition_ = state_.position;
    lastPointTime_ = Clock::now();
    return checkpoint_(state_, std::exchange(carves_, {}), final);
}

void Pass::wind() {
    stopScanner();
    {
        std::unique_lock lock(shared_->mutex);
        shared_->changed.wait(lock, [&] { return shared_->inFlight == 0; });
    }
    if (reportAdded_ || !shared_->report.has_value() || !shared_->report->ok()) {
        return;
    }
    reportAdded_ = true;
    const carving::ScanReport& report = shared_->report->value();
    carving::ScanReport& total = state_.scan;
    total.endOffset = report.endOffset;
    total.bytesRead += report.bytesRead;
    total.elapsed += report.elapsed;
    storage::BadRegionMap unreadable;
    for (const storage::BadRegion& region : total.unreadableRegions) {
        (void)unreadable.add(region);
    }
    for (const storage::BadRegion& region : report.unreadableRegions) {
        (void)unreadable.add(region);
    }
    total.unreadableRegions = unreadable.regions();
    total.unreadableBytes = unreadable.totalBytes();
}

Status Pass::fail(Status error, bool reportPoint) {
    wind();
    if (reportPoint) {
        if (Status point = consistentPoint(false); !point.ok()) {
            return point;
        }
    }
    return error;
}

Status Pass::run() {
    if (Status prepared = prepareRegistry(); !prepared.ok()) {
        return prepared;
    }
    if (state_.scan.hitsPerFormat.size() < registry_.size()) {
        state_.scan.hitsPerFormat.resize(registry_.size(), 0);
    }
    lastPointPosition_ = state_.position;
    lastPointTime_ = Clock::now();
    lastProgress_ = lastPointTime_;
    startScanner();
    const std::size_t capacity = std::max<std::size_t>(setup_.window, 64);
    for (;;) {
        if (setup_.control.isCancellationRequested()) {
            return fail(makeError(ErrorCode::Cancelled, "the scan was cancelled"), true);
        }
        bool progressed = false;
        // Take the hits the scanner found, as far as the window has room.
        while (window_.size() < capacity) {
            std::optional<SignatureHit> hit;
            {
                const std::lock_guard lock(shared_->mutex);
                if (!shared_->hits.empty()) {
                    hit = shared_->hits.front();
                    shared_->hits.pop_front();
                }
            }
            if (!hit.has_value()) {
                break;
            }
            shared_->changed.notify_all();  // room for the scanner
            admit(*hit);
            progressed = true;
        }
        releaseHeld();
        // Commit, offset by offset, in source order.
        while (frontGroupReady()) {
            const std::uint64_t offset = window_.front().hit.fileOffset;
            while (!window_.empty() && window_.front().hit.fileOffset == offset) {
                if (Status committed = commit(window_.front()); !committed.ok()) {
                    return fail(committed, true);
                }
                window_.pop_front();
            }
            progressed = true;
            releaseHeld();
        }
        // The end of the pass: the scanner has ended and every hit is committed.
        bool finished = false;
        {
            const std::lock_guard lock(shared_->mutex);
            finished = shared_->finished && shared_->hits.empty() && window_.empty();
        }
        if (finished) {
            const Result<carving::ScanReport>& report = *shared_->report;
            if (!report.ok()) {
                return fail(report.error(), true);
            }
            if (report->outcome == carving::ScanOutcome::Cancelled) {
                return fail(makeError(ErrorCode::Cancelled, "the scan was cancelled"), true);
            }
            wind();
            state_.scan.outcome = report->outcome;
            state_.scan.nextOffset = report->nextOffset;
            state_.position = std::max(state_.position, report->nextOffset);
            lastPointPosition_ = state_.position;
            return checkpoint_(state_, std::exchange(carves_, {}), true);
        }
        const auto now = Clock::now();
        if (progress_ && now - lastProgress_ >= setup_.progressInterval) {
            lastProgress_ = now;
            progress_(cut());
        }
        // A consistent point when one is due, and before a pause.
        const std::uint64_t position = cut();
        const bool due = position - std::min(position, lastPointPosition_) >= setup_.checkpointBytes ||
                         (now - lastPointTime_ >= setup_.checkpointInterval &&
                          (position > lastPointPosition_ || !carves_.empty()));
        if (due || setup_.control.isPauseRequested()) {
            if (Status point = consistentPoint(false); !point.ok()) {
                return fail(point, false);
            }
        }
        if (setup_.control.isPauseRequested()) {
            const Status waited =
                setup_.waitWhilePaused ? setup_.waitWhilePaused() : setup_.control.waitWhilePaused();
            if (!waited.ok()) {
                return fail(waited, true);
            }
            continue;
        }
        if (!progressed) {
            std::unique_lock lock(shared_->mutex);
            shared_->changed.wait_for(lock, std::chrono::milliseconds{50});
        }
    }
}

}  // namespace

Status runSourcePass(const PassSetup& setup, PassState& state, const PassCheckpoint& checkpoint,
                     const PassProgress& progress) {
    Pass pass(setup, state, checkpoint, progress);
    return pass.run();
}

}  // namespace recovery::scan::detail
