#pragma once

// The scan coordinator (P15): one scan of a source, every stage of it, on a
// bounded pool of workers, cancellable, pausable, reporting its progress, and
// resumable from the updates it handed out.
//
//   Volumes ─► MP4 examination ─► fragment seeds ─► source pass ─► MP4 delivery ─► fragments ─► evaluation
//   (Quick: Volumes ─► evaluation)
//
//  * Volumes: the partition table, then each volume's filesystem and its
//    candidates (P7), volumes in parallel.
//  * MP4 examination and fragment seeds: MP4 recovery and fragment
//    reconstruction examine the filesystem candidates, candidates in
//    parallel (P12, P13).
//  * Source pass: ONE sequential pass over the source finds the signatures
//    of every format; carving (P8-P10), MP4 recovery's carving (P12) and
//    fragment reconstruction's carving (P13) all take their hits from it.
//    Carves are made ahead on the workers and committed in source order,
//    so each stage skips exactly the hits it skips on its own.
//  * MP4 delivery, then fragment reconstruction, seed by seed (each settled
//    seed is evidence for the next, so seeds are reconstructed in order).
//  * Evaluation (P14): one candidate per file, validated and hashed on the
//    workers, delivered in order.
//
// What a scan finds does not depend on the number of workers, on pauses or
// on interruptions: a scan interrupted at any point and resumed from its
// updates delivers the candidates an uninterrupted one-thread scan delivers,
// with the same ids, in the same order, and does again only the work that
// was in progress (never a unit an update recorded).
//
// The source is only read, through a ScanSource: one cache of the blocks read
// recently serves every stage, reads wait while the scan is paused, and the
// bytes read and found unreadable are counted.
//
// Threads: run() runs on the calling thread, which drives every stage, makes
// every commit and calls the sink and the progress callback. The pool's
// workers carve, analyse, examine and validate; one more thread reads the
// source ahead during the pass. Memory: the pool's queue, the windows of
// work in flight, one block per worker and the cache are bounded by the run
// options; the stages' results (filesystem candidates, carves, MP4 and
// fragment candidates) are kept until the evaluation, about a few kilobytes
// per file, as the stages keep them on their own. See docs/recovery/scanning.md.

#include "carving/format_registry.hpp"
#include "diagnostics/logger.hpp"
#include "recovery/config.hpp"
#include "recovery/job_control.hpp"
#include "recovery/result.hpp"
#include "scan/scan_source.hpp"
#include "scan/scan_state.hpp"
#include "storage/bad_region.hpp"
#include "storage/storage_source.hpp"
#include "validation/media_validator.hpp"
#include "validation/playability.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace recovery::scan {

// The settings that decide what a scan finds. They are part of the scan's
// identity: a scan resumes only with the same configuration.
struct ScanConfiguration {
    ScanMode mode = ScanMode::Deep;
    // Deep scans: the stages that run besides volumes and evaluation.
    bool carving = true;
    bool mp4 = true;
    bool fragments = true;
    // Filesystem candidates: active files, and deleted ones (and the files
    // inside deleted directories).
    bool includeActive = true;
    bool includeDeleted = true;
    // The source pass examines file starts at multiples of `alignment` (1:
    // every byte; 512: sector starts), and stops after `maxHits` hits.
    std::uint32_t alignment = 1;
    std::uint64_t maxHits = 10'000'000;
    // A sector that fails is read again this many times before it counts as
    // unreadable; regions known to be unreadable (an image's zero-filled bad
    // sectors) are not read.
    std::uint32_t sectorRetryCount = 1;
    std::vector<storage::BadRegion> knownBadRegions;
    // Validation: the media level, and the playability level with this
    // checker (none: off; not owned, it must outlive the scan).
    bool media = true;
    validation::IPlayabilityChecker* playability = nullptr;
    // Content identity: SHA-256 (duplicates need it) and the preliminary hash.
    bool sha256 = true;
    bool preliminaryHash = true;
};

// InvalidInput for settings out of range.
[[nodiscard]] Status validate(const ScanConfiguration& configuration);

struct ScanProgress;

// How a scan runs. None of this changes what it finds.
struct ScanRunOptions {
    static constexpr std::size_t kMaxWindow = 4096;

    // Workers: 0 chooses from the hardware (EngineConfig's rules).
    std::uint32_t workerThreads = 0;
    // Size of the pass's sequential reads and of the cache's blocks: a
    // multiple of the source's sector size, at most storage::kMaxReadSize.
    std::size_t blockSize = 1 * kMiB;
    // Blocks the cache keeps (0: no cache).
    std::size_t cacheBlocks = 64;
    // Work in flight at once: hits of the pass, candidates examined or
    // evaluated (0: four per worker; at most kMaxWindow).
    std::size_t window = 0;
    // An update is handed out at least every checkpointBytes of the pass,
    // every checkpointItems candidates of the other stages, or every
    // checkpointInterval, whichever comes first, and when a stage ends, the
    // scan pauses, is cancelled or fails.
    std::uint64_t checkpointBytes = 64 * kMiB;
    std::size_t checkpointItems = 256;
    std::chrono::milliseconds checkpointInterval{2000};
    // Progress callback: on the thread of run(), at most every
    // progressInterval (and at each stage's start and end). Must not throw.
    std::chrono::milliseconds progressInterval{250};
    std::function<void(const ScanProgress&)> onProgress;
    // Cancels and pauses the scan from any thread.
    JobControl control;
    diagnostics::Logger* logger = nullptr;
};

// InvalidInput for options out of range.
[[nodiscard]] Status validate(const ScanRunOptions& options);

struct ScanProgress {
    ScanStage stage = ScanStage::Volumes;
    // The stage's units done and to do: volumes; candidates examined; bytes
    // of the source (the pass); seeds; candidates evaluated (its total is
    // known once the evaluation has merged its inputs).
    std::uint64_t stageDone = 0;
    std::uint64_t stageTotal = 0;
    // A pause was asked for and has taken effect: the scan waits at a safe
    // point, or no read of the source is under way (its readers wait).
    bool paused = false;
    ScanMetrics metrics;
};

enum class ScanOutcome : std::uint8_t {
    Completed,
    // Cancelled: the updates handed out make a checkpoint to resume from.
    Cancelled,
};

[[nodiscard]] std::string_view toString(ScanOutcome outcome) noexcept;

struct ScanSummary {
    ScanOutcome outcome = ScanOutcome::Completed;
    // The stage reached (Completed when the scan is).
    ScanStage stage = ScanStage::Volumes;
    // The last update's sequence number.
    std::uint64_t updates = 0;
    ScanMetrics metrics;
};

// Thread safety: run() needs one owner; progress() may be called from any
// thread at any time, also while run() runs. The source, the registries and
// the playability checker must outlive the object.
class ScanCoordinator {
public:
    ScanCoordinator(storage::IStorageSource& source, const carving::FormatRegistry& formats,
                    const validation::MediaValidatorRegistry& media, ScanConfiguration configuration,
                    ScanRunOptions options = {});
    ~ScanCoordinator();
    ScanCoordinator(const ScanCoordinator&) = delete;
    ScanCoordinator& operator=(const ScanCoordinator&) = delete;
    ScanCoordinator(ScanCoordinator&&) = delete;
    ScanCoordinator& operator=(ScanCoordinator&&) = delete;

    // Runs the scan, or resumes it from `resume` (the checkpoint of an earlier
    // run of the same scan). Each update goes to `sink` as it is made.
    // Returns when the scan completes or is cancelled (outcome Cancelled).
    // Fails with InvalidInput for invalid settings, a source that is not
    // open, a checkpoint of another scan or one that does not fit the source
    // as it is now; with the error of a read that fails for a reason other
    // than an I/O error; and with the sink's error. After a failure as after
    // a cancellation, the updates handed out are a checkpoint to resume from.
    [[nodiscard]] Result<ScanSummary> run(const ScanUpdateSink& sink, const ScanCheckpoint* resume = nullptr);

    [[nodiscard]] ScanProgress progress() const;
    // Every update this coordinator's runs handed out (and the checkpoint
    // they resumed from), applied: what a next run resumes from.
    [[nodiscard]] const ScanCheckpoint& checkpoint() const noexcept { return checkpoint_; }
    // The identity of this scan.
    [[nodiscard]] ScanIdentity identity() const;

    struct Run;

private:
    storage::IStorageSource& source_;
    const carving::FormatRegistry& formats_;
    const validation::MediaValidatorRegistry& media_;
    ScanConfiguration configuration_;
    ScanRunOptions options_;
    ScanCheckpoint checkpoint_;
    mutable std::mutex progressMutex_;
    ScanProgress progress_;
    // The source of the run under way (under progressMutex_), to tell when
    // its reads have stopped.
    const ScanSource* liveSource_ = nullptr;
    std::atomic<bool> running_{false};
};

}  // namespace recovery::scan
