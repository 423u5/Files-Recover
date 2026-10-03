#pragma once

#include "diagnostics/logger.hpp"
#include "filesystem/fat32/fat32_boot_sector.hpp"
#include "filesystem/filesystem.hpp"
#include "recovery/config.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace recovery::filesystem::fat32 {

class FatTable;

struct Fat32Options {
    // Memory for cached allocation-table pages; bounds memory use on large volumes.
    std::size_t fatCacheBytes = 4 * kMiB;
};

// Read-only FAT32 implementation of IFilesystem.
//
// Safety properties:
//  * every cluster number is range-checked before it is used;
//  * chain walks are bounded by the file size (files) or the largest legal
//    directory (directories), and detect loops;
//  * directory traversal is bounded in depth and entry count, and detects
//    directory cycles;
//  * a damaged allocation-table page is re-read from another FAT copy;
//  * all reads go through the (bounds-checked) volume source.
//
// Deleted files: Windows clears the cluster chain when a file is deleted, so
// only the start cluster and size survive. Their allocation is reported as
// ContiguousGuess and flagged when any guessed cluster is in use again.
class Fat32Filesystem final : public IFilesystem {
public:
    ~Fat32Filesystem() override;

    // Opens the FAT32 filesystem stored in `volume` (a partition or a whole
    // device). Falls back to the backup boot sector if the primary is
    // invalid. `volume` must stay open and outlive the returned object.
    [[nodiscard]] static Result<std::unique_ptr<Fat32Filesystem>> open(storage::IStorageSource& volume,
                                                                     Fat32Options options = {},
                                                                     diagnostics::Logger* logger = nullptr);

    [[nodiscard]] const FilesystemInfo& info() const noexcept override { return info_; }
    [[nodiscard]] DirectoryEntry rootDirectory() const override;
    [[nodiscard]] Result<DirectoryListing> readDirectory(const DirectoryEntry& directory) override;
    [[nodiscard]] Result<FileAllocation> resolveAllocation(const DirectoryEntry& entry) override;
    [[nodiscard]] Result<FileScan> scan(const ScanLimits& limits, const CancellationToken& cancel) override;
    [[nodiscard]] Result<ClusterUsage> analyzeClusters(const CancellationToken& cancel) override;
    [[nodiscard]] Result<ClusterState> clusterState(ClusterNumber cluster) override;

    // FAT32-specific details, for diagnostics and tests.
    [[nodiscard]] const BootSector& bootSector() const noexcept { return boot_; }

private:
    struct ChainWalk {
        std::vector<std::uint32_t> clusters;
        std::vector<AllocationIssue> issues;
    };
    struct Resolved {
        FileAllocation allocation;
        // Clusters owned by an active entry (empty for guesses).
        std::vector<std::uint32_t> ownedClusters;
    };

    Fat32Filesystem(storage::IStorageSource& volume, BootSector boot, Fat32Options options,
                    diagnostics::Logger* logger);

    [[nodiscard]] ChainWalk walkChain(std::uint32_t first, std::uint64_t maxClusters, bool isDirectory);
    [[nodiscard]] Resolved resolve(const DirectoryEntry& entry);
    [[nodiscard]] FileAllocation toAllocation(const std::vector<std::uint32_t>& clusters,
                                              std::optional<std::uint64_t> trimTo) const;
    [[nodiscard]] Result<DirectoryListing> listDirectory(const DirectoryEntry& directory,
                                                         std::vector<std::uint32_t>* chainOut);
    [[nodiscard]] std::uint64_t maxDirectoryClusters() const noexcept;
    void log(diagnostics::LogLevel level, std::string_view message,
             std::initializer_list<diagnostics::LogField> fields = {}) const;

    storage::IStorageSource& volume_;
    BootSector boot_;
    FilesystemInfo info_;
    std::optional<std::uint32_t> recordedFree_;
    std::optional<std::string> rootLabel_;
    std::unique_ptr<FatTable> fat_;
    diagnostics::Logger* logger_;
};

}  // namespace recovery::filesystem::fat32
