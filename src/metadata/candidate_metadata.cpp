#include "metadata/candidate_metadata.hpp"

#include <algorithm>
#include <array>

namespace recovery::metadata {

namespace {

constexpr std::size_t kReasonCount = 11;

bool anyLevel(const validation::ValidationState& state, validation::LevelStatus status) noexcept {
    return state.structural.status == status || state.media.status == status || state.playability.status == status;
}

}  // namespace

std::string_view toString(RecoveryCondition condition) noexcept {
    switch (condition) {
        case RecoveryCondition::Complete:
            return "COMPLETE";
        case RecoveryCondition::Unverified:
            return "UNVERIFIED";
        case RecoveryCondition::Corrupted:
            return "CORRUPTED";
        case RecoveryCondition::Partial:
            return "PARTIAL";
        case RecoveryCondition::Unrecoverable:
            return "UNRECOVERABLE";
        case RecoveryCondition::Ambiguous:
            return "AMBIGUOUS";
    }
    return "COMPLETE";
}

std::string_view toString(ConditionReason reason) noexcept {
    switch (reason) {
        case ConditionReason::AlternativeLayout:
            return "ALTERNATIVE_LAYOUT";
        case ConditionReason::NothingLocated:
            return "NOTHING_LOCATED";
        case ConditionReason::ReconstructionFailed:
            return "RECONSTRUCTION_FAILED";
        case ConditionReason::DataMissing:
            return "DATA_MISSING";
        case ConditionReason::ContentTruncated:
            return "CONTENT_TRUNCATED";
        case ConditionReason::ReconstructionPartial:
            return "RECONSTRUCTION_PARTIAL";
        case ConditionReason::ValidationFailed:
            return "VALIDATION_FAILED";
        case ConditionReason::DataUnreadable:
            return "DATA_UNREADABLE";
        case ConditionReason::ClustersReallocated:
            return "CLUSTERS_REALLOCATED";
        case ConditionReason::ReconstructionCorrupted:
            return "RECONSTRUCTION_CORRUPTED";
        case ConditionReason::NotValidated:
            return "NOT_VALIDATED";
    }
    return "NOT_VALIDATED";
}

bool ConditionAssessment::has(ConditionReason reason) const noexcept {
    return std::find(reasons.begin(), reasons.end(), reason) != reasons.end();
}

ConditionAssessment assessCondition(const evaluation::EvaluatedCandidate& candidate) {
    using evaluation::EvaluationWarning;
    const RecoveryCandidate& data = candidate.data;
    const std::optional<ReconstructionStatus> reconstruction =
        candidate.fragments.has_value() ? std::optional<ReconstructionStatus>(candidate.fragments->status)
                                        : std::nullopt;
    const carving::ValidationStatus status = candidate.validation.status();
    const std::uint64_t missing = data.bytes(RegionKind::Missing);

    std::array<bool, kReasonCount> facts{};
    const auto set = [&](ConditionReason reason, bool fact) {
        facts[static_cast<std::size_t>(reason)] = facts[static_cast<std::size_t>(reason)] || fact;
    };
    set(ConditionReason::AlternativeLayout, candidate.hasWarning(EvaluationWarning::AlternativeLayout) ||
                                                reconstruction == ReconstructionStatus::Ambiguous);
    set(ConditionReason::NothingLocated, data.expectedSize > 0 && missing >= data.expectedSize);
    set(ConditionReason::ReconstructionFailed, candidate.hasWarning(EvaluationWarning::ReconstructionFailed) ||
                                                   reconstruction == ReconstructionStatus::Unrecoverable);
    set(ConditionReason::DataMissing, missing > 0);
    set(ConditionReason::ContentTruncated, anyLevel(candidate.validation, validation::LevelStatus::Truncated));
    set(ConditionReason::ReconstructionPartial, reconstruction == ReconstructionStatus::Partial);
    set(ConditionReason::ValidationFailed, status == carving::ValidationStatus::Invalid);
    set(ConditionReason::DataUnreadable, candidate.unreadableBytes > 0);
    set(ConditionReason::ClustersReallocated, data.reallocatedBytes() > 0);
    set(ConditionReason::ReconstructionCorrupted, reconstruction == ReconstructionStatus::Corrupted);
    set(ConditionReason::NotValidated, status == carving::ValidationStatus::NotValidated);

    ConditionAssessment assessment;
    for (std::size_t i = 0; i < kReasonCount; ++i) {
        if (facts[i]) {
            assessment.reasons.push_back(static_cast<ConditionReason>(i));
        }
    }
    const auto any = [&](std::initializer_list<ConditionReason> reasons) {
        return std::any_of(reasons.begin(), reasons.end(),
                           [&](ConditionReason reason) { return facts[static_cast<std::size_t>(reason)]; });
    };
    if (any({ConditionReason::AlternativeLayout})) {
        assessment.condition = RecoveryCondition::Ambiguous;
    } else if (any({ConditionReason::NothingLocated, ConditionReason::ReconstructionFailed})) {
        assessment.condition = RecoveryCondition::Unrecoverable;
    } else if (any({ConditionReason::DataMissing, ConditionReason::ContentTruncated,
                    ConditionReason::ReconstructionPartial})) {
        assessment.condition = RecoveryCondition::Partial;
    } else if (any({ConditionReason::ValidationFailed, ConditionReason::DataUnreadable,
                    ConditionReason::ClustersReallocated, ConditionReason::ReconstructionCorrupted})) {
        assessment.condition = RecoveryCondition::Corrupted;
    } else if (any({ConditionReason::NotValidated})) {
        assessment.condition = RecoveryCondition::Unverified;
    } else {
        assessment.condition = RecoveryCondition::Complete;
    }
    return assessment;
}

CandidateMetadata describeCandidate(const evaluation::EvaluatedCandidate& candidate) {
    const RecoveryCandidate& data = candidate.data;
    CandidateMetadata out;
    out.id = candidate.id;
    out.name = data.filename;
    out.extension = data.extension;
    out.method = data.method;
    if (candidate.hasFilesystemEvidence()) {
        out.path = data.filesystemEvidence.path;
        out.deleted = data.isDeleted();
        out.created = data.filesystemEvidence.created;
        out.modified = data.filesystemEvidence.modified;
    }
    out.kind = kindOfFormat(candidate.formatId);
    out.formatId = candidate.formatId;
    out.mediaType = std::string(mediaTypeOfFormat(candidate.formatId));
    out.size = candidate.recoveredSize();
    out.expectedSize = data.expectedSize;
    out.sourceOffset = candidate.sourceOffset();
    out.fragments = candidate.fragmentationCount();
    out.validation = candidate.validationStatus();
    out.structural = candidate.validation.structural.status;
    out.media = candidate.validation.media.status;
    out.playability = candidate.validation.playability.status;
    out.deepestPassed = candidate.validation.deepestPassed();
    ConditionAssessment assessment = assessCondition(candidate);
    out.condition = assessment.condition;
    out.reasons = std::move(assessment.reasons);
    out.sha256 = candidate.identity.sha256;
    out.duplicateOf = candidate.duplicateOf;
    out.container = candidate.container;
    return out;
}

DuplicateGroups DuplicateGroups::build(std::span<const evaluation::EvaluatedCandidate> candidates) {
    DuplicateGroups result;
    // Original id -> its group.
    std::map<std::uint64_t, std::size_t> byOriginal;
    for (const evaluation::EvaluatedCandidate& candidate : candidates) {
        if (!candidate.duplicateOf.has_value() || *candidate.duplicateOf == candidate.id) {
            continue;
        }
        const std::uint64_t original = candidate.duplicateOf->value();
        auto [at, inserted] = byOriginal.try_emplace(original, result.groups_.size());
        if (inserted) {
            DuplicateGroup group;
            group.members.push_back(*candidate.duplicateOf);
            group.sha256 = candidate.identity.sha256;
            group.size = candidate.identity.size;
            result.groups_.push_back(std::move(group));
        }
        result.groups_[at->second].members.push_back(candidate.id);
    }
    for (DuplicateGroup& group : result.groups_) {
        std::sort(group.members.begin() + 1, group.members.end());
        group.members.erase(std::unique(group.members.begin() + 1, group.members.end()), group.members.end());
    }
    std::sort(result.groups_.begin(), result.groups_.end(),
              [](const DuplicateGroup& a, const DuplicateGroup& b) { return a.original() < b.original(); });
    for (std::size_t i = 0; i < result.groups_.size(); ++i) {
        for (const evaluation::EvaluatedCandidateId member : result.groups_[i].members) {
            result.index_.try_emplace(member.value(), i);
        }
    }
    return result;
}

const DuplicateGroup* DuplicateGroups::groupOf(evaluation::EvaluatedCandidateId id) const noexcept {
    const auto found = index_.find(id.value());
    return found == index_.end() ? nullptr : &groups_[found->second];
}

std::size_t DuplicateGroups::duplicateCount() const noexcept {
    std::size_t count = 0;
    for (const DuplicateGroup& group : groups_) {
        count += group.members.size() - 1;
    }
    return count;
}

}  // namespace recovery::metadata
