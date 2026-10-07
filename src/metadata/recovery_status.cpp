#include "metadata/recovery_status.hpp"

#include <algorithm>
#include <set>

namespace recovery::metadata {

std::string_view toString(RecoveryState state) noexcept {
    switch (state) {
        case RecoveryState::NotRecovered:
            return "not recovered";
        case RecoveryState::Pending:
            return "pending";
        case RecoveryState::Recovered:
            return "recovered";
        case RecoveryState::Failed:
            return "failed";
    }
    return "not recovered";
}

RecoveryJobIndex RecoveryJobIndex::fromSession(const session::RecoverySession& session) {
    RecoveryJobIndex index;
    const session::SessionInfo info = session.info();
    for (const session::RecoveryJobStatus& job : info.jobs) {
        const std::vector<scan::RecoveredItem> done = session.recoveredItems(job.id);
        index.addJob(job.id, job.candidates, done);
    }
    return index;
}

void RecoveryJobIndex::addJob(std::uint32_t job, std::span<const evaluation::EvaluatedCandidateId> candidates,
                              std::span<const scan::RecoveredItem> done) {
    ++jobs_;
    std::set<std::uint64_t> touched;
    for (const scan::RecoveredItem& item : done) {
        if (!touched.insert(item.candidate.value()).second) {
            continue;
        }
        JobRecovery entry;
        entry.job = job;
        if (item.file.has_value()) {
            entry.state = RecoveryState::Recovered;
            entry.path = item.file->path;
            entry.report = item.file->report;
            entry.complete = item.file->report.allBytesRead();
        } else {
            entry.state = RecoveryState::Failed;
            entry.error = item.error;
        }
        entries_[item.candidate.value()].push_back(std::move(entry));
    }
    for (const evaluation::EvaluatedCandidateId candidate : candidates) {
        if (!touched.insert(candidate.value()).second) {
            continue;
        }
        JobRecovery entry;
        entry.job = job;
        entry.state = RecoveryState::Pending;
        entries_[candidate.value()].push_back(std::move(entry));
    }
    for (const std::uint64_t id : touched) {
        std::vector<JobRecovery>& list = entries_[id];
        std::stable_sort(list.begin(), list.end(),
                         [](const JobRecovery& a, const JobRecovery& b) { return a.job < b.job; });
    }
}

CandidateRecovery RecoveryJobIndex::recoveryOf(evaluation::EvaluatedCandidateId candidate) const {
    CandidateRecovery recovery;
    const auto found = entries_.find(candidate.value());
    if (found == entries_.end()) {
        return recovery;
    }
    recovery.jobs = found->second;
    const auto has = [&](RecoveryState state) {
        return std::any_of(recovery.jobs.begin(), recovery.jobs.end(),
                           [&](const JobRecovery& entry) { return entry.state == state; });
    };
    if (has(RecoveryState::Recovered)) {
        recovery.state = RecoveryState::Recovered;
    } else if (has(RecoveryState::Pending)) {
        recovery.state = RecoveryState::Pending;
    } else if (has(RecoveryState::Failed)) {
        recovery.state = RecoveryState::Failed;
    }
    recovery.complete = std::any_of(recovery.jobs.begin(), recovery.jobs.end(), [](const JobRecovery& entry) {
        return entry.state == RecoveryState::Recovered && entry.complete;
    });
    return recovery;
}

RecoveryState RecoveryJobIndex::stateOf(evaluation::EvaluatedCandidateId candidate) const {
    return recoveryOf(candidate).state;
}

}  // namespace recovery::metadata
