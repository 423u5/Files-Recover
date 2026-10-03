#pragma once

#include "diagnostics/logger.hpp"
#include "filesystem/filesystem.hpp"
#include "filesystem/ntfs/ntfs_boot_sector.hpp"
#include "filesystem/ntfs/ntfs_record.hpp"
#include "recovery/config.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::filesystem::exfat {
class PagedRegion;
}  // namespace recovery::filesystem::exfat

namespace recovery::filesystem::ntfs {

class ClusterBitmap;

// Virtual directory under the root that holds entries whose parent directory
// is gone ("/$OrphanFiles/<name>"). It is not a directory of the volume.
inline constexpr std::string_view kOrphanDirectory = "$OrphanFiles";

struct NtfsOptions {
    // Memory for cached MFT pages; bounds memory use on large volumes.
    std::size_t mftCacheBytes = 4 * kMiB;
    // Memory for cached $Bitmap pages.
    std::size_t bitmapCacheBytes = 4 * kMiB;
    // Most MFT records examined (at most 2^32 - 1). Listing a directory or
    // scanning reads every record once, since deleted files can only be
    // found through their records.
    std::uint64_t maxRecords = 16'777'216;
    // Most entries readDirectory() returns for one directory.
    std::uint64_t maxListingEntries = 1'000'000;
};

// Read-only NTFS implementation of IFilesystem: the NTFS foundation (boot
// sector, MFT discovery, FILE records, $FILE_NAME and $DATA attributes,
// resident and non-resident data, data runs, deleted records).
//
// Safety properties:
//  * the boot sector is validated, with the backup at the end of the volume
//    as a fallback; the MFT is located through its own record 0, with the
//    copy in $MFTMirr as a fallback;
//  * every record's update sequence is verified before it is parsed, and
//    every attribute, name and run list is bounds-checked;
//  * every run is range-checked against the volume before it is reported
//    or read;
//  * the MFT is read sequentially through a bounded page cache, at most
//    maxRecords records are examined, and records stored beyond the end of
//    the source are never read;
//  * traversal is bounded in depth and entry count and detects cycles.
//
// Directories are rebuilt from the parent references in every record's
// $FILE_NAME attributes, not from the directory indexes: deleting a file
// removes it from its directory's index but leaves its record, so this finds
// deleted files too. A deleted record keeps its attributes, so its names,
// size, times and data runs survive; $Bitmap tells whether its clusters were
// reused. Records whose parent directory is gone are listed under
// "/$OrphanFiles".
//
// Not supported yet (reported, never silently wrong): attribute lists
// (AttributeListNotFollowed), and compressed, encrypted or sparse data
// (CompressedData, EncryptedData, SparseRuns). Named data streams, directory
// indexes and $LogFile are not read.
class NtfsFilesystem final : public IFilesystem {
public:
    ~NtfsFilesystem() override;

    // Opens the NTFS filesystem stored in `volume` (a partition or a whole
    // device). `volume` must stay open and outlive the returned object.
    [[nodiscard]] static Result<std::unique_ptr<NtfsFilesystem>> open(storage::IStorageSource& volume,
                                                                    NtfsOptions options = {},
                                                                    diagnostics::Logger* logger = nullptr);

    [[nodiscard]] const FilesystemInfo& info() const noexcept override { return info_; }
    [[nodiscard]] DirectoryEntry rootDirectory() const override;
    // Lists the entries whose records name `directory` as their parent,
    // deleted ones included. The first call reads the whole MFT.
    [[nodiscard]] Result<DirectoryListing> readDirectory(const DirectoryEntry& directory) override;
    // Re-reads the entry's record (found through metadataOffset). Cross-links
    // are only detected by scan().
    [[nodiscard]] Result<FileAllocation> resolveAllocation(const DirectoryEntry& entry) override;
    [[nodiscard]] Result<FileScan> scan(const ScanLimits& limits, const CancellationToken& cancel) override;
    [[nodiscard]] Result<ClusterUsage> analyzeClusters(const CancellationToken& cancel) override;
    [[nodiscard]] Result<ClusterState> clusterState(ClusterNumber cluster) override;

    // NTFS-specific details, for diagnostics and tests.
    [[nodiscard]] const BootSector& bootSector() const noexcept { return boot_; }
    // Records in the MFT (at most NtfsOptions::maxRecords).
    [[nodiscard]] std::uint64_t recordCount() const noexcept { return recordCount_; }
    [[nodiscard]] bool hasClusterBitmap() const noexcept { return bitmap_ != nullptr; }
    // True when $MFT's own record 0 was unusable and its copy in $MFTMirr was used.
    [[nodiscard]] bool usedMftMirror() const noexcept { return usedMirror_; }
    // NTFS version from $VOLUME_INFORMATION (0.0 when unknown).
    [[nodiscard]] std::uint8_t majorVersion() const noexcept { return majorVersion_; }
    [[nodiscard]] std::uint8_t minorVersion() const noexcept { return minorVersion_; }
    // Reads and parses MFT record `number` (< recordCount()).
    [[nodiscard]] Result<MftRecord> readRecord(std::uint64_t number);
    // Volume byte offset of MFT record `number` (< recordCount()).
    [[nodiscard]] std::uint64_t recordOffset(std::uint64_t number) const noexcept;
    // The number of the MFT record that starts at `volumeOffset` (an entry's
    // metadataOffset), if one does.
    [[nodiscard]] std::optional<std::uint64_t> recordNumberAt(std::uint64_t volumeOffset) const noexcept {
        return recordAt(volumeOffset);
    }

private:
    struct Catalog;
    // A run of clusters [first, first + count).
    struct Run {
        std::uint64_t first = 0;
        std::uint64_t count = 0;
    };
    // An entry's metadata apart from its names, and its allocation.
    struct Described {
        DirectoryEntry entry;
        FileAllocation allocation;
    };

    NtfsFilesystem(storage::IStorageSource& volume, BootSector boot, NtfsOptions options, diagnostics::Logger* logger);

    [[nodiscard]] Status loadMft();
    void loadSystemFiles();
    [[nodiscard]] Status readRecordBytes(std::uint64_t number, std::span<std::byte> out);
    [[nodiscard]] std::optional<std::uint64_t> recordAt(std::uint64_t volumeOffset) const noexcept;
    // Volume extents of a run list's first `bytes` bytes, stopping at the
    // first hole or invalid run. Used for the MFT and $Bitmap.
    [[nodiscard]] std::vector<Extent> contiguousExtents(const RunList& runs, std::uint64_t bytes, std::string_view what);
    [[nodiscard]] Described describe(const MftRecord& record);
    [[nodiscard]] FileAllocation runAllocation(const Attribute& attribute, std::optional<std::uint64_t> size,
                                               bool deleted);
    void checkRun(std::uint64_t first, std::uint64_t count, bool deleted, std::vector<AllocationIssue>& issues);
    void claimRuns(const MftRecord& record, std::vector<Run>& claims) const;
    [[nodiscard]] Result<const Catalog*> catalog(const CancellationToken& cancel);
    [[nodiscard]] Result<std::unique_ptr<Catalog>> buildCatalog(const CancellationToken& cancel);
    void log(diagnostics::LogLevel level, std::string_view message,
             std::initializer_list<diagnostics::LogField> fields = {}) const;

    storage::IStorageSource& volume_;
    BootSector boot_;
    NtfsOptions options_;
    FilesystemInfo info_;
    // The MFT: its extents (region order), where each starts in the region,
    // and a page cache over them.
    std::vector<Extent> mftExtents_;
    std::vector<std::uint64_t> mftStarts_;
    std::unique_ptr<exfat::PagedRegion> mft_;
    std::uint64_t recordCount_ = 0;
    // Records stored entirely before the end of the source.
    std::uint64_t reachableRecords_ = 0;
    std::unique_ptr<ClusterBitmap> bitmap_;  // null when $Bitmap is unusable
    std::vector<Run> badRuns_;               // $BadClus:$Bad, sorted and merged
    bool usedMirror_ = false;
    std::uint8_t majorVersion_ = 0;
    std::uint8_t minorVersion_ = 0;
    std::unique_ptr<Catalog> catalog_;  // built on first use
    diagnostics::Logger* logger_;
};

}  // namespace recovery::filesystem::ntfs
