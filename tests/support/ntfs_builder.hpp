#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::test {

struct NtfsBuilderOptions {
    std::uint16_t bytesPerSector = 512;
    std::uint32_t sectorsPerCluster = 1;  // 512-byte clusters keep test images small
    std::uint64_t clusterCount = 8192;
    std::uint32_t recordSize = 1024;
    std::uint32_t indexRecordSize = 4096;
    std::uint32_t mftRecords = 128;  // preallocated and formatted
    // Store the second half of the MFT away from the first (two data runs).
    bool fragmentedMft = false;
    std::string label = "TESTVOL";  // UTF-8
    std::uint64_t volumeSerial = 0x1122334455667788;
};

// One run of a non-resident attribute, for addFileWithRuns: `length`
// clusters stored from `lcn`, or a sparse hole when `lcn` is unset.
struct NtfsRunSpec {
    std::optional<std::uint64_t> lcn;
    std::uint64_t length = 0;
};

// Builds NTFS 3.1 volumes in memory, laid out the way mkntfs and Windows lay
// them out: boot sector and backup (last sector); $MFT after $Boot, with
// system records 0-11 ($MFT, $MFTMirr, $LogFile, $Volume, $AttrDef, root,
// $Bitmap, $Boot, $BadClus, $Secure, $UpCase, $Extend), reserved records
// 12-15 and user records from 64; $MFTMirr in the middle of the volume;
// FILE records protected by update sequence fixups; $STANDARD_INFORMATION,
// $FILE_NAME (Win32, or Win32 & DOS for names that are valid 8.3 names) and
// $DATA attributes; data resident in the record when it fits, otherwise in
// data runs; and deletion by clearing the in-use flag, incrementing the
// sequence number and freeing the clusters in $Bitmap and the record in the
// MFT bitmap. A new record takes the lowest free user record, so a deleted
// record is reused (as Windows does).
//
// Run lists, fixups and attributes are encoded by the builder itself, not
// with engine code. Directory indexes are written empty (the engine does not
// read them yet), except that the root index lists $Secure, which ntfs-3g
// needs to mount the volume. $AttrDef holds no definitions.
//
// Every record is stamped 2024-05-17 11:45:30 UTC (created at .500 s).
class NtfsImageBuilder {
public:
    struct Entry {
        std::uint64_t record = 0;
        std::uint64_t parent = 0;
        // Data clusters (index clusters for directories), in file order.
        std::vector<std::uint64_t> clusters;
        bool isDirectory = false;
    };

    static constexpr std::uint64_t kFirstUserRecord = 64;

    explicit NtfsImageBuilder(NtfsBuilderOptions options = {});

    [[nodiscard]] static constexpr std::uint64_t root() noexcept { return 5; }

    // A directory; with `indexClusters`, it also owns that many index clusters
    // ($INDEX_ALLOCATION), like a directory too large for its record.
    Entry addDirectory(std::uint64_t parent, std::string_view name, std::size_t indexClusters = 0);
    // Resident when the data fits in the record, otherwise one contiguous run.
    Entry addFile(std::uint64_t parent, std::string_view name, std::span<const std::byte> data);
    // Non-resident, stored in exactly these clusters, in this order.
    Entry addFileInClusters(std::uint64_t parent, std::string_view name, std::span<const std::byte> data,
                            const std::vector<std::uint64_t>& clusters);
    // Non-resident with explicit runs (holes allowed) and attribute flags
    // (0x0001 compressed, 0x4000 encrypted, 0x8000 sparse). `data` is written
    // to the stored runs at its position in the file; hole bytes are dropped.
    Entry addFileWithRuns(std::uint64_t parent, std::string_view name, std::span<const std::byte> data,
                          const std::vector<NtfsRunSpec>& runs, std::uint16_t attributeFlags = 0);
    // Another name for the same record (a hard link).
    void addHardLink(const Entry& entry, std::uint64_t parent, std::string_view name);
    // Turns the entry's name into a Win32 name and adds `dosName` as its DOS (8.3) alias.
    void setShortName(const Entry& entry, std::string_view dosName);
    // Deletes like Windows: clears the in-use flag, increments the sequence
    // number (skipping zero) and frees the record in the MFT bitmap and,
    // with `freeClusters`, the clusters of every non-resident attribute.
    void deleteEntry(const Entry& entry, bool freeClusters = true);

    // Lists clusters in $BadClus:$Bad (and marks them allocated).
    void markBadClusters(std::uint64_t first, std::uint64_t count);
    void setAllocated(std::uint64_t cluster, bool allocated);
    [[nodiscard]] bool isAllocated(std::uint64_t cluster) const;
    [[nodiscard]] std::uint64_t allocatedClusters() const;
    // Makes later allocations start `count` clusters further on.
    void skipClusters(std::uint64_t count);

    // The record's unprotected bytes (build() applies the fixups). Edits are
    // lost if the builder rewrites the record afterwards (a new name, a deletion).
    [[nodiscard]] std::span<std::byte> record(std::uint64_t number);
    // Offset of the first attribute of this type and name within the record.
    [[nodiscard]] std::size_t attributeOffset(std::uint64_t number, std::uint32_t type,
                                              std::u16string_view name = {}) const;
    [[nodiscard]] std::uint16_t sequence(std::uint64_t number) const;
    void setSequence(std::uint64_t number, std::uint16_t sequence);
    // Volume offset of record `number` (after build(), for damaging the image).
    [[nodiscard]] std::uint64_t recordOffset(std::uint64_t number) const;
    [[nodiscard]] std::uint64_t clusterOffset(std::uint64_t cluster) const;
    [[nodiscard]] std::uint32_t clusterSize() const noexcept { return clusterSize_; }
    [[nodiscard]] std::uint32_t recordSize() const noexcept { return options_.recordSize; }
    [[nodiscard]] std::uint32_t bytesPerSector() const noexcept { return options_.bytesPerSector; }
    [[nodiscard]] std::uint64_t clusterCount() const noexcept { return options_.clusterCount; }
    [[nodiscard]] std::uint64_t mftCluster() const noexcept { return mftRuns_.front().first; }
    [[nodiscard]] std::uint64_t mftMirrorCluster() const noexcept { return mirrorCluster_; }

    // FILETIME of the timestamps the builder writes.
    [[nodiscard]] static std::uint64_t createdTime();
    [[nodiscard]] static std::uint64_t modifiedTime();

    // Writes the records (with fixups), $MFTMirr, both bitmaps and both boot
    // sectors, and returns a copy of the image.
    [[nodiscard]] std::vector<std::byte> build();
    [[nodiscard]] std::vector<std::byte>& raw() noexcept { return image_; }

private:
    struct Attr {
        std::uint32_t type = 0;
        std::u16string name;
        std::vector<std::byte> bytes;  // the whole attribute record
    };
    struct Record {
        std::uint16_t sequence = 1;
        std::uint16_t flags = 0;
        std::uint16_t links = 0;
        std::vector<Attr> attributes;
        std::vector<std::byte> plain;  // serialized, unprotected
    };
    struct Run {
        std::uint64_t first = 0;
        std::uint64_t count = 0;
    };

    std::uint64_t allocateRecord();
    std::vector<std::uint64_t> allocateRun(std::uint64_t count);
    void claim(const std::vector<std::uint64_t>& clusters);
    void writeData(const std::vector<std::uint64_t>& clusters, std::span<const std::byte> data);
    void serialize(std::uint64_t number);
    void setSystemRecord(std::uint64_t number, std::string_view name, std::uint16_t flags, std::vector<Attr> extra);
    Entry addNamed(std::uint64_t parent, std::string_view name, bool directory, std::vector<Attr> extra);
    void rebuildBadClusters();
    void writeBootSector(std::uint64_t offset);

    NtfsBuilderOptions options_;
    std::uint32_t clusterSize_;
    std::vector<std::byte> image_;
    std::vector<Record> records_;
    std::vector<bool> allocated_;
    std::vector<Run> mftRuns_;
    std::uint64_t mirrorCluster_ = 0;
    std::uint64_t bitmapCluster_ = 0;
    std::uint64_t bitmapBytes_ = 0;
    std::uint64_t mftBitmapCluster_ = 0;
    std::vector<Run> badRuns_;
    std::uint64_t cursor_ = 0;
};

}  // namespace recovery::test
