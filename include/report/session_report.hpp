#pragma once

// The session report: everything a session holds, as text for people and as
// JSON for programs; `recovery report` (P18) and the API's exportReport() (P19)
// write it. Built from the engine's public API only: the session's
// information, candidates, recovered files, errors and unreadable regions
// (P16), and the candidate descriptions, duplicate groups and recovery
// status of P17.
//
// The JSON document ("format": "recovery-session-report", "formatVersion": 1)
// names states, conditions and levels as the engine does (toString); see
// docs/recovery/cli.md for its members.
//
// Thread safety: the functions share no state. gatherReport() reads the
// session through its thread-safe accessors, also while an operation runs.

#include "evaluation/evaluated_candidate.hpp"
#include "metadata/candidate_metadata.hpp"
#include "metadata/recovery_status.hpp"
#include "scan/recovery_job.hpp"
#include "session/recovery_session.hpp"
#include "storage/bad_region.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace recovery::report {

inline constexpr std::uint32_t kReportFormatVersion = 1;

struct SessionReport {
    session::SessionInfo info;
    std::vector<evaluation::EvaluatedCandidate> candidates;
    // describeCandidate() of each candidate, in the same order.
    std::vector<metadata::CandidateMetadata> described;
    metadata::DuplicateGroups duplicates;
    metadata::RecoveryJobIndex recovery;
    // Each job's files: written, or why not.
    std::map<std::uint32_t, std::vector<scan::RecoveredItem>> recovered;
    std::vector<session::SessionError> errors;
    std::vector<storage::BadRegion> unreadable;
};

[[nodiscard]] SessionReport gatherReport(const session::RecoverySession& session);

// Text: the session, its source, scan, volumes, candidates (with
// `details`: each candidate's evidence too), duplicates, recovery jobs with
// every file, errors, unreadable regions and journal damage.
[[nodiscard]] std::string textReport(const SessionReport& report, bool details);
[[nodiscard]] std::string jsonReport(const SessionReport& report);

// A session another command has open: what its summary says (read without
// opening it).
[[nodiscard]] std::string textSummary(const session::SessionSummary& summary);
[[nodiscard]] std::string jsonSummary(const session::SessionSummary& summary);

// The sessions below `root`.
[[nodiscard]] std::string textSessionList(const std::filesystem::path& root,
                                          const std::vector<session::SessionSummary>& sessions);
[[nodiscard]] std::string jsonSessionList(const std::filesystem::path& root,
                                          const std::vector<session::SessionSummary>& sessions);

}  // namespace recovery::report
