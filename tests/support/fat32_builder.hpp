#pragma once

#include "filesystem/filesystem.hpp"
#include "storage/storage_source.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::test {

struct Fat32BuilderOptions {
    std::uint16_t bytesPerSector = 512;
    std::uint8_t sectorsPerCluster = 1;
    std::uint32_t clusterCount = 2048;
    std::uint16_t reservedSectors = 32;
    std::uint8_t fatCount = 2;
    std::string label = "TESTVOL";  // empty: no label entry and "NO NAME" in the BPB
    std::uint32_t volumeId = 0x1234ABCD;
    // FAT16 (after P20) as Windows formats it: 16-bit FAT entries, no FSInfo
    // and no backup boot sector, a root directory of `rootEntries` slots
    // between the FATs and cluster 2. clusterCount must be 4085 to 65524.
    bool fat16 = false;
    // FAT12 (after P20): as FAT16, with 12-bit FAT entries (two in three
    // bytes); clusterCount must be 1 to 4084. A 1.44 MB floppy disk has 1
    // reserved sector, 224 root entries and 2847 clusters of one sector.
    bool fat12 = false;
    std::uint16_t rootEntries = 512;
    // The media descriptor, also kept in FAT entry 0 (floppy disks: 0xF0).
    std::uint8_t media = 0xF8;
    // Write a new entry before its data, as Windows and cameras do: a folder
    // that fills up then grows before the new file's data is allocated, so
    // its next cluster follows the data of the file before (L8). Off: the
    // data is allocated first (the images of the tests before L8).
    bool entriesBeforeData = false;
};

// Builds FAT32 volumes in memory the way Windows lays them out: boot sector
// and backup (sector 6), FSInfo, mirrored FATs, long names with 8.3 aliases
// and checksums, "." and ".." entries, and deletion by marking entries 0xE5
// and freeing the cluster chain.
//
// Every entry is stamped 2024-05-17 13:45:30 (created at .500 s).
class Fat32ImageBuilder {
public:
    struct Entry {
        std::uint32_t directory = 0;  // first cluster of the containing directory
        std::size_t firstSlot = 0;    // first long-name slot, or the 8.3 slot
        std::size_t shortSlot = 0;
        std::vector<std::uint32_t> clusters;
        bool isDirectory = false;
    };

    explicit Fat32ImageBuilder(Fat32BuilderOptions options = {});

    // The root directory's key for addFile and addDirectory (FAT32: its first
    // cluster; FAT12 and FAT16: the fixed region, and cluster 2 is left unused).
    [[nodiscard]] static constexpr std::uint32_t root() noexcept { return 2; }

    Entry addDirectory(std::uint32_t parent, std::string_view name);
    Entry addFile(std::uint32_t parent, std::string_view name, std::span<const std::byte> data);
    // Stores the data in exactly these clusters, in this order (fragmentation).
    Entry addFileInClusters(std::uint32_t parent, std::string_view name, std::span<const std::byte> data,
                            const std::vector<std::uint32_t>& clusters);
    // Deletes like Windows: 0xE5 on every slot of the entry set, chain freed
    // (a folder's whole chain). `clearHighCluster` also clears the top 16
    // bits of the start cluster, as Windows does on FAT32 (L7).
    void deleteEntry(const Entry& entry, bool freeClusters = true, bool clearHighCluster = false);
    // The clusters of the directory whose first cluster is `directory`.
    [[nodiscard]] const std::vector<std::uint32_t>& directoryClusters(std::uint32_t directory) const;

    void setFat(std::uint32_t cluster, std::uint32_t value);  // all copies
    void setFatInCopy(std::uint32_t copy, std::uint32_t cluster, std::uint32_t value);
    [[nodiscard]] std::uint32_t fat(std::uint32_t cluster, std::uint32_t copy = 0) const;
    // Makes later allocations start `count` clusters further on.
    void skipClusters(std::uint32_t count);
    // Where the next allocation starts looking for a free cluster.
    [[nodiscard]] std::uint32_t cursor() const noexcept { return cursor_; }

    [[nodiscard]] std::span<std::byte> shortEntry(const Entry& entry);
    [[nodiscard]] std::uint64_t shortEntryOffset(const Entry& entry) const;
    [[nodiscard]] std::uint64_t clusterOffset(std::uint32_t cluster) const;
    [[nodiscard]] std::uint64_t fatOffset(std::uint32_t copy) const;
    [[nodiscard]] std::uint32_t clusterSize() const noexcept { return clusterSize_; }
    [[nodiscard]] std::uint32_t lastCluster() const noexcept { return options_.clusterCount + 1; }
    [[nodiscard]] std::uint32_t usedClusters() const;

    // Writes FSInfo and returns a copy of the image.
    [[nodiscard]] std::vector<std::byte> build();
    [[nodiscard]] std::vector<std::byte>& raw() noexcept { return image_; }

private:
    struct Directory {
        std::vector<std::uint32_t> clusters;
        std::size_t nextSlot = 0;
        std::set<std::string> shortNames;
        bool fixed = false;  // the root of FAT12 and FAT16: the region before cluster 2
    };

    std::uint32_t allocate();
    std::span<std::byte> slot(Directory& directory, std::size_t index);
    // With entriesBeforeData: grows the parent for the entry set of `name` now.
    void reserveEntrySet(std::uint32_t parent, std::string_view name);
    std::array<std::byte, 11> shortNameFor(Directory& directory, std::string_view name, bool& needsLongName);
    Entry writeEntrySet(std::uint32_t parent, std::string_view name, std::uint8_t attributes, std::uint32_t cluster,
                        std::uint32_t size);
    void writeData(const std::vector<std::uint32_t>& clusters, std::span<const std::byte> data);
    void writeBootSector(std::uint32_t sector);
    void writeFsInfo(std::uint32_t sector);

    void writeSmallFatBootSector();
    [[nodiscard]] bool smallFat() const noexcept { return options_.fat16 || options_.fat12; }
    [[nodiscard]] std::uint32_t rootDirSectors() const noexcept;

    Fat32BuilderOptions options_;
    std::uint32_t clusterSize_;
    std::uint32_t fatSectors_;
    std::vector<std::byte> image_;
    std::map<std::uint32_t, Directory> directories_;
    std::uint32_t cursor_ = 3;
};

// UTF-8 -> UTF-16 (test inputs are valid UTF-8).
[[nodiscard]] std::u16string utf8ToUtf16(std::string_view text);

// Concatenates the bytes of `extents`, as recovery would.
[[nodiscard]] std::vector<std::byte> readExtents(storage::IStorageSource& volume,
                                                 const std::vector<filesystem::Extent>& extents);

}  // namespace recovery::test
