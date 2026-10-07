#pragma once

// What recovery jobs did with each candidate (P17), for a user interface
// that marks recovered files in its list: whether a job wrote it, where,
// and whether every byte was read; whether a job will write it; or why a
// job could not. It is built from a session's jobs (P16) or from any jobs'
// records.

#include "evaluation/evaluated_candidate.hpp"
#include "recovery/candidate_reader.hpp"
#include "recovery/error.hpp"
#include "scan/recovery_job.hpp"
#include "session/recovery_session.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::metadata {

enum class RecoveryState : std::uint8_t {
    // No job writes it.
    NotRecovered,
    // A job writes it and has not reached it yet (the job runs, is paused,
    // was cancelled or interrupted, or has not run).
    Pending,
    // A job wrote it.
    Recovered,
    // A job could not write it (no data located, a failing read, a
    // destination error), and no job wrote it.
    Failed,
};

[[nodiscard]] std::string_view toString(RecoveryState state) noexcept;

// What one job did with the candidate.
struct JobRecovery {
    std::uint32_t job = 0;
    // Pending, Recovered or Failed.
    RecoveryState state = RecoveryState::Pending;
    // Recovered: the file written, and what reconstruction found.
    std::filesystem::path path;
    std::optional<ReconstructionReport> report;
    // Recovered with every byte located and read (report->allBytesRead()).
    bool complete = false;
    // Failed: why.
    std::optional<Error> error;
};

struct CandidateRecovery {
    // Over every job, first match wins: Recovered, Pending, Failed, NotRecovered.
    RecoveryState state = RecoveryState::NotRecovered;
    // A job recovered it with every byte read.
    bool complete = false;
    // By job number.
    std::vector<JobRecovery> jobs;
};

// Thread safety: const members from any thread once built; addJob one owner.
class RecoveryJobIndex {
public:
    // The jobs of a session, as it holds them now (info() and each job's
    // recoveredItems(): a job running meanwhile may have done more since).
    [[nodiscard]] static RecoveryJobIndex fromSession(const session::RecoverySession& session);

    // Adds job `job`: the candidates it writes, and those it is done with
    // (written, or reported as failed). Items for candidates it does not
    // list count too. A job added twice replaces nothing: add each once.
    void addJob(std::uint32_t job, std::span<const evaluation::EvaluatedCandidateId> candidates,
                std::span<const scan::RecoveredItem> done);

    [[nodiscard]] CandidateRecovery recoveryOf(evaluation::EvaluatedCandidateId candidate) const;
    [[nodiscard]] RecoveryState stateOf(evaluation::EvaluatedCandidateId candidate) const;
    [[nodiscard]] std::size_t jobCount() const noexcept { return jobs_; }

private:
    // Candidate id -> its entries, by job number.
    std::map<std::uint64_t, std::vector<JobRecovery>> entries_;
    std::size_t jobs_ = 0;
};

}  // namespace recovery::metadata
