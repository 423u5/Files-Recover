#include "scan/scan_coordinator.hpp"

#include "carving/content_reader.hpp"
#include "carving/file_carver.hpp"
#include "evaluation/candidate_evaluation.hpp"
#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/crc32.hpp"
#include "recovery/ordered_work.hpp"
#include "recovery/text.hpp"
#include "recovery/version.hpp"
#include "recovery/worker_pool.hpp"
#include "scan/scan_source.hpp"
#include "source_pass.hpp"

#include <algorithm>
#include <bit>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace recovery::scan {

namespace {

using Clock = std::chrono::steady_clock;
using diagnostics::field;
using diagnostics::LogLevel;

constexpr std::string_view kComponent = "scan";

std::string yesNo(bool value) {
    return value ? "yes" : "no";
}

// The settings that decide what a scan finds, as text.
std::string describe(const ScanConfiguration& configuration) {
    std::string text;
    const auto add = [&](std::string_view key, const std::string& value) {
        text += key;
        text += '=';
        text += value;
        text += '\n';
    };
    add("mode", std::string(toString(configuration.mode)));
    add("carving", yesNo(configuration.carving));
    add("mp4", yesNo(configuration.mp4));
    add("fragments", yesNo(configuration.fragments));
    add("include_active", yesNo(configuration.includeActive));
    add("include_deleted", yesNo(configuration.includeDeleted));
    add("alignment", std::to_string(configuration.alignment));
    add("max_hits", std::to_string(configuration.maxHits));
    add("sector_retries", std::to_string(configuration.sectorRetryCount));
    // The known bad regions: how many, how many bytes, and a CRC-32 of the list.
    std::vector<std::byte> list;
    std::uint64_t bytes = 0;
    for (const storage::BadRegion& region : configuration.knownBadRegions) {
        for (const std::uint64_t value : {region.offset, region.length, std::uint64_t{region.errorCode}}) {
            for (int shift = 0; shift < 64; shift += 8) {
                list.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
            }
        }
        bytes += region.length;
    }
    add("known_bad_regions", std::to_string(configuration.knownBadRegions.size()) + "/" + std::to_string(bytes) +
                                 "/" + std::to_string(crc32(list)));
    add("media", yesNo(configuration.media));
    add("playability", yesNo(configuration.playability != nullptr));
    add("sha256", yesNo(configuration.sha256));
    add("preliminary_hash", yesNo(configuration.preliminaryHash));
    return text;
}

// The volumes of a partition table: its partitions, or the whole device.
std::vector<VolumeRecord> planVolumes(const partition::PartitionTable& table, std::uint64_t sourceSize) {
    std::vector<VolumeRecord> plan;
    if (table.scheme == partition::PartitionScheme::Mbr || table.scheme == partition::PartitionScheme::Gpt) {
        for (const partition::Partition& partition : table.partitions) {
            VolumeRecord record;
            record.offset = partition.offset;
            record.size = partition.size;
            record.partition = partition;
            plan.push_back(std::move(record));
        }
        return plan;
    }
    // A volume boot record in sector 0, or nothing recognised: the device is one volume.
    VolumeRecord record;
    record.size = sourceSize;
    plan.push_back(std::move(record));
    return plan;
}

bool samePlan(const std::vector<VolumeRecord>& a, const std::vector<VolumeRecord>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
               return x.offset == y.offset && x.size == y.size;
           });
}

}  // namespace

std::string_view toString(ScanOutcome outcome) noexcept {
    switch (outcome) {
    case ScanOutcome::Completed:
        return "completed";
    case ScanOutcome::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

Status validate(const ScanConfiguration& configuration) {
    if (!std::has_single_bit(configuration.alignment) ||
        configuration.alignment > carving::ScanOptions::kMaxAlignment) {
        return makeError(ErrorCode::InvalidInput, "scan: the alignment must be a power of two up to " +
                                                      std::to_string(carving::ScanOptions::kMaxAlignment));
    }
    if (configuration.maxHits == 0) {
        return makeError(ErrorCode::InvalidInput, "scan: maxHits must be at least 1");
    }
    if (configuration.sectorRetryCount > carving::SourceReadOptions::kMaxSectorRetries) {
        return makeError(ErrorCode::InvalidInput, "scan: at most " +
                                                      std::to_string(carving::SourceReadOptions::kMaxSectorRetries) +
                                                      " retries of a sector");
    }
    storage::BadRegionMap known;
    for (const storage::BadRegion& region : configuration.knownBadRegions) {
        if (Status added = known.add(region); !added.ok()) {
            return makeError(ErrorCode::InvalidInput, "scan: a known bad region is empty or overflows");
        }
    }
    return success();
}

Status validate(const ScanRunOptions& options) {
    if (options.workerThreads > EngineConfig::kMaxWorkerThreads) {
        return makeError(ErrorCode::InvalidInput,
                         "scan: at most " + std::to_string(EngineConfig::kMaxWorkerThreads) + " workers");
    }
    if (options.blockSize == 0 || options.blockSize > storage::kMaxReadSize) {
        return makeError(ErrorCode::InvalidInput,
                         "scan: the block size must lie between 1 and " + std::to_string(storage::kMaxReadSize));
    }
    if (options.cacheBlocks > ScanSourceOptions::kMaxCacheBlocks) {
        return makeError(ErrorCode::InvalidInput,
                         "scan: the cache holds at most " + std::to_string(ScanSourceOptions::kMaxCacheBlocks) +
                             " blocks");
    }
    if (options.window > ScanRunOptions::kMaxWindow) {
        return makeError(ErrorCode::InvalidInput,
                         "scan: the window is at most " + std::to_string(ScanRunOptions::kMaxWindow));
    }
    if (options.checkpointBytes == 0 || options.checkpointItems == 0) {
        return makeError(ErrorCode::InvalidInput, "scan: checkpoints need a size and a number of items");
    }
    return success();
}

// ---------------------------------------------------------------------------
// One run
// ---------------------------------------------------------------------------

struct ScanCoordinator::Run {
    // A volume the scan uses: its view of the source and its filesystem.
    struct LiveVolume {
        // Its index in the checkpoint's volumes.
        std::size_t record = 0;
        std::unique_ptr<partition::PartitionSource> view;
        std::unique_ptr<FilesystemRecovery> recovery;
    };

    // What scanning one volume gave.
    struct VolumeOutcome {
        VolumeRecord record;
        std::unique_ptr<partition::PartitionSource> view;
        std::unique_ptr<FilesystemRecovery> recovery;
    };

    // An update under construction, handed out when enough is in it.
    struct Batch {
        ScanUpdate update;
        std::size_t items = 0;
        Clock::time_point since = Clock::now();
    };

    Run(ScanCoordinator& owner, const ScanUpdateSink& sink)
        : owner(owner), sink(sink), configuration(owner.configuration_), options(owner.options_),
          checkpoint(owner.checkpoint_) {}
    ~Run() {
        const std::lock_guard lock(owner.progressMutex_);
        owner.liveSource_ = nullptr;
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;
    Run(Run&&) = delete;
    Run& operator=(Run&&) = delete;

    [[nodiscard]] Status prepare(const ScanCheckpoint* resume);
    [[nodiscard]] Result<ScanSummary> execute();

    [[nodiscard]] Status volumes();
    [[nodiscard]] Status mp4Examination();
    [[nodiscard]] Status fragmentSeeds();
    [[nodiscard]] Status sourcePass();
    [[nodiscard]] Status mp4Delivery();
    [[nodiscard]] Status fragments();
    [[nodiscard]] Status evaluation();

    [[nodiscard]] Status emit(ScanUpdate update);
    // Hands the batch out when it holds checkpointItems or is old enough (or
    // `force`, when it holds anything).
    [[nodiscard]] Status flush(Batch& batch, bool force);
    // A safe point: when a pause is asked for, hands out what is done, then waits.
    [[nodiscard]] Status safePoint(Batch* batch);
    [[nodiscard]] Status waitPaused();
    // Cancelled once the job is.
    [[nodiscard]] Status cancelled() const {
        if (options.control.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "the scan was cancelled");
        }
        return success();
    }
    void progress(ScanStage stage, std::uint64_t done, std::uint64_t total, bool force);
    [[nodiscard]] ScanMetrics metrics();
    [[nodiscard]] bool done(ScanStage which) const { return checkpoint.stage() > which; }
    [[nodiscard]] std::size_t window(std::size_t items) const {
        return std::max<std::size_t>(1, std::min(items, windowSize));
    }
    [[nodiscard]] carving::SourceReadOptions reads() const;
    [[nodiscard]] carving::CarveOptions carveOptions() const;
    [[nodiscard]] Result<VolumeOutcome> openVolume(VolumeRecord record, bool scan) const;
    void log(LogLevel level, std::string_view message, std::initializer_list<diagnostics::LogField> fields) const {
        if (options.logger != nullptr) {
            options.logger->log(level, kComponent, message, fields);
        }
    }

    ScanCoordinator& owner;
    const ScanUpdateSink& sink;
    const ScanConfiguration& configuration;
    const ScanRunOptions& options;
    ScanCheckpoint& checkpoint;
    ScanIdentity identity;
    storage::BadRegionMap knownBad;
    std::unique_ptr<ScanSource> source;
    std::size_t windowSize = 1;
    std::vector<LiveVolume> live;
    std::unique_ptr<Mp4RecoverySteps> mp4;
    std::unique_ptr<FragmentRecoverySteps> fragmentSteps;
    // Last, so that it is destroyed first: its workers use everything above.
    std::unique_ptr<WorkerPool> pool;

    // Metrics: what earlier runs did, and this run's clock.
    ScanMetrics base;
    Clock::time_point started = Clock::now();
    Clock::duration pausedFor{0};
    bool paused = false;
    std::uint64_t candidates = 0;
    std::uint64_t failures = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t pending = 0;  // delivered, not handed out yet
    std::uint64_t passPosition = 0;
    storage::BadRegionMap unreadable;
    // The sink failed: nothing more is handed out.
    bool sinkFailed = false;
    ScanStage stage = ScanStage::Volumes;
    std::uint64_t stageDone = 0;
    std::uint64_t stageTotal = 0;
    Clock::time_point lastProgress = Clock::now();
};

carving::SourceReadOptions ScanCoordinator::Run::reads() const {
    carving::SourceReadOptions reads;
    reads.sectorRetryCount = configuration.sectorRetryCount;
    reads.knownBadRegions = knownBad.empty() ? nullptr : &knownBad;
    reads.cancellation = options.control.token();
    return reads;
}

carving::CarveOptions ScanCoordinator::Run::carveOptions() const {
    carving::CarveOptions carve;
    carve.scan.blockSize = options.blockSize;
    carve.scan.alignment = configuration.alignment;
    carve.scan.maxHits = configuration.maxHits;
    carve.scan.reads = reads();
    carve.scan.logger = options.logger;
    carve.validate = true;
    return carve;
}

Status ScanCoordinator::Run::prepare(const ScanCheckpoint* resume) {
    if (Status valid = validate(configuration); !valid.ok()) {
        return valid;
    }
    if (Status valid = validate(options); !valid.ok()) {
        return valid;
    }
    if (!owner.source_.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "scan: the source is not open");
    }
    if (owner.formats_.empty() && configuration.mode == ScanMode::Deep) {
        return makeError(ErrorCode::InvalidInput, "scan: a deep scan needs formats to carve");
    }
    identity = owner.identity();
    if (resume != nullptr && !resume->empty()) {
        if (!resume->identity().has_value() || *resume->identity() != identity) {
            return makeError(ErrorCode::InvalidInput,
                             "scan: the checkpoint is of another scan (another source, configuration, set of "
                             "formats or engine)");
        }
        if (resume != &owner.checkpoint_) {
            owner.checkpoint_ = *resume;
        }
    } else {
        owner.checkpoint_ = ScanCheckpoint{};
    }
    for (const storage::BadRegion& region : configuration.knownBadRegions) {
        (void)knownBad.add(region);  // validated
    }
    base = checkpoint.metrics();
    for (const storage::BadRegion& region : checkpoint.unreadable().regions()) {
        (void)unreadable.add(region);
    }
    for (const evaluation::EvaluationRecord& record : checkpoint.evaluated()) {
        ++candidates;
        failures += record.validationStatus == carving::ValidationStatus::Invalid ? 1 : 0;
        duplicates += record.duplicate ? 1 : 0;
    }
    if (checkpoint.pass().has_value()) {
        passPosition = checkpoint.pass()->position;
    }

    const std::uint32_t threads = effectiveWorkerThreads(EngineConfig{IoConfig{}, options.workerThreads});
    windowSize = options.window != 0 ? options.window : std::size_t{4} * threads;
    windowSize = std::clamp<std::size_t>(windowSize, 1, ScanRunOptions::kMaxWindow);
    Result<std::unique_ptr<WorkerPool>> created =
        WorkerPool::create(threads, std::max<std::size_t>(64, 2 * std::max<std::size_t>(windowSize, 64)));
    if (!created.ok()) {
        return created.error();
    }
    pool = std::move(created).value();

    ScanSourceOptions sourceOptions;
    sourceOptions.blockSize = options.blockSize;
    sourceOptions.cacheBlocks = options.cacheBlocks;
    sourceOptions.control = options.control;
    source = std::make_unique<ScanSource>(owner.source_, sourceOptions);
    if (Status opened = source->open(); !opened.ok()) {
        return opened;
    }
    const std::lock_guard lock(owner.progressMutex_);
    owner.liveSource_ = source.get();
    return success();
}

Result<ScanCoordinator::Run::VolumeOutcome> ScanCoordinator::Run::openVolume(VolumeRecord record, bool scan) const {
    VolumeOutcome outcome;
    outcome.view = std::make_unique<partition::PartitionSource>(*source, record.offset, record.size);
    record.scanned = true;
    if (Status opened = outcome.view->open(); !opened.ok()) {
        record.error = opened.error();
        outcome.record = std::move(record);
        return outcome;
    }
    Result<std::unique_ptr<FilesystemRecovery>> recovery =
        openFilesystemRecovery(*outcome.view, record.offset, {}, options.logger);
    if (!recovery.ok()) {
        if (recovery.error().code == ErrorCode::Cancelled) {
            return recovery.error();
        }
        record.error = recovery.error();
        outcome.record = std::move(record);
        return outcome;
    }
    record.filesystem = (*recovery)->filesystem().info().type;
    if (scan) {
        CandidateOptions candidateOptions;
        candidateOptions.includeActive = configuration.includeActive;
        candidateOptions.includeDeleted = configuration.includeDeleted;
        Result<CandidateScan> found = (*recovery)->findCandidates(candidateOptions, options.control.token());
        if (!found.ok()) {
            if (found.error().code == ErrorCode::Cancelled) {
                return found.error();
            }
            record.error = found.error();
            record.filesystem.reset();
            outcome.record = std::move(record);
            return outcome;
        }
        record.candidates = std::move(*found);
    }
    outcome.recovery = std::move(*recovery);
    outcome.record = std::move(record);
    return outcome;
}

ScanMetrics ScanCoordinator::Run::metrics() {
    ScanMetrics metrics;
    const ScanSourceStats stats = source != nullptr ? source->stats() : ScanSourceStats{};
    metrics.sourceSize = owner.source_.size();
    metrics.bytesScanned = configuration.mode == ScanMode::Deep ? passPosition : 0;
    metrics.bytesRead = base.bytesRead + stats.bytesRead;
    const auto now = Clock::now();
    const Clock::duration running = now - started - pausedFor;
    const auto runningMs = std::chrono::duration_cast<std::chrono::milliseconds>(running);
    metrics.elapsed = base.elapsed + runningMs;
    metrics.scanSpeed =
        runningMs.count() > 0 ? stats.bytesRead * 1000 / static_cast<std::uint64_t>(runningMs.count()) : 0;
    for (const VolumeRecord& volume : checkpoint.volumes()) {
        metrics.filesFound += volume.candidates.has_value() ? volume.candidates->candidates.size() : 0;
    }
    metrics.carves = checkpoint.carves().size();
    metrics.mp4Candidates = checkpoint.mp4Candidates().size();
    metrics.fragmentCandidates = checkpoint.fragmentCandidates().size();
    metrics.candidates = candidates;
    metrics.validationFailures = failures;
    metrics.duplicates = duplicates;
    storage::BadRegionMap all = unreadable;
    if (source != nullptr) {
        for (const storage::BadRegion& region : source->unreadableRegions()) {
            (void)all.add(region);
        }
    }
    metrics.unreadableBytes = all.totalBytes();
    return metrics;
}

void ScanCoordinator::Run::progress(ScanStage now, std::uint64_t doneUnits, std::uint64_t total, bool force) {
    stage = now;
    stageDone = doneUnits;
    stageTotal = total;
    const auto time = Clock::now();
    if (!force && time - lastProgress < options.progressInterval) {
        // The metrics are brought up to date at most every progressInterval.
        const std::lock_guard lock(owner.progressMutex_);
        owner.progress_.stage = stage;
        owner.progress_.stageDone = stageDone;
        owner.progress_.stageTotal = stageTotal;
        owner.progress_.paused = paused;
        return;
    }
    ScanProgress snapshot;
    snapshot.stage = stage;
    snapshot.stageDone = stageDone;
    snapshot.stageTotal = stageTotal;
    snapshot.paused = paused;
    snapshot.metrics = metrics();
    {
        const std::lock_guard lock(owner.progressMutex_);
        owner.progress_ = snapshot;
    }
    lastProgress = time;
    if (options.onProgress) {
        try {
            options.onProgress(snapshot);
        } catch (...) {
            log(LogLevel::Warning, "scan progress callback threw an exception", {});
        }
    }
}

Status ScanCoordinator::Run::emit(ScanUpdate update) {
    if (sinkFailed) {
        return makeError(ErrorCode::InternalError, "scan: an update after the sink failed");
    }
    update.sequence = checkpoint.sequence() + 1;
    if (checkpoint.empty()) {
        update.identity = identity;
    }
    for (const storage::BadRegion& region : source->takeNewUnreadable()) {
        update.unreadable.push_back(region);
        (void)unreadable.add(region);
    }
    if (update.pass.has_value()) {
        passPosition = update.pass->position;
    }
    const std::uint64_t delivered = update.candidates.size();
    update.metrics = metrics();
    if (sink) {
        if (Status sent = sink(update); !sent.ok()) {
            sinkFailed = true;
            return sent;
        }
    }
    if (Status applied = checkpoint.apply(update); !applied.ok()) {
        sinkFailed = true;
        return makeError(ErrorCode::InternalError,
                         "scan: an update the scan's own checkpoint refuses: " + applied.error().message);
    }
    pending -= std::min(pending, delivered);
    progress(stage, stageDone, stageTotal, update.stageComplete);
    return success();
}

Status ScanCoordinator::Run::flush(Batch& batch, bool force) {
    const bool due = batch.items >= options.checkpointItems ||
                     (batch.items > 0 && Clock::now() - batch.since >= options.checkpointInterval);
    if (sinkFailed || !(due || (force && batch.items > 0))) {
        return success();
    }
    const ScanStage batchStage = batch.update.stage;
    ScanUpdate update = std::exchange(batch.update, ScanUpdate{});
    batch.update.stage = batchStage;
    batch.items = 0;
    batch.since = Clock::now();
    return emit(std::move(update));
}

Status ScanCoordinator::Run::waitPaused() {
    paused = true;
    progress(stage, stageDone, stageTotal, true);
    log(LogLevel::Info, "scan paused", {field("stage", toString(stage))});
    const auto since = Clock::now();
    const Status waited = options.control.waitWhilePaused();
    pausedFor += Clock::now() - since;
    paused = false;
    progress(stage, stageDone, stageTotal, true);
    log(LogLevel::Info, waited.ok() ? "scan resumed" : "scan cancelled while paused", {});
    return waited;
}

Status ScanCoordinator::Run::safePoint(Batch* batch) {
    if (!options.control.isPauseRequested()) {
        return success();
    }
    if (batch != nullptr) {
        if (Status flushed = flush(*batch, true); !flushed.ok()) {
            return flushed;
        }
    }
    return waitPaused();
}

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------

Status ScanCoordinator::Run::volumes() {
    progress(ScanStage::Volumes, 0, 0, true);
    if (Status stop = cancelled(); !stop.ok()) {
        return stop;
    }
    const std::uint64_t size = source->size();
    Result<partition::PartitionTable> table = partition::readPartitionTable(*source);
    if (!table.ok()) {
        return table.error();
    }
    std::vector<VolumeRecord> plan = planVolumes(*table, size);
    if (!checkpoint.volumesPlanned()) {
        ScanUpdate update;
        update.stage = ScanStage::Volumes;
        update.partitionTable = std::move(*table);
        update.volumePlan = std::move(plan);
        update.stageComplete = update.volumePlan->empty();
        log(LogLevel::Info, "volumes planned",
            {field("scheme", partition::toString(update.partitionTable->scheme)),
             field("volumes", update.volumePlan->size())});
        if (Status sent = emit(std::move(update)); !sent.ok()) {
            return sent;
        }
    } else if (!samePlan(plan, checkpoint.volumes())) {
        return makeError(ErrorCode::InvalidInput,
                         "scan: the source's partitions are not the ones the checkpoint recorded");
    }

    // The volumes scanned before: their filesystems, opened again.
    const std::vector<VolumeRecord>& records = checkpoint.volumes();
    std::vector<std::size_t> remaining;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const VolumeRecord& record = records[i];
        if (!record.scanned) {
            remaining.push_back(i);
            continue;
        }
        if (!record.candidates.has_value()) {
            continue;
        }
        Result<VolumeOutcome> reopened = openVolume(VolumeRecord{record.offset, record.size, record.partition}, false);
        if (!reopened.ok()) {
            return reopened.error();
        }
        if (reopened->recovery == nullptr || reopened->record.filesystem != record.filesystem) {
            return makeError(ErrorCode::InvalidInput, "scan: the volume at " + std::to_string(record.offset) +
                                                          " does not open as the checkpoint recorded");
        }
        live.push_back(LiveVolume{i, std::move(reopened->view), std::move(reopened->recovery)});
    }
    if (done(ScanStage::Volumes)) {
        return success();
    }
    progress(ScanStage::Volumes, records.size() - remaining.size(), records.size(), true);
    const auto produce = [&](std::size_t k) -> Result<VolumeOutcome> {
        const VolumeRecord& planned = checkpoint.volumes()[remaining[k]];
        return openVolume(VolumeRecord{planned.offset, planned.size, planned.partition}, true);
    };
    const auto consume = [&](std::size_t k, VolumeOutcome&& outcome) -> Status {
        const std::size_t index = remaining[k];
        log(LogLevel::Info, "volume scanned",
            {field("offset", outcome.record.offset), field("size", outcome.record.size),
             field("filesystem", outcome.record.filesystem.has_value()
                                     ? std::string(filesystem::toString(*outcome.record.filesystem))
                                     : std::string("none")),
             field("candidates", outcome.record.candidates.has_value() ? outcome.record.candidates->candidates.size()
                                                                        : 0),
             field("error", outcome.record.error.has_value() ? describe(*outcome.record.error) : std::string())});
        ScanUpdate update;
        update.stage = ScanStage::Volumes;
        update.stageComplete = k + 1 == remaining.size();
        const bool usable = outcome.record.candidates.has_value() && outcome.recovery != nullptr;
        update.volumes.emplace_back(index, std::move(outcome.record));
        if (Status sent = emit(std::move(update)); !sent.ok()) {
            return sent;
        }
        if (usable) {
            live.push_back(LiveVolume{index, std::move(outcome.view), std::move(outcome.recovery)});
        }
        progress(ScanStage::Volumes, records.size() - remaining.size() + k + 1, records.size(), false);
        return safePoint(nullptr);
    };
    return runOrdered<VolumeOutcome>(pool.get(), window(remaining.size()), 0, remaining.size(), produce, consume);
}

Status ScanCoordinator::Run::mp4Examination() {
    progress(ScanStage::Mp4Examination, 0, 0, true);
    const bool needed = configuration.mp4 && (!done(ScanStage::Mp4Examination) || !done(ScanStage::Mp4Delivery));
    if (needed) {
        Mp4RecoveryOptions mp4Options;
        mp4Options.carving = carveOptions();
        mp4Options.carving.validate = false;
        Result<std::unique_ptr<Mp4RecoverySteps>> created = Mp4RecoverySteps::create(*source, mp4Options);
        if (!created.ok()) {
            return created.error();
        }
        mp4 = std::move(created).value();
        for (const LiveVolume& volume : live) {
            if (Status added = mp4->addVolume(*volume.recovery, *checkpoint.volumes()[volume.record].candidates);
                !added.ok()) {
                return added;
            }
        }
        for (const Mp4Examination& examination : checkpoint.mp4Examinations()) {
            if (Status added = mp4->addExamination(examination); !added.ok()) {
                return makeError(ErrorCode::InvalidInput,
                                 "scan: the checkpoint's MP4 examinations do not fit the volumes: " +
                                     added.error().message);
            }
        }
    }
    if (done(ScanStage::Mp4Examination)) {
        return success();
    }
    if (Status stop = cancelled(); !stop.ok()) {
        return stop;
    }
    Batch batch;
    batch.update.stage = ScanStage::Mp4Examination;
    if (mp4 != nullptr) {
        // The candidates not examined yet, in scan order.
        std::vector<std::pair<std::size_t, std::size_t>> items;
        std::optional<std::pair<std::size_t, std::size_t>> last;
        if (!checkpoint.mp4Examinations().empty()) {
            last.emplace(checkpoint.mp4Examinations().back().volume, checkpoint.mp4Examinations().back().index);
        }
        std::uint64_t total = 0;
        for (std::size_t v = 0; v < live.size(); ++v) {
            const std::size_t count = checkpoint.volumes()[live[v].record].candidates->candidates.size();
            total += count;
            for (std::size_t i = 0; i < count; ++i) {
                if (!last.has_value() || std::pair{v, i} > *last) {
                    items.emplace_back(v, i);
                }
            }
        }
        const std::uint64_t before = total - items.size();
        progress(ScanStage::Mp4Examination, before, total, true);
        const auto produce = [&](std::size_t k) -> Result<Mp4Examination> {
            return mp4->examine(items[k].first, items[k].second);
        };
        const auto consume = [&](std::size_t k, Mp4Examination&& examination) -> Status {
            if (Status added = mp4->addExamination(examination); !added.ok()) {
                return added;
            }
            batch.update.mp4Examinations.push_back(std::move(examination));
            ++batch.items;
            progress(ScanStage::Mp4Examination, before + k + 1, total, false);
            if (Status flushed = flush(batch, false); !flushed.ok()) {
                return flushed;
            }
            return safePoint(&batch);
        };
        if (Status ran =
                runOrdered<Mp4Examination>(pool.get(), window(items.size()), 0, items.size(), produce, consume);
            !ran.ok()) {
            if (!sinkFailed) {
                (void)flush(batch, true);
            }
            return ran;
        }
    }
    batch.update.stageComplete = true;
    return emit(std::move(batch.update));
}

Status ScanCoordinator::Run::fragmentSeeds() {
    progress(ScanStage::FragmentSeeds, 0, 0, true);
    const bool needed = configuration.fragments && (!done(ScanStage::FragmentSeeds) || !done(ScanStage::Fragments));
    // The fragment steps' volumes: the live volumes they accept (one with a cluster area).
    std::vector<std::size_t> volumesUsed;
    if (needed) {
        FragmentRecoveryOptions fragmentOptions;
        fragmentOptions.carving = carveOptions();
        Result<std::unique_ptr<FragmentRecoverySteps>> created =
            FragmentRecoverySteps::create(*source, owner.formats_, fragmentOptions);
        if (!created.ok()) {
            return created.error();
        }
        fragmentSteps = std::move(created).value();
        for (std::size_t v = 0; v < live.size(); ++v) {
            const Status added =
                fragmentSteps->addVolume(*live[v].recovery, *checkpoint.volumes()[live[v].record].candidates);
            if (added.ok()) {
                volumesUsed.push_back(v);
            } else if (added.error().code != ErrorCode::InvalidInput) {
                return added;
            }
        }
        if (Status begun = fragmentSteps->begin(); !begun.ok()) {
            return begun;
        }
        for (const FragmentSeedExamination& examination : checkpoint.seedExaminations()) {
            if (Status added = fragmentSteps->addSeedExamination(examination); !added.ok()) {
                return makeError(ErrorCode::InvalidInput,
                                 "scan: the checkpoint's seed examinations do not fit the volumes: " +
                                     added.error().message);
            }
        }
    }
    if (done(ScanStage::FragmentSeeds)) {
        return success();
    }
    if (Status stop = cancelled(); !stop.ok()) {
        return stop;
    }
    Batch batch;
    batch.update.stage = ScanStage::FragmentSeeds;
    if (fragmentSteps != nullptr) {
        std::vector<std::pair<std::size_t, std::size_t>> items;
        std::optional<std::pair<std::size_t, std::size_t>> last;
        if (!checkpoint.seedExaminations().empty()) {
            last.emplace(checkpoint.seedExaminations().back().volume, checkpoint.seedExaminations().back().index);
        }
        std::uint64_t total = 0;
        for (std::size_t k = 0; k < volumesUsed.size(); ++k) {
            const std::size_t count =
                checkpoint.volumes()[live[volumesUsed[k]].record].candidates->candidates.size();
            total += count;
            for (std::size_t i = 0; i < count; ++i) {
                if (!last.has_value() || std::pair{k, i} > *last) {
                    items.emplace_back(k, i);
                }
            }
        }
        const std::uint64_t before = total - items.size();
        progress(ScanStage::FragmentSeeds, before, total, true);
        const auto produce = [&](std::size_t k) -> Result<FragmentSeedExamination> {
            return fragmentSteps->examineSeed(items[k].first, items[k].second);
        };
        const auto consume = [&](std::size_t k, FragmentSeedExamination&& examination) -> Status {
            if (Status added = fragmentSteps->addSeedExamination(examination); !added.ok()) {
                return added;
            }
            batch.update.seedExaminations.push_back(std::move(examination));
            ++batch.items;
            progress(ScanStage::FragmentSeeds, before + k + 1, total, false);
            if (Status flushed = flush(batch, false); !flushed.ok()) {
                return flushed;
            }
            return safePoint(&batch);
        };
        if (Status ran = runOrdered<FragmentSeedExamination>(pool.get(), window(items.size()), 0, items.size(),
                                                              produce, consume);
            !ran.ok()) {
            if (!sinkFailed) {
                (void)flush(batch, true);
            }
            return ran;
        }
    }
    batch.update.stageComplete = true;
    return emit(std::move(batch.update));
}

Status ScanCoordinator::Run::sourcePass() {
    // What the pass's commits did before: MP4 recovery's state, and the events
    // that rebuild fragment reconstruction's.
    if (mp4 != nullptr && checkpoint.pass().has_value()) {
        if (Status restored = mp4->restore(checkpoint.mp4State()); !restored.ok()) {
            return makeError(ErrorCode::InvalidInput,
                             "scan: the checkpoint's MP4 state does not fit: " + restored.error().message);
        }
    }
    if (fragmentSteps != nullptr && !checkpoint.fragmentEvents().empty()) {
        if (Status replayed = fragmentSteps->replay(checkpoint.fragmentEvents()); !replayed.ok()) {
            return makeError(ErrorCode::InvalidInput,
                             "scan: the checkpoint's source pass does not fit: " + replayed.error().message);
        }
    }
    if (done(ScanStage::SourcePass)) {
        return success();
    }
    if (Status stop = cancelled(); !stop.ok()) {
        return stop;
    }
    const std::uint64_t size = source->size();
    PassState state = checkpoint.pass().value_or(PassState{});
    passPosition = state.position;
    progress(ScanStage::SourcePass, state.position, size, true);
    if (!configuration.carving && mp4 == nullptr && fragmentSteps == nullptr) {
        state.position = size;
        ScanUpdate update;
        update.stage = ScanStage::SourcePass;
        update.pass = state;
        update.stageComplete = true;
        return emit(std::move(update));
    }
    detail::PassSetup setup;
    setup.source = source.get();
    setup.formats = &owner.formats_;
    setup.pool = pool.get();
    setup.window = std::max<std::size_t>(windowSize, 64);
    setup.carving = carveOptions();
    setup.scan = setup.carving.scan;
    setup.maxHits = configuration.maxHits;
    setup.carve = configuration.carving;
    setup.mp4 = mp4.get();
    setup.fragments = fragmentSteps.get();
    setup.control = options.control;
    setup.waitWhilePaused = [this] { return waitPaused(); };
    setup.checkpointBytes = options.checkpointBytes;
    setup.checkpointInterval = options.checkpointInterval;
    setup.progressInterval = options.progressInterval;
    if (fragmentSteps != nullptr) {
        fragmentSteps->recordEvents(true);
    }
    const detail::PassCheckpoint point = [&](const PassState& now, std::vector<carving::FileCandidate>&& carves,
                                             bool final) -> Status {
        ScanUpdate update;
        update.stage = ScanStage::SourcePass;
        update.pass = now;
        update.carves = std::move(carves);
        if (mp4 != nullptr) {
            update.mp4Changes = mp4->takeChanges();
        }
        if (fragmentSteps != nullptr) {
            update.fragmentEvents = fragmentSteps->takeEvents();
        }
        update.stageComplete = final;
        if (final) {
            log(LogLevel::Info, "source pass ended",
                {field("outcome", carving::toString(now.scan.outcome)), field("hits", now.scan.hits),
                 field("carves", now.carving.candidates), field("bytes_read", now.scan.bytesRead),
                 field("unreadable_bytes", now.scan.unreadableBytes)});
        }
        return emit(std::move(update));
    };
    const detail::PassProgress moved = [&](std::uint64_t position) {
        passPosition = position;
        progress(ScanStage::SourcePass, position, size, false);
    };
    log(LogLevel::Info, "source pass started", {field("position", state.position), field("size", size)});
    const Status ran = detail::runSourcePass(setup, state, point, moved);
    if (fragmentSteps != nullptr) {
        fragmentSteps->recordEvents(false);
    }
    return ran;
}

Status ScanCoordinator::Run::mp4Delivery() {
    if (done(ScanStage::Mp4Delivery)) {
        mp4.reset();
        return success();
    }
    progress(ScanStage::Mp4Delivery, 0, 0, true);
    if (Status stop = cancelled(); !stop.ok()) {
        return stop;
    }
    ScanUpdate update;
    update.stage = ScanStage::Mp4Delivery;
    update.stageComplete = true;
    if (mp4 != nullptr) {
        Mp4RecoveryReport report;
        if (checkpoint.pass().has_value()) {
            report.scan = checkpoint.pass()->scan;
        }
        const Mp4CandidateSink collect = [&](Mp4Candidate&& candidate) -> Status {
            update.mp4Candidates.push_back(std::move(candidate));
            return success();
        };
        if (Status delivered = mp4->deliver(collect, report); !delivered.ok()) {
            return delivered;
        }
        update.mp4Report = std::move(report);
    }
    mp4.reset();
    return emit(std::move(update));
}

Status ScanCoordinator::Run::fragments() {
    if (done(ScanStage::Fragments)) {
        fragmentSteps.reset();
        return success();
    }
    progress(ScanStage::Fragments, 0, 0, true);
    if (Status stop = cancelled(); !stop.ok()) {
        return stop;
    }
    Batch batch;
    batch.update.stage = ScanStage::Fragments;
    if (fragmentSteps != nullptr) {
        for (const FragmentCandidate& candidate : checkpoint.fragmentCandidates()) {
            if (Status replayed = fragmentSteps->replayReconstruction(candidate); !replayed.ok()) {
                return makeError(ErrorCode::InvalidInput,
                                 "scan: the checkpoint's reconstructions do not fit: " + replayed.error().message);
            }
        }
        const auto reportNow = [&] {
            FragmentRecoveryReport report = fragmentSteps->report();
            if (checkpoint.pass().has_value()) {
                report.scan = checkpoint.pass()->scan;
            }
            return report;
        };
        progress(ScanStage::Fragments, fragmentSteps->nextSeed(), fragmentSteps->seedCount(), true);
        while (fragmentSteps->nextSeed() < fragmentSteps->seedCount()) {
            if (Status point = safePoint(&batch); !point.ok()) {
                return point;
            }
            Result<std::optional<FragmentCandidate>> candidate = fragmentSteps->reconstructNext();
            if (!candidate.ok()) {
                if (!sinkFailed) {
                    batch.update.fragmentReport = reportNow();
                    (void)flush(batch, true);
                }
                return candidate.error();
            }
            if (candidate->has_value()) {
                batch.update.fragmentCandidates.push_back(std::move(**candidate));
                ++batch.items;
                batch.update.fragmentReport = reportNow();
            }
            progress(ScanStage::Fragments, fragmentSteps->nextSeed(), fragmentSteps->seedCount(), false);
            if (Status flushed = flush(batch, false); !flushed.ok()) {
                return flushed;
            }
        }
        batch.update.fragmentReport = reportNow();
    }
    fragmentSteps.reset();
    batch.update.stageComplete = true;
    return emit(std::move(batch.update));
}

Status ScanCoordinator::Run::evaluation() {
    if (done(ScanStage::Evaluation)) {
        return success();
    }
    progress(ScanStage::Evaluation, checkpoint.evaluated().size(), 0, true);
    if (Status stop = cancelled(); !stop.ok()) {
        return stop;
    }
    evaluation::EvaluationOptions evaluationOptions;
    evaluationOptions.firstId = 1;
    evaluationOptions.validation.media = configuration.media;
    evaluationOptions.validation.playability = configuration.playability;
    evaluationOptions.identity.sha256 = configuration.sha256;
    evaluationOptions.identity.preliminary = configuration.preliminaryHash;
    evaluationOptions.reads = reads();
    evaluationOptions.pool = pool.get();
    evaluationOptions.window = windowSize;
    evaluationOptions.resume = checkpoint.evaluated();
    evaluation::CandidateEvaluation evaluation(*source, owner.formats_, owner.media_, evaluationOptions);
    for (const LiveVolume& volume : live) {
        if (Status added = evaluation.addVolume(*volume.recovery, *checkpoint.volumes()[volume.record].candidates);
            !added.ok()) {
            return added;
        }
    }
    for (const carving::FileCandidate& carve : checkpoint.carves()) {
        if (Status added = evaluation.addCarve(carve); !added.ok()) {
            return added;
        }
    }
    for (const Mp4Candidate& candidate : checkpoint.mp4Candidates()) {
        if (Status added = evaluation.addMp4Candidate(candidate); !added.ok()) {
            return added;
        }
    }
    for (const FragmentCandidate& candidate : checkpoint.fragmentCandidates()) {
        if (Status added = evaluation.addFragmentCandidate(candidate); !added.ok()) {
            return added;
        }
    }
    Batch batch;
    batch.update.stage = ScanStage::Evaluation;
    const evaluation::EvaluatedCandidateSink deliver = [&](evaluation::EvaluatedCandidate&& candidate) -> Status {
        ++candidates;
        ++pending;
        failures += candidate.validationStatus() == carving::ValidationStatus::Invalid ? 1 : 0;
        duplicates += candidate.duplicateOf.has_value() ? 1 : 0;
        batch.update.candidates.push_back(std::move(candidate));
        ++batch.items;
        progress(ScanStage::Evaluation, candidates, 0, false);
        if (Status flushed = flush(batch, false); !flushed.ok()) {
            return flushed;
        }
        return safePoint(&batch);
    };
    Result<evaluation::EvaluationReport> report = evaluation.run(deliver);
    if (!report.ok()) {
        if (!sinkFailed) {
            (void)flush(batch, true);
        }
        return report.error();
    }
    log(LogLevel::Info, "evaluation ended",
        {field("candidates", report->candidates()), field("valid", report->valid), field("invalid", report->invalid),
         field("duplicates", report->duplicates)});
    batch.update.evaluationReport = *report;
    batch.update.stageComplete = true;
    return emit(std::move(batch.update));
}

Result<ScanSummary> ScanCoordinator::Run::execute() {
    started = Clock::now();
    log(LogLevel::Info, "scan started",
        {field("mode", toString(configuration.mode)), field("source", identity.sourcePath),
         field("size", identity.sourceSize), field("workers", pool->threadCount()),
         field("resumed_at", checkpoint.sequence()), field("stage", toString(checkpoint.stage()))});
    Status status = success();
    if (!checkpoint.completed()) {
        const bool deep = configuration.mode == ScanMode::Deep;
        status = volumes();
        if (status.ok() && deep) {
            status = mp4Examination();
        }
        if (status.ok() && deep) {
            status = fragmentSeeds();
        }
        if (status.ok() && deep) {
            status = sourcePass();
        }
        if (status.ok() && deep) {
            status = mp4Delivery();
        }
        if (status.ok() && deep) {
            status = fragments();
        }
        if (status.ok()) {
            status = evaluation();
        }
    }
    ScanSummary summary;
    summary.stage = checkpoint.stage();
    summary.updates = checkpoint.sequence();
    summary.metrics = metrics();
    if (!status.ok()) {
        if (status.error().code != ErrorCode::Cancelled) {
            log(LogLevel::Error, "scan failed", {field("error", describe(status.error()))});
            return status.error();
        }
        summary.outcome = ScanOutcome::Cancelled;
    }
    progress(summary.stage, stageDone, stageTotal, true);
    log(LogLevel::Info, "scan ended",
        {field("outcome", toString(summary.outcome)), field("stage", toString(summary.stage)),
         field("candidates", summary.metrics.candidates), field("bytes_read", summary.metrics.bytesRead),
         field("unreadable_bytes", summary.metrics.unreadableBytes),
         field("elapsed_ms", summary.metrics.elapsed.count())});
    return summary;
}

// ---------------------------------------------------------------------------
// ScanCoordinator
// ---------------------------------------------------------------------------

ScanCoordinator::ScanCoordinator(storage::IStorageSource& source, const carving::FormatRegistry& formats,
                                 const validation::MediaValidatorRegistry& media, ScanConfiguration configuration,
                                 ScanRunOptions options)
    : source_(source),
      formats_(formats),
      media_(media),
      configuration_(std::move(configuration)),
      options_(std::move(options)) {}

ScanCoordinator::~ScanCoordinator() = default;

ScanIdentity ScanCoordinator::identity() const {
    ScanIdentity identity;
    identity.engineVersion = std::string(kEngineVersion);
    const storage::SourceInfo info = source_.getInfo();
    identity.sourceType = info.type;
    identity.sourcePath = toUtf8(info.path);
    identity.sourceSize = source_.size();
    identity.sectorSize = source_.sectorSize();
    identity.mode = configuration_.mode;
    identity.configuration = describe(configuration_);
    for (const std::shared_ptr<const carving::IFileFormat>& format : formats_.formats()) {
        identity.formats.push_back(format->descriptor().id);
    }
    for (const std::shared_ptr<const validation::IMediaValidator>& validator : media_.validators()) {
        identity.mediaValidators.emplace_back(validator->formatId());
    }
    return identity;
}

ScanProgress ScanCoordinator::progress() const {
    const std::lock_guard lock(progressMutex_);
    ScanProgress snapshot = progress_;
    if (!snapshot.paused && liveSource_ != nullptr && options_.control.isPauseRequested() &&
        liveSource_->activeReads() == 0) {
        snapshot.paused = true;
    }
    return snapshot;
}

Result<ScanSummary> ScanCoordinator::run(const ScanUpdateSink& sink, const ScanCheckpoint* resume) {
    if (running_.exchange(true)) {
        return makeError(ErrorCode::InvalidInput, "scan: the coordinator is running already");
    }
    struct Done {
        std::atomic<bool>& flag;
        ~Done() { flag = false; }
    } done{running_};
    Run run(*this, sink);
    if (Status prepared = run.prepare(resume); !prepared.ok()) {
        return prepared.error();
    }
    return run.execute();
}

}  // namespace recovery::scan
