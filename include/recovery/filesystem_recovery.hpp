#pragma once

// Filesystem-based recovery: recovery candidates built from filesystem
// metadata, attempted before raw carving because metadata gives names,
// sizes and exact data locations.

#include "diagnostics/logger.hpp"
#include "filesystem/exfat/exfat_filesystem.hpp"
#include "filesystem/fat32/fat32_filesystem.hpp"
#include "filesystem/filesystem.hpp"
#include "filesystem/ntfs/ntfs_filesystem.hpp"
#include "recovery/cancellation.hpp"
#include "recovery/recovery_candidate.hpp"
#include "recovery/result.hpp"
#include "storage/storage_source.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace recovery {

struct CandidateOptions {
    bool includeActive = true;
    // Deleted entries, and entries inside deleted directories.
    bool includeDeleted = true;
    // Traversal limits for the filesystem scan. Its includeDeleted and
    // recurseIntoDeletedDirectories are turned off when includeDeleted is false.
    filesystem::ScanLimits limits;
    // Identifier of the first candidate; the others follow in scan order.
    std::uint64_t firstId = 1;
};

struct CandidateScan {
    filesystem::FilesystemInfo filesystemInfo;
    std::uint64_t volumeOffset = 0;
    // Files, in scan order (directories are not candidates).
    std::vector<RecoveryCandidate> candidates;
    // Problems met while traversing the filesystem.
    std::vector<filesystem::ScanIssue> issues;
    std::uint64_t directories = 0;
    // Entries that belong to the filesystem itself (NTFS metadata files).
    std::uint64_t systemFiles = 0;
    // False when a traversal limit stopped the scan early.
    bool complete = true;
};

// Turns the files a filesystem's metadata knows about, active and deleted,
// into recovery candidates.
//
// Each candidate maps every byte of the file to a source region: stored on
// the source, embedded in the metadata, known zeros, or missing. Nothing is
// assumed contiguous unless the metadata records it; a contiguous guess is
// labelled as such (LayoutEvidence::Guessed). For deleted files, the
// clusters that have been allocated again since the deletion are marked
// (SourceRegion::reallocated), so a partially overwritten file shows which
// of its parts may be damaged.
//
// Candidates are metadata evidence only. Their data is not validated here.
//
// Thread safety: not thread-safe (the filesystem it uses is not).
class FilesystemRecovery {
public:
    virtual ~FilesystemRecovery() = default;
    FilesystemRecovery(const FilesystemRecovery&) = delete;
    FilesystemRecovery& operator=(const FilesystemRecovery&) = delete;
    FilesystemRecovery(FilesystemRecovery&&) = delete;
    FilesystemRecovery& operator=(FilesystemRecovery&&) = delete;

    [[nodiscard]] virtual filesystem::IFilesystem& filesystem() noexcept = 0;
    // Byte offset of the volume on the source, added to every offset in the candidates.
    [[nodiscard]] std::uint64_t volumeOffset() const noexcept { return volumeOffset_; }

    [[nodiscard]] Result<CandidateScan> findCandidates(const CandidateOptions& options,
                                                       const CancellationToken& cancel);

protected:
    // A piece of a file's data, in volume coordinates. Pieces follow each
    // other in file order.
    struct LayoutPiece {
        std::uint64_t length = 0;
        RegionKind kind = RegionKind::Missing;
        // Stored: volume offset. Embedded: offset within the embedded data.
        std::uint64_t volumeOffset = 0;
    };

    FilesystemRecovery(std::uint64_t volumeOffset, diagnostics::Logger* logger) noexcept;

    // False for entries that belong to the filesystem itself rather than to a user.
    [[nodiscard]] virtual bool isUserFile(const filesystem::FileRecord& record);
    // The file's layout when the filesystem-independent allocation cannot
    // describe it (NTFS sparse files: its extents omit the holes). nullopt
    // uses record.allocation.
    [[nodiscard]] virtual std::optional<std::vector<LayoutPiece>> customLayout(const filesystem::FileRecord& record);

private:
    [[nodiscard]] RecoveryCandidate buildCandidate(const filesystem::FileRecord& record, CandidateId id);
    [[nodiscard]] std::vector<SourceRegion> splitReallocated(const std::vector<SourceRegion>& regions);

    std::uint64_t volumeOffset_;
    diagnostics::Logger* logger_;
};

// FAT32: deleted entries keep only their first cluster and size (Windows
// frees the chain), so a deleted file of more than one cluster has a Guessed
// layout.
class Fat32Recovery final : public FilesystemRecovery {
public:
    // Opens the FAT32 volume in `volume`, which starts `volumeOffset` bytes into the source.
    [[nodiscard]] static Result<std::unique_ptr<Fat32Recovery>> open(storage::IStorageSource& volume,
                                                                   std::uint64_t volumeOffset,
                                                                   filesystem::fat32::Fat32Options options = {},
                                                                   diagnostics::Logger* logger = nullptr);
    Fat32Recovery(std::unique_ptr<filesystem::fat32::Fat32Filesystem> fs, std::uint64_t volumeOffset,
                  diagnostics::Logger* logger = nullptr);

    [[nodiscard]] filesystem::IFilesystem& filesystem() noexcept override { return *filesystem_; }

private:
    std::unique_ptr<filesystem::fat32::Fat32Filesystem> filesystem_;
};

// exFAT: deleted entry sets keep their size, valid data length, first
// cluster and contiguous-data flag; a FAT-chained file keeps its old chain
// when it survived the deletion. Bytes beyond the valid data length are zeros.
class ExFatRecovery final : public FilesystemRecovery {
public:
    [[nodiscard]] static Result<std::unique_ptr<ExFatRecovery>> open(storage::IStorageSource& volume,
                                                                   std::uint64_t volumeOffset,
                                                                   filesystem::exfat::ExFatOptions options = {},
                                                                   diagnostics::Logger* logger = nullptr);
    ExFatRecovery(std::unique_ptr<filesystem::exfat::ExFatFilesystem> fs, std::uint64_t volumeOffset,
                  diagnostics::Logger* logger = nullptr);

    [[nodiscard]] filesystem::IFilesystem& filesystem() noexcept override { return *filesystem_; }

private:
    std::unique_ptr<filesystem::exfat::ExFatFilesystem> filesystem_;
};

// NTFS: deleted records keep their data runs, so deleted files have an exact
// layout. Sparse files are laid out from their run list, holes included.
// The metadata files (records 0-15 and everything under $Extend) are not
// candidates. Compressed and encrypted data is Missing (DataNotDecoded).
class NtfsRecovery final : public FilesystemRecovery {
public:
    [[nodiscard]] static Result<std::unique_ptr<NtfsRecovery>> open(storage::IStorageSource& volume,
                                                                  std::uint64_t volumeOffset,
                                                                  filesystem::ntfs::NtfsOptions options = {},
                                                                  diagnostics::Logger* logger = nullptr);
    NtfsRecovery(std::unique_ptr<filesystem::ntfs::NtfsFilesystem> fs, std::uint64_t volumeOffset,
                 diagnostics::Logger* logger = nullptr);

    [[nodiscard]] filesystem::IFilesystem& filesystem() noexcept override { return *filesystem_; }

private:
    [[nodiscard]] bool isUserFile(const filesystem::FileRecord& record) override;
    [[nodiscard]] std::optional<std::vector<LayoutPiece>> customLayout(const filesystem::FileRecord& record) override;

    std::unique_ptr<filesystem::ntfs::NtfsFilesystem> filesystem_;
    // Volume offsets of records 0-15, computed on first use.
    std::optional<std::set<std::uint64_t>> systemRecordOffsets_;
};

struct FilesystemRecoveryOptions {
    filesystem::fat32::Fat32Options fat32;
    filesystem::exfat::ExFatOptions exfat;
    filesystem::ntfs::NtfsOptions ntfs;
};

// Opens the FAT32, exFAT or NTFS filesystem in `volume` (a partition, or the
// whole device when it is unpartitioned) for recovery. The boot record's
// signature decides which module is tried first; if that fails, or the
// signature is unrecognisable (a damaged primary boot sector), the others
// are tried, since each can fall back to its backup boot record.
// `volume` must stay open and outlive the returned object.
//
// Fails with UnsupportedFilesystem when no module recognises the volume, and
// with CorruptedFilesystem when one recognises it but cannot use it.
[[nodiscard]] Result<std::unique_ptr<FilesystemRecovery>> openFilesystemRecovery(
    storage::IStorageSource& volume, std::uint64_t volumeOffset, const FilesystemRecoveryOptions& options = {},
    diagnostics::Logger* logger = nullptr);

}  // namespace recovery
