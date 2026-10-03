#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::test {

struct ExFatBuilderOptions {
    std::uint8_t bytesPerSectorShift = 9;     // 512-byte sectors
    std::uint8_t sectorsPerClusterShift = 0;  // one sector per cluster
    std::uint32_t clusterCount = 4096;
    std::uint32_t fatOffsetSectors = 32;
    std::uint8_t fatCount = 1;
    std::string label = "TESTVOL";  // UTF-8, at most 11 UTF-16 units; empty: no label
    std::uint32_t volumeSerial = 0x1234ABCD;
    bool volumeGuidEntry = true;  // benign primary entry in the root directory
    // Deletion frees clusters in the bitmap only; set to also clear the FAT
    // chain of deleted FAT-chained files (implementations differ).
    bool clearFatOnDelete = false;
};

// Builds exFAT volumes in memory the way Windows lays them out: main and
// backup boot regions with checksums; one FAT; the allocation bitmap, a
// compressed up-case table and the root directory in the first clusters,
// each with a FAT chain; File / Stream Extension / File Name entry sets with
// set checksums and name hashes; files written in one piece stored as
// "NoFatChain" runs; fragmented files and grown directories linked through
// the FAT; and deletion by clearing the InUse bit of every entry of the set
// and the file's bits in the allocation bitmap.
//
// Checksums and name hashes are computed by the builder itself, not with
// engine code. Its up-case table maps ASCII, Latin-1, Greek and Cyrillic.
//
// Every entry is stamped 2024-05-17 13:45:30 local time at UTC+02:00
// (created at .500 s).
class ExFatImageBuilder {
public:
    struct Entry {
        std::uint32_t directory = 0;  // first cluster of the containing directory
        std::size_t firstSlot = 0;    // the File entry
        std::size_t slotCount = 0;    // File entry and its secondaries
        std::vector<std::uint32_t> clusters;
        bool isDirectory = false;
        bool contiguous = false;  // NoFatChain
    };

    explicit ExFatImageBuilder(ExFatBuilderOptions options = {});

    [[nodiscard]] std::uint32_t root() const noexcept { return rootCluster_; }

    // A one-cluster directory. It grows by a cluster when full, switching to
    // a FAT chain when the next cluster is not free (as Windows does).
    Entry addDirectory(std::uint32_t parent, std::string_view name);
    // Contiguous data stored as a NoFatChain run (the FAT is left untouched).
    Entry addFile(std::uint32_t parent, std::string_view name, std::span<const std::byte> data,
                  std::size_t vendorExtensions = 0);
    // Stores the data in exactly these clusters, in this order, linked by a FAT chain.
    Entry addFileInClusters(std::uint32_t parent, std::string_view name, std::span<const std::byte> data,
                            const std::vector<std::uint32_t>& clusters);
    // Deletes like Windows: clears the InUse bit of every entry in the set
    // and frees the clusters in the allocation bitmap.
    void deleteEntry(const Entry& entry, bool freeClusters = true);

    // Entry `index` of an entry set (0 = File, 1 = Stream Extension, 2.. = names).
    [[nodiscard]] std::span<std::byte> slot(const Entry& entry, std::size_t index);
    [[nodiscard]] std::uint64_t slotOffset(const Entry& entry, std::size_t index) const;
    // Raw entry `index` of a directory, grown as needed.
    [[nodiscard]] std::span<std::byte> directorySlot(std::uint32_t directory, std::size_t index);
    [[nodiscard]] std::size_t nextFreeSlot(std::uint32_t directory) const;
    // Recomputes the set checksum after a test edited the set.
    void rechecksum(const Entry& entry);

    void setFat(std::uint32_t cluster, std::uint32_t value);
    [[nodiscard]] std::uint32_t fat(std::uint32_t cluster) const;
    void setAllocated(std::uint32_t cluster, bool allocated);
    [[nodiscard]] bool isAllocated(std::uint32_t cluster) const;
    [[nodiscard]] std::uint32_t allocatedClusters() const;
    // Makes later allocations start `count` clusters further on.
    void skipClusters(std::uint32_t count);

    [[nodiscard]] std::uint64_t clusterOffset(std::uint32_t cluster) const;
    [[nodiscard]] std::uint64_t fatOffset() const;
    [[nodiscard]] std::uint32_t bitmapCluster() const noexcept { return bitmapCluster_; }
    [[nodiscard]] std::uint32_t upcaseCluster() const noexcept { return upcaseCluster_; }
    [[nodiscard]] std::uint32_t upcaseBytes() const noexcept { return static_cast<std::uint32_t>(upcase_.size()); }
    [[nodiscard]] std::uint32_t clusterSize() const noexcept { return clusterSize_; }
    [[nodiscard]] std::uint32_t bytesPerSector() const noexcept { return bytesPerSector_; }
    [[nodiscard]] std::uint32_t lastCluster() const noexcept { return options_.clusterCount + 1; }

    // Name hash as the builder computes it.
    [[nodiscard]] std::uint16_t hashOf(std::u16string_view name) const;

    // Writes both boot regions (with checksums) and returns a copy of the image.
    [[nodiscard]] std::vector<std::byte> build();
    [[nodiscard]] std::vector<std::byte>& raw() noexcept { return image_; }

    // Rewrites the checksum sector of the boot region starting at sector
    // `firstSector` (0 = main, 12 = backup) after a test edited it.
    static void rewriteBootChecksum(std::span<std::byte> image, std::uint32_t bytesPerSector,
                                    std::uint32_t firstSector);

private:
    struct Directory {
        std::vector<std::uint32_t> clusters;
        std::size_t nextSlot = 0;
        bool contiguous = true;
        std::optional<Entry> self;  // not set for the root
    };

    std::uint32_t allocate();
    std::vector<std::uint32_t> allocateRun(std::size_t count);
    void chain(const std::vector<std::uint32_t>& clusters);
    void grow(Directory& directory);
    std::span<std::byte> directorySlot(Directory& directory, std::size_t index);
    void updateDirectorySize(const Directory& directory);
    Entry writeEntrySet(std::uint32_t parent, std::string_view name, std::uint16_t attributes,
                        std::uint32_t firstCluster, std::uint64_t dataLength, bool noFatChain,
                        std::size_t vendorExtensions);
    void writeData(const std::vector<std::uint32_t>& clusters, std::span<const std::byte> data);
    void writeBootRegion(std::uint32_t firstSector);

    ExFatBuilderOptions options_;
    std::uint32_t bytesPerSector_;
    std::uint32_t clusterSize_;
    std::uint32_t fatLengthSectors_;
    std::uint32_t heapOffsetSectors_;
    std::uint64_t volumeSectors_;
    std::vector<std::byte> image_;
    std::array<char16_t, 65536> upcaseMap_{};
    std::vector<std::byte> upcase_;  // compressed on-disk table
    std::uint32_t upcaseChecksum_ = 0;
    std::uint32_t bitmapCluster_ = 2;
    std::uint32_t upcaseCluster_ = 0;
    std::uint32_t rootCluster_ = 0;
    std::map<std::uint32_t, Directory> directories_;
    std::uint32_t cursor_ = 2;
};

}  // namespace recovery::test
