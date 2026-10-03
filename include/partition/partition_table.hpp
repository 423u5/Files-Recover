#pragma once

#include "partition/guid.hpp"
#include "recovery/result.hpp"
#include "storage/storage_source.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::partition {

enum class PartitionScheme : std::uint8_t {
    // No recognizable partition table (blank, wiped or damaged sector 0).
    Unknown,
    // Sector 0 holds a volume boot record: the whole device is one volume
    // ("superfloppy", common on USB flash drives).
    Unpartitioned,
    Mbr,
    Gpt,
};

[[nodiscard]] std::string_view toString(PartitionScheme scheme) noexcept;

// Filesystems a partition may contain, judged from its type code or boot
// record signature only. Filesystem modules make the final decision.
struct FilesystemCandidates {
    bool fat = false;
    bool exfat = false;
    bool ntfs = false;

    [[nodiscard]] bool any() const noexcept { return fat || exfat || ntfs; }
    friend bool operator==(const FilesystemCandidates&, const FilesystemCandidates&) = default;
};

enum class PartitionIssueKind : std::uint8_t {
    SectorUnreadable,
    MbrInvalidEntry,
    PartitionOutOfRange,  // starts beyond the device: dropped
    PartitionTruncated,   // extends beyond the device: size clamped
    PartitionOverlap,
    ExtendedChainInvalid,
    ExtendedChainLoop,
    ExtendedChainTooLong,
    ProtectiveMbrMissing,
    ProtectiveMbrInconsistent,
    GptPrimaryInvalid,
    GptBackupInvalid,
    GptHeadersDisagree,
    GptEntryInvalid,
    GptUnavailable,
};

[[nodiscard]] std::string_view toString(PartitionIssueKind kind) noexcept;

struct PartitionIssue {
    PartitionIssueKind kind = PartitionIssueKind::SectorUnreadable;
    std::optional<std::uint32_t> partitionIndex;
    std::string detail;
};

struct Partition {
    // MBR: 0-3 primary, 4+ logical. GPT: entry number. Unpartitioned: 0.
    std::uint32_t index = 0;
    std::uint64_t firstLba = 0;
    // Clamped to the device; see `truncated`.
    std::uint64_t sectorCount = 0;
    std::uint64_t offset = 0;  // bytes from the start of the device
    std::uint64_t size = 0;    // bytes
    // The table claims more sectors than the device holds.
    bool truncated = false;

    // MBR only.
    std::uint8_t mbrType = 0;
    bool bootable = false;
    bool logical = false;

    // GPT only.
    Guid typeGuid;
    Guid uniqueGuid;
    std::uint64_t attributes = 0;
    std::string name;  // UTF-8

    std::string typeName;
    FilesystemCandidates candidates;
};

struct GptInfo {
    Guid diskGuid;
    std::uint64_t headerLba = 0;  // header actually used (1 = primary)
    std::uint64_t firstUsableLba = 0;
    std::uint64_t lastUsableLba = 0;
    std::uint32_t entryCount = 0;
    std::uint32_t entrySize = 0;
    bool primaryValid = false;
    bool backupValid = false;
};

struct PartitionTable {
    PartitionScheme scheme = PartitionScheme::Unknown;
    std::uint32_t sectorSize = 0;
    std::uint64_t deviceSectors = 0;
    // Volumes only (MBR extended containers are not listed), in table order.
    // Every entry lies entirely inside the device.
    std::vector<Partition> partitions;
    std::optional<GptInfo> gpt;
    std::vector<PartitionIssue> issues;

    [[nodiscard]] bool hasIssue(PartitionIssueKind kind) const noexcept;
};

// Limits applied to untrusted partition metadata.
inline constexpr std::uint32_t kMaxLogicalPartitions = 128;
inline constexpr std::uint32_t kMaxGptEntries = 16384;
inline constexpr std::uint32_t kMaxGptEntrySize = 4096;
inline constexpr std::size_t kMaxGptEntryArrayBytes = 4 * kMiB;

// Detects and parses the partition table of `source`.
//
// Malformed metadata is never fatal: it is reported in `issues`, and every
// partition returned has been validated against the size of the source.
// Fails only when the source itself is unusable (not open, invalid sector
// size).
[[nodiscard]] Result<PartitionTable> readPartitionTable(storage::IStorageSource& source);

// Candidates implied by an MBR partition type byte.
[[nodiscard]] FilesystemCandidates candidatesForMbrType(std::uint8_t type) noexcept;
// Candidates implied by a GPT partition type GUID.
[[nodiscard]] FilesystemCandidates candidatesForGptType(const Guid& type) noexcept;
// Candidates when `sector` is a FAT/exFAT/NTFS volume boot record; none otherwise.
[[nodiscard]] FilesystemCandidates sniffVolumeBootRecord(std::span<const std::byte> sector) noexcept;

}  // namespace recovery::partition
