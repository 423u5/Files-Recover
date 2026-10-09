#pragma once

// The API's boundary (P19): the engine's types turned into the API's plain
// values, and the API's scan settings into the engine's configuration.
// Nothing of a filesystem's structures, clusters or carving evidence crosses
// it (candidate explanations go across as text only).

#include "api/api_types.hpp"
#include "diagnostics/logger.hpp"
#include "evaluation/evaluated_candidate.hpp"
#include "filesystem/filesystem.hpp"
#include "imaging/image_metadata.hpp"
#include "imaging/image_writer.hpp"
#include "metadata/candidate_metadata.hpp"
#include "metadata/media_metadata.hpp"
#include "partition/partition_table.hpp"
#include "recovery/config.hpp"
#include "recovery/recovery_writer.hpp"
#include "scan/recovery_job.hpp"
#include "scan/scan_coordinator.hpp"
#include "scan/scan_state.hpp"
#include "session/recovery_session.hpp"
#include "session/session_types.hpp"
#include "storage/bad_region.hpp"
#include "storage/storage_source.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace recovery::api::detail {

// ---- Values ----

[[nodiscard]] ScanMode toApi(recovery::ScanMode mode) noexcept;
[[nodiscard]] recovery::ScanMode toEngine(ScanMode mode) noexcept;
[[nodiscard]] ScanStage toApi(scan::ScanStage stage) noexcept;
[[nodiscard]] scan::ScanStage toEngine(ScanStage stage) noexcept;
[[nodiscard]] MediaKind toApi(metadata::MediaKind kind) noexcept;
[[nodiscard]] FoundBy toApi(RecoveryMethod method) noexcept;
[[nodiscard]] Condition toApi(metadata::RecoveryCondition condition) noexcept;
[[nodiscard]] ConditionReason toApi(metadata::ConditionReason reason) noexcept;
[[nodiscard]] ValidationResult toApi(carving::ValidationStatus status) noexcept;
[[nodiscard]] CheckResult toApi(validation::LevelStatus status) noexcept;
[[nodiscard]] ValidationLevel toApi(validation::ValidationLevel level) noexcept;
[[nodiscard]] FilesystemKind toApi(filesystem::FilesystemType type) noexcept;
[[nodiscard]] PartitionScheme toApi(partition::PartitionScheme scheme) noexcept;
[[nodiscard]] ImageState toApi(imaging::ImageState state) noexcept;
[[nodiscard]] PreviewKind toApi(metadata::PreviewKind kind) noexcept;
[[nodiscard]] LogLevel toApi(diagnostics::LogLevel level) noexcept;
[[nodiscard]] diagnostics::LogLevel toEngine(LogLevel level) noexcept;
// A source the API names (physical disks and image files; anything else is
// shown as an image file).
[[nodiscard]] SourceKind toApi(storage::SourceType type) noexcept;
// The state of a run as a session records it.
[[nodiscard]] RunState runStateOf(const std::optional<session::SessionState>& state, bool interrupted) noexcept;

// ---- Scans ----

// The engine's configuration for `settings` (the image's known unreadable
// regions are added by the caller). InvalidInput for settings out of range.
[[nodiscard]] Result<scan::ScanConfiguration> configurationOf(const ScanSettings& settings);
[[nodiscard]] ScanSettings settingsOf(const scan::ScanConfiguration& configuration, bool playability);
[[nodiscard]] ScanMetrics toApi(const scan::ScanMetrics& metrics);
// The stage's number in a scan of `mode` (1-based), and how many stages it has.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> stageNumber(scan::ScanStage stage,
                                                                  recovery::ScanMode mode) noexcept;
// How much of a scan is done (0 to 1), by the stages' usual shares of its time.
[[nodiscard]] double scanFraction(scan::ScanStage stage, std::uint64_t done, std::uint64_t total,
                                  recovery::ScanMode mode) noexcept;
[[nodiscard]] ScanProgress scanProgressOf(const scan::ScanProgress& progress, recovery::ScanMode mode);
// A scan as its session recorded it (stage and metrics of its last update).
[[nodiscard]] ScanProgress scanProgressOf(const session::ScanStatus& status, recovery::ScanMode mode);

// ---- Recovery ----

[[nodiscard]] RecoveryMetrics toApi(const scan::RecoveryJobMetrics& metrics);
[[nodiscard]] RecoveredFile recoveredFileOf(std::uint32_t job, const scan::RecoveredItem& item);

// ---- Candidates ----

// What a list shows of a candidate; its recovery fields are left as they are
// (the session's recovery index fills them).
[[nodiscard]] CandidateInfo describeCandidate(const evaluation::EvaluatedCandidate& candidate);
[[nodiscard]] MediaDetails toApi(const metadata::MediaMetadata& metadata);
[[nodiscard]] MediaTime toApi(const metadata::MediaDateTime& time);

// ---- Sources and sessions ----

[[nodiscard]] SourceDetails sourceDetailsOf(const storage::SourceInfo& info);
[[nodiscard]] SourceDetails sourceDetailsOf(const session::SessionSource& source);
[[nodiscard]] ImageFileInfo imageFileInfoOf(const imaging::ImageMetadata& metadata,
                                            const std::filesystem::path& metadataFile);
// "1A2B-3C4D" (FAT32, exFAT) or 16 hex digits (NTFS).
[[nodiscard]] std::string serialText(filesystem::FilesystemType type, std::uint64_t serial);
[[nodiscard]] VolumeInfo volumeOf(const session::VolumeSummary& volume);
[[nodiscard]] PartitionInfo partitionOf(const partition::Partition& partition);
[[nodiscard]] std::vector<StateChange> historyOf(const std::vector<session::StateChange>& history);
[[nodiscard]] JobInfo jobOf(const session::RecoveryJobStatus& job);
// Everything but the operation under way (running, paused).
[[nodiscard]] SessionDetails sessionDetailsOf(const session::SessionInfo& info,
                                              const std::vector<session::SessionError>& errors,
                                              const std::vector<storage::BadRegion>& unreadable);
[[nodiscard]] SessionListing listingOf(const session::SessionSummary& summary);

}  // namespace recovery::api::detail
