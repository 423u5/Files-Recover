#pragma once

// The sources the CLI reads: disk image files and physical disks, named on
// the command line (--source) or by a session, opened read-only; and what an
// image's metadata file (written by `recovery image`) says about the image.

#include "cli/context.hpp"
#include "imaging/image_metadata.hpp"
#include "recovery/result.hpp"
#include "report/text_format.hpp"
#include "session/session_types.hpp"
#include "storage/bad_region.hpp"
#include "storage/storage_source.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::cli {

// What --source names.
struct SourceSpec {
    // A physical disk (\\.\PhysicalDriveN), or else an image file.
    std::optional<std::uint32_t> disk;
    std::filesystem::path image;
};

// --source, by its spelling alone (a usage error otherwise): a physical disk,
// or a path taken as an image file. Device paths other than
// \\.\PhysicalDriveN (volumes, \\.\E:) are refused: the engine reads whole
// disks and image files.
[[nodiscard]] Result<SourceSpec> parseSource(std::string_view option, std::string_view text);

struct OpenedSource {
    std::unique_ptr<storage::IStorageSource> source;
    storage::SourceInfo info;
    // An image's metadata (<image>.imgmeta), when it has one that reads.
    std::optional<imaging::ImageMetadata> imageMetadata;
};

// Opens `spec` read-only. An image's logical sector size is `sectorSize`, else
// its metadata's, else 512. Warns about an image whose metadata says it is
// incomplete or cannot be read. The error explains what to do where it can
// (a drive letter: the disk it is on; access denied: administrator rights).
[[nodiscard]] Result<OpenedSource> openSource(const SourceSpec& spec, std::optional<std::uint32_t> sectorSize,
                                              Context& context);

// Opens the source a session records: the same disk, or the same image file
// with the same sector size. Whether it is unchanged is the session's check.
[[nodiscard]] Result<OpenedSource> openRecordedSource(const session::SessionSource& recorded, Context& context);

// What a destination check needs of a session's source.
[[nodiscard]] storage::SourceInfo sourceInfoOf(const session::SessionSource& recorded);
// The same from a source's type and path alone (a session's summary): a
// physical disk's number is read from its path.
[[nodiscard]] storage::SourceInfo sourceInfoOf(storage::SourceType type, std::string_view path);

// The unreadable regions an image's metadata lists (the image holds zeros
// there): a scan and recovery treat them as unreadable.
[[nodiscard]] std::vector<storage::BadRegion> knownBadRegions(const OpenedSource& source);

// "disk image E:\card.img" or "physical disk 2 (Vendor Product)".
using report::describeSource;
[[nodiscard]] std::string describeSource(const storage::SourceInfo& info);

}  // namespace recovery::cli
