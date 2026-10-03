#include "recovery/recovery_candidate.hpp"

#include "recovery/checked_math.hpp"

#include <algorithm>
#include <string>

namespace recovery {

std::string_view toString(RecoveryMethod method) noexcept {
    switch (method) {
    case RecoveryMethod::Filesystem:
        return "FILESYSTEM";
    case RecoveryMethod::Carving:
        return "CARVING";
    case RecoveryMethod::Hybrid:
        return "HYBRID";
    case RecoveryMethod::Fragmented:
        return "FRAGMENTED";
    }
    return "Unknown";
}

std::string_view toString(RegionKind kind) noexcept {
    switch (kind) {
    case RegionKind::Stored:
        return "Stored";
    case RegionKind::Embedded:
        return "Embedded";
    case RegionKind::Zeros:
        return "Zeros";
    case RegionKind::Missing:
        return "Missing";
    }
    return "Unknown";
}

std::string_view toString(LayoutEvidence evidence) noexcept {
    switch (evidence) {
    case LayoutEvidence::None:
        return "None";
    case LayoutEvidence::Recorded:
        return "Recorded";
    case LayoutEvidence::Guessed:
        return "Guessed";
    }
    return "Unknown";
}

std::string_view toString(CandidateWarning warning) noexcept {
    switch (warning) {
    case CandidateWarning::LayoutGuessed:
        return "LayoutGuessed";
    case CandidateWarning::DataMissing:
        return "DataMissing";
    case CandidateWarning::ClustersReallocated:
        return "ClustersReallocated";
    case CandidateWarning::DataNotDecoded:
        return "DataNotDecoded";
    case CandidateWarning::CrossLinked:
        return "CrossLinked";
    case CandidateWarning::AllocationDamaged:
        return "AllocationDamaged";
    case CandidateWarning::AllocationUnknown:
        return "AllocationUnknown";
    case CandidateWarning::NameUncertain:
        return "NameUncertain";
    case CandidateWarning::LocationUnknown:
        return "LocationUnknown";
    case CandidateWarning::MetadataDamaged:
        return "MetadataDamaged";
    }
    return "Unknown";
}

bool AllocationInfo::hasIssue(filesystem::AllocationIssue issue) const noexcept {
    return std::find(issues.begin(), issues.end(), issue) != issues.end();
}

bool FilesystemEvidence::hasIssue(filesystem::EntryIssue issue) const noexcept {
    return std::find(entryIssues.begin(), entryIssues.end(), issue) != entryIssues.end();
}

bool RecoveryCandidate::isDeleted() const noexcept {
    return filesystemEvidence.state == filesystem::EntryState::Deleted || filesystemEvidence.parentDeleted;
}

bool RecoveryCandidate::hasWarning(CandidateWarning warning) const noexcept {
    return std::find(warnings.begin(), warnings.end(), warning) != warnings.end();
}

std::optional<std::uint64_t> RecoveryCandidate::sourceOffset() const noexcept {
    for (const SourceRegion& region : sourceRegions) {
        if (region.kind == RegionKind::Stored) {
            return region.sourceOffset;
        }
    }
    return std::nullopt;
}

std::uint64_t RecoveryCandidate::bytes(RegionKind kind) const noexcept {
    std::uint64_t total = 0;
    for (const SourceRegion& region : sourceRegions) {
        if (region.kind == kind) {
            total += region.length;
        }
    }
    return total;
}

std::uint64_t RecoveryCandidate::reallocatedBytes() const noexcept {
    std::uint64_t total = 0;
    for (const SourceRegion& region : sourceRegions) {
        if (region.kind == RegionKind::Stored && region.reallocated) {
            total += region.length;
        }
    }
    return total;
}

Status validateCandidate(const RecoveryCandidate& candidate) {
    const auto invalid = [&](const std::string& what) {
        return makeError(ErrorCode::InvalidInput, "candidate " + std::to_string(candidate.id.value()) + ": " + what);
    };
    std::uint64_t position = 0;
    for (const SourceRegion& region : candidate.sourceRegions) {
        if (region.fileOffset != position) {
            return invalid("regions are not contiguous at file offset " + std::to_string(position));
        }
        if (region.length == 0) {
            return invalid("empty region at file offset " + std::to_string(position));
        }
        const std::optional<std::uint64_t> end = checkedAdd(region.fileOffset, region.length);
        if (!end.has_value()) {
            return invalid("region length overflows");
        }
        switch (region.kind) {
        case RegionKind::Stored:
            if (!checkedAdd(region.sourceOffset, region.length).has_value()) {
                return invalid("stored region overflows the source offset range");
            }
            break;
        case RegionKind::Embedded:
            if (!rangeWithin<std::uint64_t>(region.sourceOffset, region.length, candidate.embeddedData.size())) {
                return invalid("embedded region lies outside the embedded data");
            }
            break;
        case RegionKind::Zeros:
        case RegionKind::Missing:
            break;
        default:
            return invalid("unknown region kind");
        }
        if (region.reallocated && region.kind != RegionKind::Stored) {
            return invalid("only stored regions can be reallocated");
        }
        position = *end;
    }
    if (position != candidate.expectedSize) {
        return invalid("regions cover " + std::to_string(position) + " bytes, expected " +
                       std::to_string(candidate.expectedSize));
    }
    return success();
}

}  // namespace recovery
