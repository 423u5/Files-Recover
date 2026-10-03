#pragma once

#include "diagnostics/logger.hpp"
#include "filesystem/exfat/exfat_boot_sector.hpp"
#include "filesystem/exfat/exfat_names.hpp"
#include "filesystem/filesystem.hpp"
#include "recovery/config.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace recovery::filesystem::exfat {

class AllocationBitmap;
class FatTable;
struct ParseOutput;

struct ExFatOptions {
    // Memory for cached FAT pages; bounds memory use on large volumes.
    std::size_t fatCacheBytes = 4 * kMiB;
    // Memory for cached allocation-bitmap pages.
    std::size_t bitmapCacheBytes = 4 * kMiB;
    // Most entries readDirectory() returns for one directory (an exFAT
    // directory may hold up to 256 MiB of entries).
    std::uint64_t maxListingEntries = 1'000'000;
};

// Read-only exFAT implementation of IFilesystem.
//
// Safety properties:
//  * the boot region checksum is verified, with the backup boot region as a
//    fallback;
//  * every cluster number is range-checked before it is used;
//  * chain walks are bounded by the recorded data length and detect loops;
//    contiguous ("NoFatChain") runs are clipped to the cluster heap;
//  * directories are read in bounded chunks, at most 256 MiB each;
//    traversal is bounded in depth and entry count and detects cycles;
//  * FAT and allocation-bitmap pages are cached within fixed memory limits,
//    and pages stored beyond the end of the source are never read;
//  * all reads go through the (bounds-checked) volume source.
//
// Deleted files: exFAT keeps a deleted entry set intact apart from the InUse
// bit of each entry, so the name, size, start cluster and NoFatChain flag
// survive and are verified with the entry-set checksum. A "NoFatChain" file
// is recovered as its exact run; a FAT-chained file uses its old chain when
// the chain survived deletion intact, and a contiguous guess otherwise.
// Either way the allocation bitmap tells whether the clusters were reused.
class ExFatFilesystem final : public IFilesystem {
public:
    ~ExFatFilesystem() override;

    // Opens the exFAT filesystem stored in `volume` (a partition or a whole
    // device). `volume` must stay open and outlive the returned object.
    [[nodiscard]] static Result<std::unique_ptr<ExFatFilesystem>> open(storage::IStorageSource& volume,
                                                                     ExFatOptions options = {},
                                                                     diagnostics::Logger* logger = nullptr);

    [[nodiscard]] const FilesystemInfo& info() const noexcept override { return info_; }
    [[nodiscard]] DirectoryEntry rootDirectory() const override;
    [[nodiscard]] Result<DirectoryListing> readDirectory(const DirectoryEntry& directory) override;
    [[nodiscard]] Result<FileAllocation> resolveAllocation(const DirectoryEntry& entry) override;
    [[nodiscard]] Result<FileScan> scan(const ScanLimits& limits, const CancellationToken& cancel) override;
    [[nodiscard]] Result<ClusterUsage> analyzeClusters(const CancellationToken& cancel) override;
    [[nodiscard]] Result<ClusterState> clusterState(ClusterNumber cluster) override;

    // exFAT-specific details, for diagnostics and tests.
    [[nodiscard]] const BootSector& bootSector() const noexcept { return boot_; }
    [[nodiscard]] bool hasAllocationBitmap() const noexcept { return bitmap_ != nullptr; }
    // False when the volume's up-case table was unusable and the basic
    // built-in table is used for name hashes.
    [[nodiscard]] bool usesVolumeUpcaseTable() const noexcept { return !upcase_.isBasic(); }

private:
    // A run of clusters [first, first + count).
    struct Run {
        std::uint64_t first = 0;
        std::uint64_t count = 0;
    };
    struct ChainWalk {
        std::vector<std::uint32_t> clusters;
        std::vector<AllocationIssue> issues;
    };
    struct Resolved {
        FileAllocation allocation;
        // Clusters owned by an active entry (empty for deleted entries).
        std::vector<Run> ownedRuns;
    };

    ExFatFilesystem(storage::IStorageSource& volume, BootSector boot, ExFatOptions options,
                    diagnostics::Logger* logger);

    void loadSystemStructures();
    [[nodiscard]] std::optional<std::vector<Run>> systemRuns(std::uint32_t first, std::uint64_t bytes,
                                                             std::string_view what);
    [[nodiscard]] std::vector<Extent> extentsOf(const std::vector<Run>& runs, std::uint64_t bytes) const;
    [[nodiscard]] bool isRoot(const DirectoryEntry& entry) const noexcept;
    [[nodiscard]] ChainWalk walkChain(std::uint32_t first, std::uint64_t maxClusters, bool expectExact);
    void checkRun(const Run& run, bool deleted, std::vector<AllocationIssue>& issues);
    [[nodiscard]] Resolved resolve(const DirectoryEntry& entry);
    [[nodiscard]] FileAllocation toAllocation(const std::vector<std::uint32_t>& clusters,
                                              std::optional<std::uint64_t> trimTo) const;
    [[nodiscard]] Result<DirectoryListing> listDirectory(const DirectoryEntry& directory, std::uint64_t budget,
                                                         bool& truncated, ParseOutput* systemEntries);
    [[nodiscard]] std::uint64_t maxDirectoryClusters() const noexcept;
    void log(diagnostics::LogLevel level, std::string_view message,
             std::initializer_list<diagnostics::LogField> fields = {}) const;

    storage::IStorageSource& volume_;
    BootSector boot_;
    ExFatOptions options_;
    FilesystemInfo info_;
    std::unique_ptr<FatTable> fat_;
    std::unique_ptr<AllocationBitmap> bitmap_;  // null when the volume has no usable bitmap
    UpcaseTable upcase_ = UpcaseTable::basic();
    // Clusters of the allocation bitmap and the up-case table.
    std::vector<Run> systemRuns_;
    diagnostics::Logger* logger_;
};

}  // namespace recovery::filesystem::exfat
