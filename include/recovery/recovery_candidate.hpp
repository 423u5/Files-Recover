#pragma once

// Recovery candidates: files the engine can reconstruct, where each of their
// bytes is on the source, and the evidence they were found with.
//
// P7 produces candidates from filesystem metadata (RecoveryMethod::Filesystem).
// MP4 recovery (P12, mp4_recovery.hpp) adds carved and hybrid ones, whose
// filesystem evidence is empty or partial (see RecoveryMethod), and fragment
// reconstruction (P13, fragment_recovery.hpp) fragmented ones. Every offset
// in a candidate refers to the whole source (disk or image), not to the
// volume it was found on, so a candidate can be reconstructed from the
// source alone.

#include "filesystem/filesystem.hpp"
#include "recovery/result.hpp"
#include "recovery/strong_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace recovery {

using CandidateId = StrongValue<struct CandidateIdTag, std::uint64_t>;

enum class RecoveryMethod : std::uint8_t {
    // Name, size and data location come from filesystem metadata (a directory
    // entry or an MFT record).
    Filesystem,
    // Found by its content alone (carving): no filesystem metadata names it.
    // The filesystem evidence is empty, and the name is made up
    // ("recovered_000001.mp4").
    Carving,
    // Filesystem metadata names the file and says where it starts; the file's
    // own structure, read by carving, gives or confirms its layout and length
    // (a deleted file whose guessed layout the structure validates, or a
    // carve that starts at a deleted entry's first cluster).
    Hybrid,
    // Reconstructed from several fragments by fragment reconstruction (P13,
    // fragment_recovery.hpp): where each piece is was inferred from the
    // allocation, the file's structure and the evidence of other files, and
    // the layout was validated as a whole. Filesystem evidence is kept when
    // the file's metadata named it.
    Fragmented,
};

[[nodiscard]] std::string_view toString(RecoveryMethod method) noexcept;

enum class RegionKind : std::uint8_t {
    // Stored on the source, at SourceRegion::sourceOffset.
    Stored,
    // Stored inside the metadata record itself (NTFS resident data), at
    // SourceRegion::sourceOffset within RecoveryCandidate::embeddedData.
    Embedded,
    // Reads as zeros without being stored: a sparse hole, or bytes beyond the
    // valid data length (their clusters hold stale data).
    Zeros,
    // The metadata does not say where these bytes are: a chain or run list
    // cut short, a start cluster that is invalid, data beyond the volume, or
    // data stored in a form the engine cannot decode (compressed, encrypted).
    Missing,
};

[[nodiscard]] std::string_view toString(RegionKind kind) noexcept;

// One piece of a candidate: file bytes [fileOffset, fileOffset + length).
struct SourceRegion {
    std::uint64_t fileOffset = 0;
    std::uint64_t length = 0;
    RegionKind kind = RegionKind::Missing;
    // Stored: byte offset on the source. Embedded: offset within
    // RecoveryCandidate::embeddedData. Zero otherwise.
    std::uint64_t sourceOffset = 0;
    // Stored, deleted entries: the clusters have been allocated to other data
    // since the deletion, so these bytes may have been overwritten.
    bool reallocated = false;

    friend bool operator==(const SourceRegion&, const SourceRegion&) = default;
};

// How the data's layout is known.
enum class LayoutEvidence : std::uint8_t {
    // Nothing is located: an empty file, or no usable allocation.
    None,
    // The metadata records where every located byte is: an allocation-table
    // chain, data runs, a contiguous-data flag, or resident data. A file that
    // fits in one cluster also needs no more than its recorded first cluster.
    Recorded,
    // Only the first cluster survived (a deleted FAT32 entry, or a deleted
    // exFAT entry whose chain was cleared). The data is assumed to continue
    // contiguously from it, which is wrong if the file was fragmented.
    Guessed,
};

[[nodiscard]] std::string_view toString(LayoutEvidence evidence) noexcept;

struct AllocationInfo {
    filesystem::AllocationMethod method = filesystem::AllocationMethod::None;
    LayoutEvidence layout = LayoutEvidence::None;
    filesystem::ClusterNumber firstCluster{0};
    std::uint64_t clusterCount = 0;
    std::uint32_t clusterSize = 0;
    // As reported by the filesystem.
    std::vector<filesystem::AllocationIssue> issues;

    [[nodiscard]] bool hasIssue(filesystem::AllocationIssue issue) const noexcept;
};

struct FragmentationInfo {
    // Pieces of stored data that do not continue physically from the piece
    // before them (0 when nothing is stored on the source).
    std::size_t fragmentCount = 0;
    // False when the layout is a guess: the file may have had more fragments.
    bool known = true;

    [[nodiscard]] bool fragmented() const noexcept { return fragmentCount > 1; }
};

// What the filesystem metadata says about a candidate.
struct FilesystemEvidence {
    filesystem::FilesystemType type = filesystem::FilesystemType::Fat32;
    // Byte offset of the volume on the source.
    std::uint64_t volumeOffset = 0;
    // Source offset of the metadata record (FAT32: the 8.3 entry; exFAT: the
    // first surviving entry of the set; NTFS: the FILE record).
    std::uint64_t metadataOffset = 0;
    // Original path within the volume ("/DCIM/100MEDIA/IMG_0001.JPG"). NTFS
    // entries whose parent directory is gone are under "/$OrphanFiles".
    std::string path;
    std::string shortName;
    filesystem::EntryState state = filesystem::EntryState::Active;
    // Found inside a deleted directory.
    bool parentDeleted = false;
    std::optional<std::uint64_t> validDataLength;
    std::optional<filesystem::Timestamp> created;
    std::optional<filesystem::Timestamp> modified;
    std::optional<filesystem::Timestamp> accessed;
    filesystem::EntryAttributes attributes;
    std::vector<filesystem::EntryIssue> entryIssues;
    AllocationInfo allocation;

    [[nodiscard]] bool hasIssue(filesystem::EntryIssue issue) const noexcept;
};

// Consequences of the evidence that a user (or a later validation stage)
// must know about. Each has a precise cause, documented below.
enum class CandidateWarning : std::uint8_t {
    // The layout is Guessed and spans more than one cluster.
    LayoutGuessed,
    // Some bytes are Missing.
    DataMissing,
    // Deleted entry: some stored regions are reallocated.
    ClustersReallocated,
    // Compressed or encrypted data: the stored bytes are not the file's content.
    DataNotDecoded,
    // Active entry: clusters are shared with another entry.
    CrossLinked,
    // The allocation metadata is damaged or inconsistent: loops, invalid or
    // bad clusters in a chain, a malformed run list, an invalid start
    // cluster, a size larger than the volume, a chain longer than the size,
    // or an active entry whose clusters are marked free.
    AllocationDamaged,
    // The allocation table could not be read where this file's data is: a
    // chain may be cut short, and reuse of a deleted file's clusters cannot
    // be checked.
    AllocationUnknown,
    // The name may not be the original: rebuilt, unverified, not matching its
    // hash or checksum, or containing forbidden characters.
    NameUncertain,
    // The original directory is gone; the path is under "/$OrphanFiles".
    LocationUnknown,
    // The metadata record is damaged or incomplete (checksum mismatch,
    // damaged record, lost attributes, invalid timestamps or attribute bits,
    // valid data length larger than the size).
    MetadataDamaged,
};

[[nodiscard]] std::string_view toString(CandidateWarning warning) noexcept;

struct RecoveryCandidate {
    CandidateId id{0};
    RecoveryMethod method = RecoveryMethod::Filesystem;
    // Original name (UTF-8), exactly as the metadata records it; it may
    // contain characters Windows does not allow (see output_names.hpp).
    std::string filename;
    // Lower-case extension without the dot; empty when there is none.
    std::string extension;
    std::uint64_t expectedSize = 0;
    // The whole file in order: regions are contiguous in file offsets, start
    // at 0 and end at expectedSize. Adjacent regions differ in kind, source
    // continuity or reallocation.
    std::vector<SourceRegion> sourceRegions;
    // Data held by the metadata itself (Embedded regions).
    std::vector<std::byte> embeddedData;
    FragmentationInfo fragmentation;
    FilesystemEvidence filesystemEvidence;
    std::vector<CandidateWarning> warnings;

    // Deleted itself, or found inside a deleted directory.
    [[nodiscard]] bool isDeleted() const noexcept;
    [[nodiscard]] bool hasWarning(CandidateWarning warning) const noexcept;
    // Source offset of the first stored byte, if any.
    [[nodiscard]] std::optional<std::uint64_t> sourceOffset() const noexcept;
    // Total length of the regions of this kind.
    [[nodiscard]] std::uint64_t bytes(RegionKind kind) const noexcept;
    [[nodiscard]] std::uint64_t reallocatedBytes() const noexcept;
};

// Checks the structural invariants of a candidate (regions contiguous from 0
// to expectedSize, non-empty, no overflow, Embedded regions inside
// embeddedData). Candidates may come from outside the engine (a saved
// session), so readers call this before using one.
[[nodiscard]] Status validateCandidate(const RecoveryCandidate& candidate);

}  // namespace recovery
