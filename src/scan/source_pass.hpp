#pragma once

// Private to recovery_scan: the source pass of a scan (P15). One signature
// scan of the source gives its hits to every stage that carves: carving (as
// FileCarver::run carves), MP4 recovery and fragment reconstruction (through
// their steps). The scan runs on a thread of its own and reads the source
// sequentially; the stages' carves and analyses are made ahead on the pool's
// workers; the commits are made on the calling thread, hit after hit in
// source order, so that each stage skips exactly the hits it skips when it
// scans on its own.

#include "carving/file_candidate.hpp"
#include "carving/file_carver.hpp"
#include "carving/format_registry.hpp"
#include "carving/signature_scanner.hpp"
#include "recovery/fragment_recovery.hpp"
#include "recovery/job_control.hpp"
#include "recovery/mp4_recovery.hpp"
#include "recovery/result.hpp"
#include "recovery/worker_pool.hpp"
#include "scan/scan_state.hpp"
#include "storage/storage_source.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace recovery::scan::detail {

struct PassSetup {
    // The scan's source (a ScanSource).
    storage::IStorageSource* source = nullptr;
    // The carving formats.
    const carving::FormatRegistry* formats = nullptr;
    WorkerPool* pool = nullptr;
    // Hits in flight at once (admitted and not committed); at least 64.
    std::size_t window = 64;
    // Carves: validation on, the scan's reads (with the job's token).
    carving::CarveOptions carving;
    // The scan's settings: block size, alignment, reads; the pass sets the
    // range, the hit limit and the progress callback.
    carving::ScanOptions scan;
    std::uint64_t maxHits = 10'000'000;
    // The stages that take part: carving, and the steps of MP4 recovery and
    // of fragment reconstruction (null when they do not).
    bool carve = true;
    Mp4RecoverySteps* mp4 = nullptr;
    FragmentRecoverySteps* fragments = nullptr;
    JobControl control;
    // Waits while the job is paused (control.waitWhilePaused() when empty).
    std::function<Status()> waitWhilePaused;
    // A consistent point is handed out at least every checkpointBytes of the
    // source or every checkpointInterval.
    std::uint64_t checkpointBytes = 64 * kMiB;
    std::chrono::milliseconds checkpointInterval{2000};
    std::chrono::milliseconds progressInterval{250};
};

// Called at each consistent point with the pass's state and the carves
// committed since the last one (the caller takes the stages' changes itself).
// `final` once the pass has ended (at the end of the source or the hit
// limit). Not called again after it fails.
using PassCheckpoint =
    std::function<Status(const PassState& state, std::vector<carving::FileCandidate>&& carves, bool final)>;
// Called at least every progressInterval with the pass's position.
using PassProgress = std::function<void(std::uint64_t position)>;

// Runs the pass from `state.position` (a checkpoint's) to the end of the
// source. A pause request is a consistent point: the pass hands out a
// checkpoint, then waits. Returns Cancelled when the job is cancelled (after
// a last consistent point), or the first failure of a read, a stage or a
// callback (after a last consistent point, unless the callback failed).
[[nodiscard]] Status runSourcePass(const PassSetup& setup, PassState& state, const PassCheckpoint& checkpoint,
                                   const PassProgress& progress);

}  // namespace recovery::scan::detail
