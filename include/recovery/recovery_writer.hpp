#pragma once

// Recovery output: writes reconstructed candidates to a destination directory.

#include "diagnostics/logger.hpp"
#include "recovery/candidate_reader.hpp"
#include "recovery/recovery_candidate.hpp"
#include "recovery/result.hpp"
#include "storage/destination_guard.hpp"
#include "storage/storage_source.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>

namespace recovery {

struct RecoveryWriterOptions {
    // Most " (n)" suffixes tried for one name before giving up.
    static constexpr std::uint32_t kMaxNameAttempts = 10'000;

    // Recreate the original directories below the destination; otherwise
    // every file goes directly into it.
    bool preserveDirectories = true;
    ReconstructionOptions reconstruction;
    // Determines which physical disk a destination lives on. Empty selects
    // the platform resolver.
    storage::DiskResolver diskResolver;
    diagnostics::Logger* logger = nullptr;
};

struct RecoveredFile {
    CandidateId candidate{0};
    std::filesystem::path path;
    ReconstructionReport report;
};

// Told the path of a file once it is created (empty), before anything is
// written to it (P16: a session records it, so that a file a crash leaves
// half written is found and removed). An error fails the file, which is
// removed again.
using FileCreatedCallback = std::function<Status(const std::filesystem::path& path)>;

// Writes reconstructed candidates below a destination directory.
//
// Safety properties:
//  * the destination is checked once, and every directory the writer uses
//    again, with checkDestinationSafety: it must not be the source image, and
//    for a physical disk it must not live on that disk;
//  * existing files are never overwritten: a name that is taken gets a
//    " (n)" suffix ("photo.jpg", "photo (1).jpg", ...), and the file is
//    created with DestinationFile::CreateNew, which fails atomically if the
//    name was taken in the meantime;
//  * names from the filesystem go through safeFileName, so they cannot leave
//    the destination or address devices; existing symbolic links and
//    junctions are never followed (a name held by one counts as taken);
//  * paths use the \\?\ prefix, so deep original trees are not limited to
//    MAX_PATH;
//  * a file whose reconstruction or write fails is deleted again, so no
//    half-written file is left behind; the data is flushed before success
//    is reported.
//
// Thread safety: none; one owner at a time. The source must stay open and
// outlive the writer.
class RecoveryWriter {
public:
    // Creates `destination` if needed, then checks that writing below it
    // cannot modify `source`.
    [[nodiscard]] static Result<RecoveryWriter> create(storage::IStorageSource& source,
                                                       std::filesystem::path destination,
                                                       RecoveryWriterOptions options = {});

    // Reconstructs `candidate` into a new file below the destination. A
    // candidate with data but no located byte (nothing but Missing regions)
    // is refused with InvalidInput rather than written as an empty file.
    [[nodiscard]] Result<RecoveredFile> recover(const RecoveryCandidate& candidate);
    // The same, telling `onCreated` (when set) the file's path (as
    // RecoveredFile::path) once it is created, before its data is written.
    [[nodiscard]] Result<RecoveredFile> recover(const RecoveryCandidate& candidate,
                                                const FileCreatedCallback& onCreated);

    [[nodiscard]] const std::filesystem::path& destination() const noexcept { return destination_; }

private:
    RecoveryWriter(storage::IStorageSource& source, std::filesystem::path destination, std::filesystem::path ioRoot,
                   RecoveryWriterOptions options);

    [[nodiscard]] Result<std::filesystem::path> directoryFor(const RecoveryCandidate& candidate);
    [[nodiscard]] Status checkSafety(const std::filesystem::path& path) const;

    storage::IStorageSource* source_;
    storage::SourceInfo sourceInfo_;
    // As given (absolute and normalised), and with the \\?\ prefix for file operations.
    std::filesystem::path destination_;
    std::filesystem::path ioRoot_;
    RecoveryWriterOptions options_;
    // Original directory path -> relative destination directory.
    std::map<std::string, std::filesystem::path> directories_;
};

}  // namespace recovery
