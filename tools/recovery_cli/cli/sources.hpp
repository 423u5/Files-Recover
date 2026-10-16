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
    // A physical disk (\\.\PhysicalDriveN); a drive ("F:\"), whose disk is
    // read whole (L177); or else an image file.
    std::optional<std::uint32_t> disk;
    std::optional<std::filesystem::path> drive;
    std::filesystem::path image;
};

// --source, by its spelling alone (a usage error otherwise): a physical disk,
// a drive letter ("F:", "F:\"), or a path taken as an image file. Device
// paths other than \\.\PhysicalDriveN (volumes, \\.\E:) are refused: the
// engine reads whole disks and image files.
[[nodiscard]] Result<SourceSpec> parseSource(std::string_view option, std::string_view text);

struct OpenedSource {
    std::unique_ptr<storage::IStorageSource> source;
    storage::SourceInfo info;
    // An image's metadata (<image>.imgmeta), when it has one that reads.
    std::optional<imaging::ImageMetadata> imageMetadata;
};

// Opens `spec` read-only. A drive's disk is the one Windows says it is on,
// which a note names; a drive on several disks is refused. An image's
// logical sector size is `sectorSize`, else its metadata's, else 512. Warns
// about an image whose metadata says it is incomplete or cannot be read. The
// error explains what to do where it can (access denied: administrator
// rights).
[[nodiscard]] Result<OpenedSource> openSource(const SourceSpec& spec, std::optional<std::uint32_t> sectorSize,
                                              Context& context);

// Opens the source a session records: the same image file with the same
// sector size, or the same disk, under the number it had or another one
// (session::findSessionDisk, L147; a note says when it has another). Whether
// it is unchanged is the session's check.
[[nodiscard]] Result<OpenedSource> openRecordedSource(const session::SessionSource& recorded, Context& context);

// What a file the command writes without opening the session's source (a
// report, a log) must not be on: the session's image file, or every disk
// attached that may be the session's disk (session::possibleSessionSources).
[[nodiscard]] std::vector<storage::SourceInfo> protectedSourcesOf(const session::SessionSource& recorded,
                                                                  Context& context);

// The unreadable regions an image's metadata lists (the image holds zeros
// there): a scan and recovery treat them as unreadable.
[[nodiscard]] std::vector<storage::BadRegion> knownBadRegions(const OpenedSource& source);

// "disk image E:\card.img" or "physical disk 2 (Vendor Product)".
using report::describeSource;
[[nodiscard]] std::string describeSource(const storage::SourceInfo& info);

}  // namespace recovery::cli
