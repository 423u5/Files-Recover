#pragma once

// The recovery job (P15): writes evaluated candidates to a destination on a
// bounded pool of workers, cancellable, pausable, reporting its progress,
// and resumable from the updates it handed out, without writing a file twice.

#include "diagnostics/logger.hpp"
#include "evaluation/evaluated_candidate.hpp"
#include "recovery/job_control.hpp"
#include "recovery/recovery_writer.hpp"
#include "recovery/result.hpp"
#include "scan/scan_source.hpp"
#include "storage/storage_source.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace recovery::scan {

struct RecoveryJobMetrics {
    // Files to write, written, and that could not be written.
    std::uint64_t files = 0;
    std::uint64_t recoveredFiles = 0;
    std::uint64_t failedFiles = 0;
    // Bytes written, and bytes of them that could not be read from the
    // source (written as zeros).
    std::uint64_t bytesRecovered = 0;
    std::uint64_t unreadableBytes = 0;
    // Bytes read from the source.
    std::uint64_t bytesRead = 0;
    // Bytes written per second of running time in this run.
    std::uint64_t speed = 0;
    // Running time (time paused is not counted), over every run of the job.
    std::chrono::milliseconds elapsed{0};
};

// A candidate the job is done with: written (where, and what reconstruction
// found), or not (why).
struct RecoveredItem {
    evaluation::EvaluatedCandidateId candidate{0};
    std::optional<RecoveredFile> file;
    std::optional<Error> error;
};

// One consistent point of a recovery job: the candidates done since the
// update before. Every file it names is complete and flushed.
struct RecoveryJobUpdate {
    std::uint64_t sequence = 0;
    // The first update of a job: the destination it writes to (UTF-8).
    std::optional<std::string> destination;
    std::vector<RecoveredItem> items;
    // Every candidate is done.
    bool complete = false;
    RecoveryJobMetrics metrics;
};

using RecoveryJobUpdateSink = std::function<Status(const RecoveryJobUpdate& update)>;

// The updates of a recovery job, applied in order: what a job resumes from.
//
// Thread safety: none; one owner at a time.
class RecoveryJobCheckpoint {
public:
    // Applies `update`, all of it or nothing: fails with InvalidInput,
    // leaving the checkpoint as it was, for an update that does not follow
    // (sequence, destination) or names a candidate done before.
    [[nodiscard]] Status apply(const RecoveryJobUpdate& update);

    [[nodiscard]] bool empty() const noexcept { return sequence_ == 0; }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
    [[nodiscard]] const std::optional<std::string>& destination() const noexcept { return destination_; }
    [[nodiscard]] bool complete() const noexcept { return complete_; }
    // The candidates done, by id.
    [[nodiscard]] const std::map<std::uint64_t, RecoveredItem>& items() const noexcept { return items_; }
    [[nodiscard]] bool done(evaluation::EvaluatedCandidateId candidate) const {
        return items_.contains(candidate.value());
    }
    [[nodiscard]] const RecoveryJobMetrics& metrics() const noexcept { return metrics_; }

private:
    std::uint64_t sequence_ = 0;
    std::optional<std::string> destination_;
    bool complete_ = false;
    std::map<std::uint64_t, RecoveredItem> items_;
    RecoveryJobMetrics metrics_;
};

struct RecoveryJobProgress {
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    // A pause was asked for and no read of the source is under way.
    bool paused = false;
    RecoveryJobMetrics metrics;
};

struct RecoveryJobOptions {
    // How files are written: directories, chunk size, retries, known bad
    // regions. Its cancellation token is replaced by the job's.
    RecoveryWriterOptions writer;
    // Workers: 0 chooses from the hardware (EngineConfig's rules).
    std::uint32_t workerThreads = 0;
    // Files written at once, at most (0: twice the workers).
    std::size_t window = 0;
    // An update every checkpointItems files done or every checkpointInterval.
    std::size_t checkpointItems = 64;
    std::chrono::milliseconds checkpointInterval{2000};
    // Progress callback, on the thread of run(); must not throw.
    std::chrono::milliseconds progressInterval{250};
    std::function<void(const RecoveryJobProgress&)> onProgress;
    // P16: told, on a worker, the path of each candidate's file once it is
    // created and before its data is written (RecoveryWriter's
    // FileCreatedCallback). An error fails the file (it is removed and
    // reported). Must be safe to call from several workers at once.
    std::function<Status(evaluation::EvaluatedCandidateId candidate, const std::filesystem::path& path)> onFileCreated;
    JobControl control;
    diagnostics::Logger* logger = nullptr;
};

enum class RecoveryJobOutcome : std::uint8_t {
    Completed,
    Cancelled,
};

struct RecoveryJobSummary {
    RecoveryJobOutcome outcome = RecoveryJobOutcome::Completed;
    RecoveryJobMetrics metrics;
};

// Writes candidates with RecoveryWriter, one writer per worker.
//
// Names: RecoveryWriter gives a file the first free name ("photo.jpg",
// "photo (1).jpg", ...), so the order in which files that could take the
// same name are written decides their names. The job writes such files in
// candidate order, and others in parallel: two candidates are written one
// after the other when their names, made safe and folded, could meet at the
// same place (the same name, or one's name and the other's directory, at the
// same level, " (n)" suffixes aside). A job, resumed or not, on any number of
// workers, names every file as one writer would, one file after the other.
//
// A file that cannot be written (no data located, a failing read, a
// destination error) is reported as failed and the job goes on; a
// cancellation stops it after the files being written (RecoveryWriter
// removes a file it could not finish).
//
// Thread safety: run() needs one owner; progress() may be called from any
// thread. The source and the candidates must outlive the run.
class RecoveryJob {
public:
    RecoveryJob(storage::IStorageSource& source, std::filesystem::path destination, RecoveryJobOptions options = {});
    ~RecoveryJob();
    RecoveryJob(const RecoveryJob&) = delete;
    RecoveryJob& operator=(const RecoveryJob&) = delete;
    RecoveryJob(RecoveryJob&&) = delete;
    RecoveryJob& operator=(RecoveryJob&&) = delete;

    // Writes every candidate not done in `resume` (a checkpoint of an earlier
    // run of the same job: same destination, candidates among these). Fails
    // with InvalidInput for invalid options, a source that is not open,
    // candidates with the same id, or a checkpoint of another job; with the
    // destination's error when it cannot be used; and with the sink's error.
    [[nodiscard]] Result<RecoveryJobSummary> run(std::span<const evaluation::EvaluatedCandidate> candidates,
                                                 const RecoveryJobUpdateSink& sink,
                                                 const RecoveryJobCheckpoint* resume = nullptr);

    [[nodiscard]] RecoveryJobProgress progress() const;
    [[nodiscard]] const RecoveryJobCheckpoint& checkpoint() const noexcept { return checkpoint_; }

    struct Run;

private:
    storage::IStorageSource& source_;
    std::filesystem::path destination_;
    RecoveryJobOptions options_;
    RecoveryJobCheckpoint checkpoint_;
    mutable std::mutex progressMutex_;
    RecoveryJobProgress progress_;
    // The source of the run under way (under progressMutex_).
    const ScanSource* liveSource_ = nullptr;
    std::atomic<bool> running_{false};
};

}  // namespace recovery::scan
