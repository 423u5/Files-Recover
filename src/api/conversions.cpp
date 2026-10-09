#include "conversions.hpp"

#include "recovery/text.hpp"
#include "report/text_format.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace recovery::api::detail {

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

ScanMode toApi(recovery::ScanMode mode) noexcept {
    return mode == recovery::ScanMode::Quick ? ScanMode::Quick : ScanMode::Deep;
}

recovery::ScanMode toEngine(ScanMode mode) noexcept {
    return mode == ScanMode::Quick ? recovery::ScanMode::Quick : recovery::ScanMode::Deep;
}

ScanStage toApi(scan::ScanStage stage) noexcept {
    switch (stage) {
    case scan::ScanStage::Volumes:
        return ScanStage::Volumes;
    case scan::ScanStage::Mp4Examination:
        return ScanStage::Mp4Examination;
    case scan::ScanStage::FragmentSeeds:
        return ScanStage::FragmentSeeds;
    case scan::ScanStage::SourcePass:
        return ScanStage::SourcePass;
    case scan::ScanStage::Mp4Delivery:
        return ScanStage::Mp4Delivery;
    case scan::ScanStage::Fragments:
        return ScanStage::Fragments;
    case scan::ScanStage::Evaluation:
        return ScanStage::Evaluation;
    case scan::ScanStage::Completed:
        return ScanStage::Completed;
    }
    return ScanStage::Volumes;
}

scan::ScanStage toEngine(ScanStage stage) noexcept {
    switch (stage) {
    case ScanStage::Volumes:
        return scan::ScanStage::Volumes;
    case ScanStage::Mp4Examination:
        return scan::ScanStage::Mp4Examination;
    case ScanStage::FragmentSeeds:
        return scan::ScanStage::FragmentSeeds;
    case ScanStage::SourcePass:
        return scan::ScanStage::SourcePass;
    case ScanStage::Mp4Delivery:
        return scan::ScanStage::Mp4Delivery;
    case ScanStage::Fragments:
        return scan::ScanStage::Fragments;
    case ScanStage::Evaluation:
        return scan::ScanStage::Evaluation;
    case ScanStage::Completed:
        return scan::ScanStage::Completed;
    }
    return scan::ScanStage::Volumes;
}

MediaKind toApi(metadata::MediaKind kind) noexcept {
    switch (kind) {
    case metadata::MediaKind::Image:
        return MediaKind::Image;
    case metadata::MediaKind::Audio:
        return MediaKind::Audio;
    case metadata::MediaKind::Video:
        return MediaKind::Video;
    case metadata::MediaKind::Unknown:
        break;
    }
    return MediaKind::Other;
}

FoundBy toApi(RecoveryMethod method) noexcept {
    switch (method) {
    case RecoveryMethod::Filesystem:
        return FoundBy::Filesystem;
    case RecoveryMethod::Carving:
        return FoundBy::Carving;
    case RecoveryMethod::Hybrid:
        return FoundBy::Hybrid;
    case RecoveryMethod::Fragmented:
        return FoundBy::Reconstruction;
    }
    return FoundBy::Filesystem;
}

Condition toApi(metadata::RecoveryCondition condition) noexcept {
    switch (condition) {
    case metadata::RecoveryCondition::Complete:
        return Condition::Complete;
    case metadata::RecoveryCondition::Unverified:
        return Condition::Unverified;
    case metadata::RecoveryCondition::Corrupted:
        return Condition::Corrupted;
    case metadata::RecoveryCondition::Partial:
        return Condition::Partial;
    case metadata::RecoveryCondition::Unrecoverable:
        return Condition::Unrecoverable;
    case metadata::RecoveryCondition::Ambiguous:
        return Condition::Ambiguous;
    }
    return Condition::Unverified;
}

ConditionReason toApi(metadata::ConditionReason reason) noexcept {
    switch (reason) {
    case metadata::ConditionReason::AlternativeLayout:
        return ConditionReason::AlternativeLayout;
    case metadata::ConditionReason::NothingLocated:
        return ConditionReason::NothingLocated;
    case metadata::ConditionReason::ReconstructionFailed:
        return ConditionReason::ReconstructionFailed;
    case metadata::ConditionReason::DataMissing:
        return ConditionReason::DataMissing;
    case metadata::ConditionReason::ContentTruncated:
        return ConditionReason::ContentTruncated;
    case metadata::ConditionReason::ReconstructionPartial:
        return ConditionReason::ReconstructionPartial;
    case metadata::ConditionReason::ValidationFailed:
        return ConditionReason::ValidationFailed;
    case metadata::ConditionReason::DataUnreadable:
        return ConditionReason::DataUnreadable;
    case metadata::ConditionReason::ClustersReallocated:
        return ConditionReason::ClustersReallocated;
    case metadata::ConditionReason::ReconstructionCorrupted:
        return ConditionReason::ReconstructionCorrupted;
    case metadata::ConditionReason::NotValidated:
        return ConditionReason::NotValidated;
    }
    return ConditionReason::NotValidated;
}

ValidationResult toApi(carving::ValidationStatus status) noexcept {
    switch (status) {
    case carving::ValidationStatus::NotValidated:
        return ValidationResult::NotValidated;
    case carving::ValidationStatus::Valid:
        return ValidationResult::Valid;
    case carving::ValidationStatus::Truncated:
        return ValidationResult::Truncated;
    case carving::ValidationStatus::Invalid:
        return ValidationResult::Invalid;
    }
    return ValidationResult::NotValidated;
}

CheckResult toApi(validation::LevelStatus status) noexcept {
    switch (status) {
    case validation::LevelStatus::NotRun:
        return CheckResult::NotRun;
    case validation::LevelStatus::Passed:
        return CheckResult::Passed;
    case validation::LevelStatus::Truncated:
        return CheckResult::Truncated;
    case validation::LevelStatus::Failed:
        return CheckResult::Failed;
    case validation::LevelStatus::NotApplicable:
        return CheckResult::NotApplicable;
    case validation::LevelStatus::Unsupported:
        return CheckResult::Unsupported;
    }
    return CheckResult::NotRun;
}

ValidationLevel toApi(validation::ValidationLevel level) noexcept {
    switch (level) {
    case validation::ValidationLevel::Structural:
        return ValidationLevel::Structural;
    case validation::ValidationLevel::Media:
        return ValidationLevel::Media;
    case validation::ValidationLevel::Playability:
        return ValidationLevel::Playability;
    }
    return ValidationLevel::Structural;
}

FilesystemKind toApi(filesystem::FilesystemType type) noexcept {
    switch (type) {
    case filesystem::FilesystemType::Fat32:
        return FilesystemKind::Fat32;
    case filesystem::FilesystemType::ExFat:
        return FilesystemKind::ExFat;
    case filesystem::FilesystemType::Ntfs:
        return FilesystemKind::Ntfs;
    }
    return FilesystemKind::Fat32;
}

PartitionScheme toApi(partition::PartitionScheme scheme) noexcept {
    switch (scheme) {
    case partition::PartitionScheme::Unknown:
        return PartitionScheme::None;
    case partition::PartitionScheme::Unpartitioned:
        return PartitionScheme::Unpartitioned;
    case partition::PartitionScheme::Mbr:
        return PartitionScheme::Mbr;
    case partition::PartitionScheme::Gpt:
        return PartitionScheme::Gpt;
    }
    return PartitionScheme::None;
}

ImageState toApi(imaging::ImageState state) noexcept {
    switch (state) {
    case imaging::ImageState::InProgress:
        return ImageState::InProgress;
    case imaging::ImageState::Completed:
        return ImageState::Completed;
    case imaging::ImageState::Cancelled:
        return ImageState::Cancelled;
    case imaging::ImageState::Failed:
        return ImageState::Failed;
    }
    return ImageState::InProgress;
}

PreviewKind toApi(metadata::PreviewKind kind) noexcept {
    switch (kind) {
    case metadata::PreviewKind::Content:
        return PreviewKind::Content;
    case metadata::PreviewKind::Thumbnail:
        return PreviewKind::Thumbnail;
    case metadata::PreviewKind::CoverArt:
        return PreviewKind::CoverArt;
    }
    return PreviewKind::Content;
}

LogLevel toApi(diagnostics::LogLevel level) noexcept {
    switch (level) {
    case diagnostics::LogLevel::Trace:
        return LogLevel::Trace;
    case diagnostics::LogLevel::Debug:
        return LogLevel::Debug;
    case diagnostics::LogLevel::Info:
        return LogLevel::Info;
    case diagnostics::LogLevel::Warning:
        return LogLevel::Warning;
    case diagnostics::LogLevel::Error:
        return LogLevel::Error;
    case diagnostics::LogLevel::Critical:
    case diagnostics::LogLevel::Off:
        break;
    }
    return LogLevel::Critical;
}

diagnostics::LogLevel toEngine(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return diagnostics::LogLevel::Trace;
    case LogLevel::Debug:
        return diagnostics::LogLevel::Debug;
    case LogLevel::Info:
        return diagnostics::LogLevel::Info;
    case LogLevel::Warning:
        return diagnostics::LogLevel::Warning;
    case LogLevel::Error:
        return diagnostics::LogLevel::Error;
    case LogLevel::Critical:
        return diagnostics::LogLevel::Critical;
    }
    return diagnostics::LogLevel::Info;
}

SourceKind toApi(storage::SourceType type) noexcept {
    return type == storage::SourceType::PhysicalDisk ? SourceKind::PhysicalDisk : SourceKind::DiskImage;
}

RunState runStateOf(const std::optional<session::SessionState>& state, bool interrupted) noexcept {
    if (!state.has_value()) {
        return RunState::NotStarted;
    }
    if (interrupted) {
        return RunState::Interrupted;
    }
    switch (*state) {
    case session::SessionState::Started:
        return RunState::Running;
    case session::SessionState::Paused:
        return RunState::Paused;
    case session::SessionState::Cancelled:
        return RunState::Cancelled;
    case session::SessionState::Completed:
        return RunState::Completed;
    case session::SessionState::Failed:
        return RunState::Failed;
    }
    return RunState::NotStarted;
}

// ---------------------------------------------------------------------------
// Scans
// ---------------------------------------------------------------------------

Result<scan::ScanConfiguration> configurationOf(const ScanSettings& settings) {
    scan::ScanConfiguration configuration;
    configuration.mode = toEngine(settings.mode);
    configuration.carving = settings.carving;
    configuration.mp4 = settings.mp4;
    configuration.fragments = settings.fragments;
    configuration.includeActive = settings.activeFiles;
    configuration.includeDeleted = settings.deletedFiles;
    configuration.alignment = settings.alignment;
    configuration.maxHits = settings.maxSignatures;
    configuration.sectorRetryCount = settings.sectorRetries;
    configuration.media = settings.mediaValidation;
    if (Status valid = scan::validate(configuration); !valid.ok()) {
        return valid.error();
    }
    return configuration;
}

ScanSettings settingsOf(const scan::ScanConfiguration& configuration, bool playability) {
    ScanSettings settings;
    settings.mode = toApi(configuration.mode);
    settings.carving = configuration.carving;
    settings.mp4 = configuration.mp4;
    settings.fragments = configuration.fragments;
    settings.activeFiles = configuration.includeActive;
    settings.deletedFiles = configuration.includeDeleted;
    settings.alignment = configuration.alignment;
    settings.maxSignatures = configuration.maxHits;
    settings.sectorRetries = configuration.sectorRetryCount;
    settings.mediaValidation = configuration.media;
    settings.playability = playability;
    settings.workerThreads = 0;
    return settings;
}

ScanMetrics toApi(const scan::ScanMetrics& metrics) {
    ScanMetrics out;
    out.sourceSize = metrics.sourceSize;
    out.bytesScanned = metrics.bytesScanned;
    out.bytesRead = metrics.bytesRead;
    out.bytesPerSecond = metrics.scanSpeed;
    out.filesFound = metrics.filesFound;
    out.carves = metrics.carves;
    out.mp4Candidates = metrics.mp4Candidates;
    out.reconstructions = metrics.fragmentCandidates;
    out.candidates = metrics.candidates;
    out.validationFailures = metrics.validationFailures;
    out.duplicates = metrics.duplicates;
    out.unreadableBytes = metrics.unreadableBytes;
    out.elapsed = metrics.elapsed;
    return out;
}

namespace {

// The stages of each mode in order, and their usual shares of a scan's time:
// a deep scan spends most of it in the pass over the source; the evaluation
// reads every candidate once more.
struct StageWeight {
    scan::ScanStage stage;
    double weight;
};

constexpr std::array<StageWeight, 2> kQuickStages = {{
    {scan::ScanStage::Volumes, 0.5},
    {scan::ScanStage::Evaluation, 0.5},
}};

constexpr std::array<StageWeight, 7> kDeepStages = {{
    {scan::ScanStage::Volumes, 0.05},
    {scan::ScanStage::Mp4Examination, 0.02},
    {scan::ScanStage::FragmentSeeds, 0.02},
    {scan::ScanStage::SourcePass, 0.70},
    {scan::ScanStage::Mp4Delivery, 0.01},
    {scan::ScanStage::Fragments, 0.05},
    {scan::ScanStage::Evaluation, 0.15},
}};

}  // namespace

std::pair<std::uint32_t, std::uint32_t> stageNumber(scan::ScanStage stage, recovery::ScanMode mode) noexcept {
    const auto number = [stage](const auto& stages) -> std::pair<std::uint32_t, std::uint32_t> {
        const auto count = static_cast<std::uint32_t>(stages.size());
        std::uint32_t index = 0;
        for (const StageWeight& entry : stages) {
            ++index;
            if (entry.stage == stage) {
                return {index, count};
            }
        }
        return {count, count};
    };
    return mode == recovery::ScanMode::Quick ? number(kQuickStages) : number(kDeepStages);
}

double scanFraction(scan::ScanStage stage, std::uint64_t done, std::uint64_t total, recovery::ScanMode mode) noexcept {
    if (stage == scan::ScanStage::Completed) {
        return 1.0;
    }
    const auto fraction = [&](const auto& stages) {
        double before = 0.0;
        for (const StageWeight& entry : stages) {
            if (entry.stage == stage) {
                const double part =
                    total == 0 ? 0.0 : static_cast<double>(std::min(done, total)) / static_cast<double>(total);
                return std::clamp(before + entry.weight * part, 0.0, 1.0);
            }
            before += entry.weight;
        }
        return std::clamp(before, 0.0, 1.0);
    };
    return mode == recovery::ScanMode::Quick ? fraction(kQuickStages) : fraction(kDeepStages);
}

ScanProgress scanProgressOf(const scan::ScanProgress& progress, recovery::ScanMode mode) {
    ScanProgress out;
    out.stage = toApi(progress.stage);
    const auto [number, count] = stageNumber(progress.stage, mode);
    out.stageNumber = number;
    out.stageCount = count;
    out.stageDone = progress.stageDone;
    out.stageTotal = progress.stageTotal;
    out.fraction = scanFraction(progress.stage, progress.stageDone, progress.stageTotal, mode);
    out.metrics = toApi(progress.metrics);
    return out;
}

ScanProgress scanProgressOf(const session::ScanStatus& status, recovery::ScanMode mode) {
    ScanProgress out;
    out.stage = toApi(status.stage);
    const auto [number, count] = stageNumber(status.stage, mode);
    out.stageNumber = number;
    out.stageCount = count;
    out.fraction = scanFraction(status.stage, 0, 0, mode);
    out.metrics = toApi(status.metrics);
    return out;
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

RecoveryMetrics toApi(const scan::RecoveryJobMetrics& metrics) {
    RecoveryMetrics out;
    out.files = metrics.files;
    out.recovered = metrics.recoveredFiles;
    out.failed = metrics.failedFiles;
    out.bytesRecovered = metrics.bytesRecovered;
    out.unreadableBytes = metrics.unreadableBytes;
    out.bytesRead = metrics.bytesRead;
    out.bytesPerSecond = metrics.speed;
    out.elapsed = metrics.elapsed;
    return out;
}

RecoveredFile recoveredFileOf(std::uint32_t job, const scan::RecoveredItem& item) {
    RecoveredFile out;
    out.candidate = CandidateId{item.candidate.value()};
    out.job = job;
    if (item.file.has_value()) {
        const ReconstructionReport& report = item.file->report;
        out.file = item.file->path;
        out.size = report.outputSize;
        out.missingBytes = report.missingBytes;
        out.unreadableBytes = report.unreadableBytes;
        out.complete = report.allBytesRead();
    }
    out.error = item.error;
    if (!item.file.has_value() && !out.error.has_value()) {
        out.error = makeError(ErrorCode::InternalError, "the file was not written, for a reason not recorded");
    }
    return out;
}

// ---------------------------------------------------------------------------
// Candidates
// ---------------------------------------------------------------------------

namespace {

FileTime fileTimeOf(const filesystem::Timestamp& timestamp) {
    return FileTime{timestamp.time, timestamp.local};
}

std::string colorModelName(metadata::ColorModel model) {
    switch (model) {
    case metadata::ColorModel::Grayscale:
        return "grayscale";
    case metadata::ColorModel::Rgb:
        return "rgb";
    case metadata::ColorModel::Indexed:
        return "indexed";
    case metadata::ColorModel::YCbCr:
        return "ycbcr";
    case metadata::ColorModel::Cmyk:
        return "cmyk";
    case metadata::ColorModel::Ycck:
        return "ycck";
    case metadata::ColorModel::Unknown:
        break;
    }
    return "unknown";
}

AudioDetails audioOf(const metadata::AudioStreamMetadata& audio) {
    AudioDetails out;
    out.codec = audio.codec;
    out.profile = audio.profile;
    out.sampleRate = audio.sampleRate;
    out.channels = audio.channels;
    out.bitsPerSample = audio.bitsPerSample;
    out.bitrate = audio.bitrate;
    out.variableBitrate = audio.variableBitrate;
    return out;
}

VideoDetails videoOf(const metadata::VideoStreamMetadata& video) {
    VideoDetails out;
    out.codec = video.codec;
    out.profile = video.profile;
    out.level = video.level;
    out.width = video.width;
    out.height = video.height;
    out.displayWidth = video.displayWidth;
    out.displayHeight = video.displayHeight;
    if (video.frameRate.has_value()) {
        out.frameRate = std::pair(video.frameRate->numerator, video.frameRate->denominator);
    }
    out.rotation = video.rotation;
    out.bitrate = video.bitrate;
    return out;
}

}  // namespace

CandidateInfo describeCandidate(const evaluation::EvaluatedCandidate& candidate) {
    const metadata::CandidateMetadata d = metadata::describeCandidate(candidate);
    CandidateInfo out;
    out.id = CandidateId{d.id.value()};
    out.name = d.name;
    out.path = d.path;
    out.extension = d.extension;
    out.kind = toApi(d.kind);
    out.format = d.formatId;
    out.mediaType = d.mediaType;
    out.size = d.size;
    out.expectedSize = d.expectedSize;
    out.deleted = d.deleted;
    out.foundBy = toApi(d.method);
    out.fragmented = d.fragments > 1;
    if (d.created.has_value()) {
        out.created = fileTimeOf(*d.created);
    }
    if (d.modified.has_value()) {
        out.modified = fileTimeOf(*d.modified);
    }
    out.condition = toApi(d.condition);
    for (const metadata::ConditionReason reason : d.reasons) {
        out.reasons.push_back(toApi(reason));
    }
    out.validation.result = toApi(d.validation);
    out.validation.structural = toApi(d.structural);
    out.validation.media = toApi(d.media);
    out.validation.playability = toApi(d.playability);
    if (d.deepestPassed.has_value()) {
        out.validation.deepestPassed = toApi(*d.deepestPassed);
    }
    if (d.sha256.has_value()) {
        out.sha256 = d.sha256->hex();
    }
    if (d.duplicateOf.has_value()) {
        out.duplicateOf = CandidateId{d.duplicateOf->value()};
    }
    if (d.container.has_value()) {
        out.container = CandidateId{d.container->value()};
    }
    out.unreadableBytes = candidate.unreadableBytes;
    return out;
}

MediaTime toApi(const metadata::MediaDateTime& time) {
    return MediaTime{time.iso8601(), time.utc()};
}

MediaDetails toApi(const metadata::MediaMetadata& metadata) {
    const auto timeOf = [](const std::optional<metadata::MediaDateTime>& time) -> std::optional<MediaTime> {
        return time.has_value() ? std::optional(toApi(*time)) : std::nullopt;
    };
    MediaDetails out;
    out.kind = toApi(metadata.kind);
    out.format = metadata.formatId;
    out.mediaType = metadata.mediaType;
    out.duration = metadata.duration;
    out.durationEstimated = metadata.durationEstimated;
    out.bitrate = metadata.bitrate;
    if (metadata.image.has_value()) {
        const metadata::ImageMetadata& image = *metadata.image;
        ImageDetails details;
        details.width = image.width;
        details.height = image.height;
        details.colorModel = colorModelName(image.color);
        details.channels = image.channels;
        details.bitsPerChannel = image.bitsPerChannel;
        details.bitsPerPixel = image.bitsPerPixel;
        details.alpha = image.alpha;
        details.frames = image.frames;
        details.loopCount = image.loopCount;
        details.progressive = image.progressive;
        if (image.orientation.has_value()) {
            details.rotation = metadata::rotationOf(*image.orientation);
            details.mirrored = metadata::mirrored(*image.orientation);
        }
        details.dateTaken = timeOf(image.dateTaken);
        details.cameraMake = image.cameraMake;
        details.cameraModel = image.cameraModel;
        out.image = std::move(details);
    }
    if (metadata.audio.has_value()) {
        out.audio = audioOf(*metadata.audio);
    }
    if (metadata.video.has_value()) {
        out.video = videoOf(*metadata.video);
    }
    if (metadata.movie.has_value()) {
        const metadata::MovieMetadata& movie = *metadata.movie;
        MovieDetails details;
        details.majorBrand = movie.majorBrand;
        details.compatibleBrands = movie.compatibleBrands;
        details.created = timeOf(movie.created);
        details.modified = timeOf(movie.modified);
        details.fragmented = movie.fragmented;
        for (const metadata::TrackMetadata& track : movie.tracks) {
            TrackDetails t;
            t.id = track.id;
            t.kind = toApi(track.kind);
            t.handler = track.handler;
            t.sampleEntry = track.sampleEntry;
            t.language = track.language;
            t.enabled = track.enabled;
            t.duration = track.duration;
            t.samples = track.samples;
            t.bytes = track.bytes;
            if (track.video.has_value()) {
                t.video = videoOf(*track.video);
            }
            if (track.audio.has_value()) {
                t.audio = audioOf(*track.audio);
            }
            details.tracks.push_back(std::move(t));
        }
        out.movie = std::move(details);
    }
    out.tags.title = metadata.tags.title;
    out.tags.artist = metadata.tags.artist;
    out.tags.album = metadata.tags.album;
    out.tags.genre = metadata.tags.genre;
    out.tags.date = timeOf(metadata.tags.date);
    out.tags.track = metadata.tags.track;
    out.tags.trackTotal = metadata.tags.trackTotal;
    for (const metadata::PreviewSource& preview : metadata.previews) {
        PreviewInfo info;
        info.kind = toApi(preview.kind);
        info.format = preview.formatId;
        info.mediaType = preview.mediaType;
        info.size = preview.length;
        info.width = preview.width;
        info.height = preview.height;
        if (preview.orientation.has_value()) {
            info.rotation = metadata::rotationOf(*preview.orientation);
            info.mirrored = metadata::mirrored(*preview.orientation);
        }
        if (preview.pictureType.has_value()) {
            info.pictureType = *preview.pictureType;
        }
        out.previews.push_back(std::move(info));
    }
    for (const metadata::MetadataIssue& issue : metadata.issues) {
        out.issues.push_back(issue.offset.has_value()
                                 ? issue.detail + " (at offset " + std::to_string(*issue.offset) + ")"
                                 : issue.detail);
    }
    out.issueCount = metadata.issueCount;
    return out;
}

// ---------------------------------------------------------------------------
// Sources and sessions
// ---------------------------------------------------------------------------

SourceDetails sourceDetailsOf(const storage::SourceInfo& info) {
    SourceDetails out;
    out.kind = toApi(info.type);
    out.path = toUtf8(info.path);
    out.disk = info.diskNumber;
    out.size = info.sizeBytes;
    out.sectorSize = info.logicalSectorSize;
    out.physicalSectorSize = info.physicalSectorSize;
    out.vendor = info.vendor;
    out.product = info.product;
    out.removable = info.removable;
    out.description = report::describeSource(info.type, out.path, info.diskNumber, info.vendor, info.product);
    return out;
}

SourceDetails sourceDetailsOf(const session::SessionSource& source) {
    SourceDetails out;
    out.kind = toApi(source.type);
    out.path = source.path;
    out.disk = source.diskNumber;
    out.size = source.size;
    out.sectorSize = source.sectorSize;
    out.physicalSectorSize = source.physicalSectorSize;
    out.vendor = source.vendor;
    out.product = source.product;
    out.removable = source.removable;
    out.description =
        report::describeSource(source.type, source.path, source.diskNumber, source.vendor, source.product);
    return out;
}

ImageFileInfo imageFileInfoOf(const imaging::ImageMetadata& metadata, const std::filesystem::path& metadataFile) {
    ImageFileInfo out;
    out.metadataFile = metadataFile;
    out.state = toApi(metadata.state);
    out.imagedFrom = report::describeSource(metadata.sourceType, metadata.sourcePath, std::nullopt,
                                            metadata.sourceVendor, metadata.sourceProduct);
    out.sourceSize = metadata.sourceSize;
    out.bytesImaged = metadata.bytesCompleted;
    out.sectorSize = metadata.sectorSize;
    for (const storage::BadRegion& region : metadata.badRegions) {
        out.unreadableBytes += region.length;
    }
    out.unreadableRegions = metadata.badRegions.size();
    out.started = metadata.startedUtc;
    out.updated = metadata.updatedUtc;
    return out;
}

std::string serialText(filesystem::FilesystemType type, std::uint64_t serial) {
    if (type == filesystem::FilesystemType::Ntfs) {
        return std::format("{:016X}", serial);
    }
    return std::format("{:04X}-{:04X}", (serial >> 16) & 0xFFFFu, serial & 0xFFFFu);
}

VolumeInfo volumeOf(const session::VolumeSummary& volume) {
    VolumeInfo out;
    out.offset = volume.offset;
    out.size = volume.size;
    out.partition = volume.partition;
    if (volume.filesystem.has_value()) {
        out.filesystem = toApi(*volume.filesystem);
        out.serialNumber = serialText(*volume.filesystem, volume.serialNumber);
    }
    out.label = volume.label;
    out.clusterSize = volume.clusterSize;
    out.scanned = volume.scanned;
    out.files = volume.files;
    out.error = volume.error;
    return out;
}

PartitionInfo partitionOf(const partition::Partition& partition) {
    PartitionInfo out;
    out.index = partition.index;
    out.offset = partition.offset;
    out.size = partition.size;
    out.type = partition.typeName;
    out.name = partition.name;
    out.bootable = partition.bootable;
    out.logical = partition.logical;
    out.truncated = partition.truncated;
    return out;
}

std::vector<StateChange> historyOf(const std::vector<session::StateChange>& history) {
    std::vector<StateChange> out;
    out.reserve(history.size());
    for (const session::StateChange& change : history) {
        out.push_back(StateChange{runStateOf(change.state, false), change.time, change.engineVersion, change.error});
    }
    return out;
}

JobInfo jobOf(const session::RecoveryJobStatus& job) {
    JobInfo out;
    out.id = job.id;
    if (Result<std::filesystem::path> destination = report::pathFromUtf8(job.destination); destination.ok()) {
        out.destination = std::move(destination).value();
    }
    out.created = job.created;
    out.state = runStateOf(job.state, job.interrupted);
    out.files = job.candidates.size();
    out.done = job.done;
    out.recovered = job.recovered;
    out.failed = job.failed;
    out.filesInProgress = job.filesInProgress;
    out.metrics = toApi(job.metrics);
    out.history = historyOf(job.history);
    return out;
}

SessionDetails sessionDetailsOf(const session::SessionInfo& info, const std::vector<session::SessionError>& errors,
                                const std::vector<storage::BadRegion>& unreadable) {
    SessionDetails out;
    out.id = info.id;
    out.folder = info.folder;
    out.engineVersion = info.engineVersion;
    out.created = info.created;
    out.updated = info.updated;
    out.source = sourceDetailsOf(info.source);
    out.settings = settingsOf(info.configuration, info.playability);
    out.scan.state = runStateOf(info.scan.state, info.scan.interrupted);
    out.scan.stage = toApi(info.scan.stage);
    out.scan.metrics = toApi(info.scan.metrics);
    out.scan.candidates = info.scan.candidates;
    out.scan.resumable = info.scan.runnable;
    out.scan.notResumable = info.scan.notRunnable;
    out.scan.history = historyOf(info.scan.history);
    out.partitionScheme = info.partitionScheme.has_value() ? toApi(*info.partitionScheme) : PartitionScheme::None;
    for (const session::VolumeSummary& volume : info.volumes) {
        out.volumes.push_back(volumeOf(volume));
    }
    for (const session::RecoveryJobStatus& job : info.jobs) {
        out.jobs.push_back(jobOf(job));
    }
    for (const session::SessionError& error : errors) {
        out.errors.push_back(SessionError{error.time, error.context, error.error});
    }
    for (const storage::BadRegion& region : unreadable) {
        out.unreadableBytes += region.length;
    }
    out.unreadableRegions = unreadable.size();
    for (const session::SessionDamage& damage : info.damage) {
        out.repairs.push_back(report::formatTime(damage.time) + ": the journal was damaged at offset " +
                              std::to_string(damage.damage.offset) + " (" + damage.damage.reason + "); " +
                              report::formatBytes(damage.damage.bytesDropped) + " and " +
                              std::to_string(damage.damage.recordsDropped) +
                              " intact records after it were dropped, a copy is kept as " + damage.damage.backup +
                              "; the work they recorded is done again");
    }
    if (info.tornBytesDropped != 0) {
        out.repairs.push_back("the last record was cut short (the program ended while writing it): " +
                              report::formatBytes(info.tornBytesDropped) + " dropped");
    }
    if (info.recordsSkipped != 0) {
        out.repairs.push_back(std::to_string(info.recordsSkipped) +
                              " records of a newer engine that this one may skip were skipped");
    }
    return out;
}

SessionListing listingOf(const session::SessionSummary& summary) {
    SessionListing out;
    out.id = summary.id;
    out.folder = summary.folder;
    out.error = summary.error;
    out.engineVersion = summary.engineVersion;
    out.created = summary.created;
    out.updated = summary.updated;
    out.sourceKind = toApi(summary.sourceType);
    out.sourcePath = summary.sourcePath;
    out.sourceSize = summary.sourceSize;
    out.sourceDescription =
        summary.error.has_value()
            ? std::string()
            : report::describeSource(summary.sourceType, summary.sourcePath, std::nullopt, {}, {});
    out.mode = toApi(summary.mode);
    out.scanState = runStateOf(summary.state, false);
    out.stage = toApi(summary.stage);
    out.candidates = summary.metrics.candidates;
    out.jobs = summary.jobs;
    return out;
}

}  // namespace recovery::api::detail
