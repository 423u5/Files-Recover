#pragma once

#include "diagnostics/logger.hpp"
#include "filesystem/fat32/fat32_boot_sector.hpp"
#include "filesystem/filesystem.hpp"
#include "recovery/config.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace recovery::filesystem::fat32 {

class FatTable;
class DeletedEntries;
class DirectoryParser;

// Whether `head` (the first bytes of where a file may start: kStartCheckBytes,
// fewer at the end of the volume) starts a file named `name`: true, false, or
// nullopt when the name tells nothing (no known type for it). Must be safe to
// call from the thread that scans the filesystem.
using StartCheck = std::function<std::optional<bool>(std::string_view name, std::span<const std::byte> head)>;
inline constexpr std::size_t kStartCheckBytes = 4096;
// Whether the file named `name` of `size` bytes that would start at byte
// `offset` of the volume ends there, by the structure of its type: false when
// it ends elsewhere or that cannot be told. Must be safe to call from the
// thread that scans the filesystem, and read the volume the filesystem reads.
using EndCheck = std::function<bool(std::string_view name, std::uint64_t offset, std::uint64_t size)>;

struct Fat32Options {
    // Memory for cached allocation-table pages; bounds memory use on large volumes.
    std::size_t fatCacheBytes = 4 * kMiB;
    // What starts a file of a given name (the scan's formats). Lets the start
    // cluster of a deleted file whose top 16 bits were cleared be found when
    // the entries around it cannot tell (L7). Empty: only they, and the state
    // and content of the clusters at each place (L242), are used.
    StartCheck startsFile;
    // Where a file of a given name ends (the scan's formats): of several
    // places that each start a file of its type, the one where that file
    // ends at its size (L242). Empty: not used.
    EndCheck endsFile;
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
// Windows also clears the top 16 bits of the start cluster (FAT32): on a
// volume of more than 65,535 clusters the start is then inferred from the
// folder's other entries, from what starts there, and for a folder from its
// "." and ".." entries (EntryIssue::StartClusterInferred, L7). A deleted
// folder's chain is gone too: its clusters after the first are found where
// writers put them, after the folder's last file or after its previous
// cluster, if they hold folder entries (L8). A scan looks for the clusters
// that search cannot find among the free clusters right after the data of
// any file and after any folder's clusters: those that hold folder entries
// no listing reached are listed in the deleted folder that leads to them, or
// under "/$OrphanFiles" when none does alone (L243).
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
    // A directory's entries as read, and what a scan needs to know besides.
    struct Listed {
        DirectoryListing listing;
        // The clusters read: an active directory's chain; the first cluster of
        // a deleted one, and those found to continue it (L8).
        std::vector<std::uint32_t> clusters;
        // A deleted directory whose last cluster read is full and whose next
        // cluster was not found: it goes on somewhere (L243).
        bool endedFull = false;
    };
    // What a scan saw of the volume's clusters, and the deleted folders that
    // go on somewhere (L243). Defined with scan().
    struct Seen;
    struct LostPart;

    Fat32Filesystem(storage::IStorageSource& volume, BootSector boot, Fat32Options options,
                    diagnostics::Logger* logger);

    [[nodiscard]] ChainWalk walkChain(std::uint32_t first, std::uint64_t maxClusters, bool isDirectory);
    [[nodiscard]] Resolved resolve(const DirectoryEntry& entry);
    [[nodiscard]] FileAllocation toAllocation(const std::vector<std::uint32_t>& clusters,
                                              std::optional<std::uint64_t> trimTo) const;
    // `inferDeleted`: place the deleted entries whose start lost its top 16
    // bits (off for reading the root's label only).
    [[nodiscard]] Result<Listed> listDirectory(const DirectoryEntry& directory, bool inferDeleted);
    // Reads the slots of `listed.clusters`, or of the fixed root region, into
    // `listed.listing`; a deleted directory goes on in the clusters found to
    // continue it (L8), none of `taken`. `dotFirst`: the first cluster of a
    // deleted directory must start with its "." entry.
    [[nodiscard]] Status readSlots(Listed& listed, DirectoryParser& parser, bool fixedRoot, bool deleted,
                                   bool dotFirst, const std::unordered_set<std::uint64_t>* taken);
    // A part of a deleted folder found apart from it (L243), from its cluster
    // `head` on, continued where it grew into none of `taken`; its entries
    // are placed with no parent known.
    [[nodiscard]] Listed listPart(std::uint32_t head, const std::unordered_set<std::uint64_t>& taken);
    // The parts of deleted folders no listing reached, each with the folder
    // that leads to it if one does alone (L243).
    [[nodiscard]] Result<std::vector<LostPart>> findLostParts(const Seen& seen, const CancellationToken& cancel);
    [[nodiscard]] std::uint64_t maxDirectoryClusters() const noexcept;
    void log(diagnostics::LogLevel level, std::string_view message,
             std::initializer_list<diagnostics::LogField> fields = {}) const;

    storage::IStorageSource& volume_;
    BootSector boot_;
    FilesystemInfo info_;
    std::optional<std::uint32_t> recordedFree_;
    std::optional<std::string> rootLabel_;
    std::unique_ptr<FatTable> fat_;
    std::unique_ptr<DeletedEntries> deleted_;
    diagnostics::Logger* logger_;
};

}  // namespace recovery::filesystem::fat32
