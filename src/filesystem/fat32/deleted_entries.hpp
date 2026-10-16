#pragma once

// Private to the FAT32 module.
//
// What deletion takes from FAT directory entries besides their chains, found
// again from the evidence the volume still holds.
//
// L7: Windows clears the top 16 bits of a deleted entry's start cluster
// (FAT32). On a volume of more than 65,535 clusters the start recorded is then
// one of several clusters 65,536 apart. inferStarts() places such an entry by
// the strongest evidence there is, and leaves it as recorded when the
// evidence does not single one place out:
//  1. a folder by its first cluster: "." names that cluster and ".." the
//     folder listed;
//  2. a file by its neighbours in the listing (files written one after the
//     other lie one after the other): it starts where the one before it ends,
//     or ends where the one after it starts, when the low 16 bits agree;
//  3. a run of files whose low bits say that they lie one after the other, by
//     what starts at each place (Fat32Options::startsFile): the one place
//     where their starts are free and start files of their names' types.
//
// L8: a deleted folder's chain is gone, so its clusters after the first are
// unknown. nextCluster() finds the next one where writers put it when the
// folder grew: after the data of the last file it lists so far (written
// after the folder's last cluster filled up), or right after its previous
// cluster. A cluster qualifies when it is free, is no folder's first cluster,
// and holds directory entries only.

#include "fat_table.hpp"
#include "filesystem/fat32/fat32_boot_sector.hpp"
#include "filesystem/fat32/fat32_filesystem.hpp"
#include "filesystem/filesystem.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace recovery::filesystem::fat32 {

class DeletedEntries {
public:
    DeletedEntries(storage::IStorageSource& volume, const BootSector& boot, FatTable& fat, StartCheck startsFile);

    // Places the deleted entries of a listing (in on-disk order) whose start
    // cluster lost its top 16 bits: firstCluster is set where the evidence
    // places the data, with EntryIssue::StartClusterInferred when that is not
    // the start recorded. `directory` is the listed directory's first cluster,
    // `isRoot` whether it is the root, and `clusters` all of its clusters.
    void inferStarts(std::vector<DirectoryEntry>& entries, std::uint32_t directory, bool isRoot,
                     const std::vector<std::uint32_t>& clusters);

    // The cluster that continues a deleted directory whose `clusters` so far
    // are full, given the entries they list, or nullopt.
    [[nodiscard]] std::optional<std::uint32_t> nextCluster(const std::vector<DirectoryEntry>& entries,
                                                         const std::vector<std::uint32_t>& clusters);

private:
    // A listed entry that holds data, in listing order.
    struct Item {
        std::size_t entry = 0;
        // Clusters its data takes (a folder: its first one).
        std::uint64_t clusters = 1;
        // The low 16 bits of the start recorded.
        std::uint64_t low = 0;
        bool directory = false;
        // The start lost its top 16 bits.
        bool ambiguous = false;
        // Where it starts: as recorded, or as inferred.
        std::optional<std::uint64_t> start;
    };

    [[nodiscard]] std::vector<Item> itemsOf(const std::vector<DirectoryEntry>& entries) const;
    [[nodiscard]] bool isAmbiguous(const DirectoryEntry& entry) const noexcept;
    // The places 65,536 clusters apart where data of `span` clusters whose
    // start has these low 16 bits fits, lowest first.
    [[nodiscard]] std::vector<std::uint64_t> placesFor(std::uint64_t low, std::uint64_t span) const;
    [[nodiscard]] bool fits(std::uint64_t start, std::uint64_t span) const noexcept;
    [[nodiscard]] bool isFree(std::uint64_t cluster);
    // The cluster after an item's data: its chain's last cluster for an
    // active file, else the end of its run.
    [[nodiscard]] std::uint64_t endOf(const Item& item, const DirectoryEntry& entry);
    // Whether `cluster` is the first cluster of a folder of `parent`.
    [[nodiscard]] bool isFolderAt(std::uint64_t cluster, std::uint32_t parent, bool parentIsRoot);
    // Whether a file named as `entry` starts at `cluster` (nullopt: its name tells nothing).
    [[nodiscard]] std::optional<bool> startsFileAt(const DirectoryEntry& entry, std::uint64_t cluster);
    // Whether `cluster` can continue a deleted directory that has `clusters`.
    [[nodiscard]] bool continuesDirectory(std::uint64_t cluster, const std::vector<std::uint32_t>& clusters);

    void placeFolders(std::vector<Item>& items, std::uint32_t directory, bool isRoot);
    void placeByNeighbours(std::vector<Item>& items, const std::vector<DirectoryEntry>& entries,
                           std::uint32_t directory, const std::vector<std::uint32_t>& clusters);
    void placeRuns(std::vector<Item>& items, const std::vector<DirectoryEntry>& entries);
    void placeRun(std::vector<Item>& items, const std::vector<DirectoryEntry>& entries, std::size_t first,
                  std::size_t end);

    storage::IStorageSource& volume_;
    const BootSector& boot_;
    FatTable& fat_;
    StartCheck startsFile_;
};

}  // namespace recovery::filesystem::fat32
