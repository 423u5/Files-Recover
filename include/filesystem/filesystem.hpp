#pragma once

// Filesystem-independent model shared by every filesystem module (FAT32,
// exFAT and NTFS). Recovery code depends only on these types, never on
// on-disk structures of a particular filesystem.

#include "recovery/cancellation.hpp"
#include "recovery/result.hpp"
#include "recovery/strong_types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::filesystem {

enum class FilesystemType : std::uint8_t {
    Fat32,
    ExFat,
    Ntfs,
};

[[nodiscard]] std::string_view toString(FilesystemType type) noexcept;

using ClusterNumber = StrongValue<struct ClusterNumberTag, std::uint64_t>;

struct FilesystemInfo {
    FilesystemType type = FilesystemType::Fat32;
    std::string label;  // UTF-8, empty if none
    std::uint64_t serialNumber = 0;
    std::uint32_t bytesPerSector = 0;
    std::uint32_t clusterSize = 0;
    // Number of data clusters, numbered firstCluster .. firstCluster + clusterCount - 1
    // (FAT32 and exFAT start at 2, NTFS at 0).
    std::uint64_t clusterCount = 0;
    std::uint64_t firstCluster = 0;
    // Bytes described by the boot record (may exceed the source if truncated).
    std::uint64_t volumeSize = 0;
    // Byte offset of the first data cluster within the volume.
    std::uint64_t dataOffset = 0;
    // Non-fatal oddities found while opening (non-standard values, invalid
    // primary boot sector, ...).
    std::vector<std::string> warnings;
};

// A byte range of the volume.
struct Extent {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;

    friend bool operator==(const Extent&, const Extent&) = default;
};

enum class AllocationMethod : std::uint8_t {
    // No data: empty file, or no usable start cluster.
    None,
    // Followed through the filesystem's allocation table. For a deleted entry
    // (exFAT keeps the table entries after deletion) the chain is old
    // evidence that recovery must validate.
    ClusterChain,
    // The chain is gone (deleted entry); contiguity from the start cluster is
    // an assumption that recovery must validate.
    ContiguousGuess,
    // The filesystem records the data as one run from the start cluster
    // (exFAT "NoFatChain"). Exact for active entries; for deleted entries the
    // run may have been reused (see ClustersInUse).
    Contiguous,
    // The filesystem records the data as a list of runs (NTFS data runs),
    // which survives deletion. Exact for active entries; for deleted entries
    // the runs may have been reused (see ClustersInUse).
    RunList,
    // The data is stored inside the metadata record itself (NTFS resident
    // data). It is in FileAllocation::residentData; there are no extents.
    Resident,
};

[[nodiscard]] std::string_view toString(AllocationMethod method) noexcept;

enum class AllocationIssue : std::uint8_t {
    InvalidStartCluster,
    InvalidClusterInChain,
    FreeClusterInChain,
    BadClusterInChain,
    ChainLoop,
    CrossLinked,
    ChainShorterThanSize,
    ChainLongerThanSize,
    UnreadableAllocationTable,
    // Deleted entry: some of its clusters are allocated to other data.
    ClustersInUse,
    // The run would leave the cluster area; truncated.
    BeyondVolume,
    SizeExceedsVolume,
    // Active entry: the allocation bitmap marks some of its clusters as free.
    ClustersMarkedFree,
    // NTFS: the run list is malformed (truncated, zero-length or oversized
    // fields, or it disagrees with the attribute's VCN range). Runs decoded
    // before the damage are kept.
    InvalidRunList,
    // NTFS: part of the data is a sparse hole with no clusters (it reads as
    // zeros). The extents list only the stored runs, so they cannot simply
    // be concatenated.
    SparseRuns,
    // NTFS: the data is stored compressed; the extents hold compressed units.
    CompressedData,
    // NTFS: the data is stored encrypted (EFS); the extents hold ciphertext.
    EncryptedData,
    // NTFS: the record has an attribute list. Its extension records are not
    // read yet, so runs (or the whole data attribute) stored there are missing.
    AttributeListNotFollowed,
    // NTFS: a file record without a data attribute (and no attribute list).
    DataAttributeMissing,
};

[[nodiscard]] std::string_view toString(AllocationIssue issue) noexcept;

struct FileAllocation {
    AllocationMethod method = AllocationMethod::None;
    // Volume byte ranges holding the data, in file order. For files the last
    // extent is trimmed to the file size; for directories they cover whole
    // clusters. Never trust these blindly: see `issues` and `method`.
    std::vector<Extent> extents;
    // Clusters in the chain, runs or guessed run.
    std::uint64_t clusterCount = 0;
    std::vector<AllocationIssue> issues;
    // Method Resident: the data itself, copied out of the metadata record
    // (NTFS resident data; at most one MFT record). It cannot be described
    // as extents, because NTFS replaces the last two bytes of every 512-byte
    // block of a record on disk (update sequence fixups).
    std::vector<std::byte> residentData;

    // Bytes located: the extents' total, plus any resident data.
    [[nodiscard]] std::uint64_t dataBytes() const noexcept;
    [[nodiscard]] std::size_t fragmentCount() const noexcept { return extents.size(); }
    [[nodiscard]] bool hasIssue(AllocationIssue issue) const noexcept;
};

enum class EntryState : std::uint8_t {
    Active,
    Deleted,
};

enum class EntryIssue : std::uint8_t {
    InvalidShortName,
    LongNameChecksumMismatch,  // the long name did not belong to this entry and was ignored
    LongNameUnverified,        // deleted entry: long name used, but could not be verified
    InvalidTimestamp,
    ReservedAttributeBits,
    DirectoryWithSize,
    // Part of the name was lost when the entry was deleted and has been substituted.
    NameReconstructed,
    // exFAT: the entry set checksum does not match its contents.
    EntrySetChecksumMismatch,
    // exFAT: the name does not match the name hash stored with it.
    NameHashMismatch,
    // Part of the metadata was overwritten: attributes, timestamps and the
    // directory flag are unknown (reported as a plain file).
    MetadataIncomplete,
    // The name is empty or contains characters the filesystem forbids.
    InvalidName,
    // The valid data length is larger than the size.
    ValidDataLengthExceedsSize,
    // NTFS: the directory the entry names as its parent no longer exists (its
    // record was reused or is unreadable). The entry is listed under
    // "/$OrphanFiles", not at its original path.
    ParentMissing,
    // NTFS: the metadata record is malformed after some point; attributes
    // beyond the damage are ignored.
    DamagedRecord,
};

[[nodiscard]] std::string_view toString(EntryIssue issue) noexcept;

struct Timestamp {
    std::chrono::sys_time<std::chrono::milliseconds> time{};
    // FAT stores local time with no zone: such values must not be treated as UTC.
    bool local = true;

    friend bool operator==(const Timestamp&, const Timestamp&) = default;
};

struct EntryAttributes {
    bool readOnly = false;
    bool hidden = false;
    bool system = false;
    bool archive = false;
};

// One item of a directory listing.
struct DirectoryEntry {
    std::string name;       // best available name, UTF-8
    std::string shortName;  // 8.3 name ("IMG_0001.JPG"), when the filesystem has one
    EntryState state = EntryState::Active;
    bool isDirectory = false;
    EntryAttributes attributes;
    std::uint64_t size = 0;
    // Bytes actually written (exFAT ValidDataLength, NTFS initialized size).
    // Data between this and `size` is undefined on disk and reads as zeros.
    // Unset when the filesystem has no such concept (FAT32).
    std::optional<std::uint64_t> validDataLength;
    ClusterNumber firstCluster{0};  // 0 = none (NTFS: resident or no data)
    // The filesystem records the data as one run from firstCluster, with no
    // chain in its allocation table (exFAT NoFatChain), or as a single data
    // run (NTFS).
    bool contiguousData = false;
    std::optional<Timestamp> created;
    std::optional<Timestamp> modified;
    std::optional<Timestamp> accessed;
    // Volume byte offset of the metadata record (FAT: the 8.3 entry; exFAT:
    // the first entry of the entry set that survived; NTFS: the FILE record,
    // which also identifies the entry to readDirectory and resolveAllocation).
    std::uint64_t metadataOffset = 0;
    std::vector<EntryIssue> issues;

    [[nodiscard]] bool hasIssue(EntryIssue issue) const noexcept;
};

// A directory entry placed in the tree, with its data location resolved.
struct FileRecord {
    DirectoryEntry entry;
    std::string path;  // "/DCIM/100MEDIA/IMG_0001.JPG"
    // Found inside a deleted directory (and therefore deleted as well).
    bool parentDeleted = false;
    FileAllocation allocation;
};

enum class ScanIssueKind : std::uint8_t {
    DirectoryLoop,
    DirectoryUnreadable,
    DirectoryInvalid,
    DepthLimit,
    EntryLimit,
    // NTFS: a metadata (FILE) record is malformed and was skipped.
    RecordInvalid,
    // NTFS: a metadata (FILE) record could not be read.
    RecordUnreadable,
};

[[nodiscard]] std::string_view toString(ScanIssueKind kind) noexcept;

struct ScanIssue {
    ScanIssueKind kind = ScanIssueKind::DirectoryUnreadable;
    std::string path;
    std::string detail;
};

struct DirectoryListing {
    std::vector<DirectoryEntry> entries;  // includes deleted entries; "." and ".." omitted
    std::vector<ScanIssue> issues;
};

struct ScanLimits {
    std::uint32_t maxDepth = 64;
    std::uint64_t maxEntries = 10'000'000;
    bool includeDeleted = true;
    bool recurseIntoDeletedDirectories = true;
};

struct FileScan {
    // Every file and directory below the root, active and deleted.
    std::vector<FileRecord> records;
    std::vector<ScanIssue> issues;
    // Clusters claimed by active entries (including directories and the root).
    std::uint64_t referencedClusters = 0;
    std::uint64_t crossLinkedClusters = 0;
    // False when a limit stopped the traversal early.
    bool complete = true;
};

enum class ClusterState : std::uint8_t {
    Free,
    Allocated,
    Bad,
    Invalid,     // reserved or out-of-range allocation-table value
    Unreadable,  // the allocation table could not be read here
};

struct ClusterUsage {
    std::uint64_t total = 0;
    std::uint64_t free = 0;
    std::uint64_t allocated = 0;
    std::uint64_t bad = 0;
    std::uint64_t invalid = 0;
    std::uint64_t unreadable = 0;
    // Entries that differ between copies of the allocation table.
    std::uint64_t mirrorMismatches = 0;
    // Free count recorded by the filesystem itself (FAT32 FSInfo), if valid.
    // exFAT records no exact count.
    std::optional<std::uint64_t> recordedFree;
};

// Read-only view of a filesystem.
//
// Implementations never write to the volume, bound every traversal, and
// validate every cluster reference before following it.
//
// Thread safety: not thread-safe (internal caches); use one instance per thread.
class IFilesystem {
public:
    virtual ~IFilesystem() = default;

    [[nodiscard]] virtual const FilesystemInfo& info() const noexcept = 0;
    [[nodiscard]] virtual DirectoryEntry rootDirectory() const = 0;
    [[nodiscard]] virtual Result<DirectoryListing> readDirectory(const DirectoryEntry& directory) = 0;
    [[nodiscard]] virtual Result<FileAllocation> resolveAllocation(const DirectoryEntry& entry) = 0;
    [[nodiscard]] virtual Result<FileScan> scan(const ScanLimits& limits, const CancellationToken& cancel) = 0;
    [[nodiscard]] virtual Result<ClusterUsage> analyzeClusters(const CancellationToken& cancel) = 0;
    [[nodiscard]] virtual Result<ClusterState> clusterState(ClusterNumber cluster) = 0;
};

}  // namespace recovery::filesystem
