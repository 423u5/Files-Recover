#pragma once

// The sources the API reads (P19): disk image files and physical disks,
// opened read-only from what the user interface names or from what a session
// records; what a source holds (inspection); and the disks attached.

#include "api/api_platform.hpp"
#include "api/api_types.hpp"
#include "diagnostics/logger.hpp"
#include "imaging/image_metadata.hpp"
#include "recovery/result.hpp"
#include "session/session_types.hpp"
#include "storage/bad_region.hpp"
#include "storage/storage_source.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace recovery::api::detail {

// The platform's parts, the hooks' where given.
struct Platform {
    std::shared_ptr<storage::IDeviceOpener> diskOpener;
    storage::DiskResolver diskResolver;
    storage::DiskLister diskLister;
    std::filesystem::path systemFolder;
    std::function<std::unique_ptr<validation::IPlayabilityChecker>()> playability;
    std::function<void(std::string_view session, const Progress& progress)> onOperationProgress;
    std::uint64_t maxQueuedFiles = 0;
};

// The platform's parts where `hooks` leaves them empty.
[[nodiscard]] Platform platformOf(PlatformHooks hooks);

struct OpenedSource {
    std::unique_ptr<storage::IStorageSource> source;
    storage::SourceInfo info;
    // An image's metadata (<image>.imgmeta), when it has one that reads.
    std::optional<imaging::ImageMetadata> imageMetadata;
    std::filesystem::path metadataFile;
    // Why an image's metadata could not be read (its unreadable regions are
    // then not known: they read as zeros).
    std::optional<Error> metadataError;
};

// Opens `source` read-only. An image's sector size is source.sectorSize,
// else its metadata's, else 512. The errors say what to do where they can
// (administrator rights, a drive letter given for a disk).
[[nodiscard]] Result<OpenedSource> openSource(const SourceRef& source, const Platform& platform);
// Opens the source a session records (the same disk, or the same image file
// with the same sector size). Whether it is unchanged is checked apart.
[[nodiscard]] Result<OpenedSource> openRecordedSource(const session::SessionSource& recorded,
                                                      const Platform& platform);
// What a destination check needs of a session's source.
[[nodiscard]] storage::SourceInfo sourceInfoOf(const session::SessionSource& recorded);

// The unreadable regions an image's metadata lists.
[[nodiscard]] std::vector<storage::BadRegion> knownBadRegions(const OpenedSource& source);

// The source, its partitions and its volumes' filesystems.
[[nodiscard]] Result<SourceInspection> inspect(OpenedSource& source, diagnostics::Logger* logger);

// The disks attached, with the drive letters on them and whether they hold
// Windows or the sessions folder.
[[nodiscard]] Result<std::vector<DiskInfo>> listDisks(const Platform& platform,
                                                      const std::filesystem::path& sessionsRoot);

// An environment variable that names a folder (%LOCALAPPDATA%), read from
// the C runtime's copy of the process environment; none when it is not set.
[[nodiscard]] std::optional<std::filesystem::path> environmentFolder(const wchar_t* name);

// A path's nearest part that exists (itself, or a parent), for telling the
// disk of a folder that is not created yet; empty when none does.
[[nodiscard]] std::filesystem::path existingPart(const std::filesystem::path& path);

}  // namespace recovery::api::detail
