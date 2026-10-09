#include "api/api_types.hpp"

#include "report/text_format.hpp"

namespace recovery::api {

SourceRef SourceRef::physicalDisk(std::uint32_t number) {
    SourceRef source;
    source.kind = SourceKind::PhysicalDisk;
    source.disk = number;
    return source;
}

SourceRef SourceRef::imageFile(std::filesystem::path path, std::uint32_t sectorSize) {
    SourceRef source;
    source.kind = SourceKind::DiskImage;
    source.image = std::move(path);
    source.sectorSize = sectorSize;
    return source;
}

bool CandidateFilter::selectsAll() const noexcept {
    return kinds.empty() && conditions.empty() && recovery.empty() && !deleted.has_value() && !skipDuplicates;
}

std::string_view toString(SourceKind kind) noexcept {
    switch (kind) {
    case SourceKind::PhysicalDisk:
        return "physical-disk";
    case SourceKind::DiskImage:
        return "disk-image";
    }
    return "unknown";
}

std::string_view toString(ImageState state) noexcept {
    switch (state) {
    case ImageState::InProgress:
        return "in-progress";
    case ImageState::Completed:
        return "completed";
    case ImageState::Cancelled:
        return "cancelled";
    case ImageState::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view toString(PartitionScheme scheme) noexcept {
    switch (scheme) {
    case PartitionScheme::None:
        return "none";
    case PartitionScheme::Unpartitioned:
        return "unpartitioned";
    case PartitionScheme::Mbr:
        return "mbr";
    case PartitionScheme::Gpt:
        return "gpt";
    }
    return "unknown";
}

std::string_view toString(FilesystemKind kind) noexcept {
    switch (kind) {
    case FilesystemKind::Fat32:
        return "fat32";
    case FilesystemKind::ExFat:
        return "exfat";
    case FilesystemKind::Ntfs:
        return "ntfs";
    }
    return "unknown";
}

std::string_view toString(ScanMode mode) noexcept {
    switch (mode) {
    case ScanMode::Quick:
        return "quick";
    case ScanMode::Deep:
        return "deep";
    }
    return "unknown";
}

std::string_view toString(ScanStage stage) noexcept {
    switch (stage) {
    case ScanStage::Volumes:
        return "volumes";
    case ScanStage::Mp4Examination:
        return "mp4-examination";
    case ScanStage::FragmentSeeds:
        return "fragment-seeds";
    case ScanStage::SourcePass:
        return "source-pass";
    case ScanStage::Mp4Delivery:
        return "mp4-delivery";
    case ScanStage::Fragments:
        return "fragments";
    case ScanStage::Evaluation:
        return "evaluation";
    case ScanStage::Completed:
        return "completed";
    }
    return "unknown";
}

std::string_view toString(OperationKind kind) noexcept {
    switch (kind) {
    case OperationKind::None:
        return "none";
    case OperationKind::Scan:
        return "scan";
    case OperationKind::Recovery:
        return "recovery";
    case OperationKind::Imaging:
        return "imaging";
    }
    return "unknown";
}

std::string_view toString(OperationState state) noexcept {
    switch (state) {
    case OperationState::Idle:
        return "idle";
    case OperationState::Running:
        return "running";
    case OperationState::Paused:
        return "paused";
    case OperationState::Completed:
        return "completed";
    case OperationState::Cancelled:
        return "cancelled";
    case OperationState::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view toString(MediaKind kind) noexcept {
    switch (kind) {
    case MediaKind::Other:
        return "other";
    case MediaKind::Image:
        return "image";
    case MediaKind::Audio:
        return "audio";
    case MediaKind::Video:
        return "video";
    }
    return "unknown";
}

std::string_view toString(FoundBy method) noexcept {
    switch (method) {
    case FoundBy::Filesystem:
        return "filesystem";
    case FoundBy::Carving:
        return "carving";
    case FoundBy::Hybrid:
        return "hybrid";
    case FoundBy::Reconstruction:
        return "reconstruction";
    }
    return "unknown";
}

std::string_view toString(Condition condition) noexcept {
    switch (condition) {
    case Condition::Complete:
        return "complete";
    case Condition::Unverified:
        return "unverified";
    case Condition::Corrupted:
        return "corrupted";
    case Condition::Partial:
        return "partial";
    case Condition::Unrecoverable:
        return "unrecoverable";
    case Condition::Ambiguous:
        return "ambiguous";
    }
    return "unknown";
}

std::string_view toString(ConditionReason reason) noexcept {
    switch (reason) {
    case ConditionReason::AlternativeLayout:
        return "alternative-layout";
    case ConditionReason::NothingLocated:
        return "nothing-located";
    case ConditionReason::ReconstructionFailed:
        return "reconstruction-failed";
    case ConditionReason::DataMissing:
        return "data-missing";
    case ConditionReason::ContentTruncated:
        return "content-truncated";
    case ConditionReason::ReconstructionPartial:
        return "reconstruction-partial";
    case ConditionReason::ValidationFailed:
        return "validation-failed";
    case ConditionReason::DataUnreadable:
        return "data-unreadable";
    case ConditionReason::ClustersReallocated:
        return "clusters-reallocated";
    case ConditionReason::ReconstructionCorrupted:
        return "reconstruction-corrupted";
    case ConditionReason::NotValidated:
        return "not-validated";
    }
    return "unknown";
}

std::string_view toString(ValidationResult result) noexcept {
    switch (result) {
    case ValidationResult::NotValidated:
        return "not-validated";
    case ValidationResult::Valid:
        return "valid";
    case ValidationResult::Truncated:
        return "truncated";
    case ValidationResult::Invalid:
        return "invalid";
    }
    return "unknown";
}

std::string_view toString(CheckResult result) noexcept {
    switch (result) {
    case CheckResult::NotRun:
        return "not-run";
    case CheckResult::Passed:
        return "passed";
    case CheckResult::Truncated:
        return "truncated";
    case CheckResult::Failed:
        return "failed";
    case CheckResult::NotApplicable:
        return "not-applicable";
    case CheckResult::Unsupported:
        return "unsupported";
    }
    return "unknown";
}

std::string_view toString(ValidationLevel level) noexcept {
    switch (level) {
    case ValidationLevel::Structural:
        return "structural";
    case ValidationLevel::Media:
        return "media";
    case ValidationLevel::Playability:
        return "playability";
    }
    return "unknown";
}

std::string_view toString(RecoveryState state) noexcept {
    switch (state) {
    case RecoveryState::NotRecovered:
        return "not-recovered";
    case RecoveryState::Pending:
        return "pending";
    case RecoveryState::Recovered:
        return "recovered";
    case RecoveryState::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view toString(PreviewKind kind) noexcept {
    switch (kind) {
    case PreviewKind::Content:
        return "content";
    case PreviewKind::Thumbnail:
        return "thumbnail";
    case PreviewKind::CoverArt:
        return "cover-art";
    }
    return "unknown";
}

std::string_view toString(RunState state) noexcept {
    switch (state) {
    case RunState::NotStarted:
        return "not-started";
    case RunState::Running:
        return "running";
    case RunState::Paused:
        return "paused";
    case RunState::Completed:
        return "completed";
    case RunState::Cancelled:
        return "cancelled";
    case RunState::Failed:
        return "failed";
    case RunState::Interrupted:
        return "interrupted";
    }
    return "unknown";
}

std::string_view toString(ReportFormat format) noexcept {
    switch (format) {
    case ReportFormat::Json:
        return "json";
    case ReportFormat::Text:
        return "text";
    }
    return "unknown";
}

std::string_view toString(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::OperationStarted:
        return "operation-started";
    case EventKind::Progress:
        return "progress";
    case EventKind::Paused:
        return "paused";
    case EventKind::Resumed:
        return "resumed";
    case EventKind::CandidatesFound:
        return "candidates-found";
    case EventKind::FilesRecovered:
        return "files-recovered";
    case EventKind::OperationFinished:
        return "operation-finished";
    }
    return "unknown";
}

std::string_view toString(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    case LogLevel::Critical:
        return "critical";
    }
    return "unknown";
}

std::string formatSize(std::uint64_t bytes) {
    return report::formatSize(bytes);
}

}  // namespace recovery::api
