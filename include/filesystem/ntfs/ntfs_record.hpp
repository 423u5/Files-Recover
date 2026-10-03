#pragma once

// NTFS on-disk structures below the filesystem level: multi-sector record
// protection (update sequence fixups), run lists, and MFT (FILE) records with
// their attributes. Everything here treats its input as untrusted.

#include "recovery/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::filesystem::ntfs {

// Well-known MFT records.
inline constexpr std::uint64_t kMftRecord = 0;
inline constexpr std::uint64_t kMftMirrorRecord = 1;
inline constexpr std::uint64_t kVolumeRecord = 3;
inline constexpr std::uint64_t kRootRecord = 5;
inline constexpr std::uint64_t kBitmapRecord = 6;
inline constexpr std::uint64_t kBadClusterRecord = 8;
// $MFTMirr holds copies of the first four records.
inline constexpr std::uint64_t kMirroredRecords = 4;

// Attribute types.
inline constexpr std::uint32_t kAttrStandardInformation = 0x10;
inline constexpr std::uint32_t kAttrAttributeList = 0x20;
inline constexpr std::uint32_t kAttrFileName = 0x30;
inline constexpr std::uint32_t kAttrVolumeName = 0x60;
inline constexpr std::uint32_t kAttrVolumeInformation = 0x70;
inline constexpr std::uint32_t kAttrData = 0x80;
inline constexpr std::uint32_t kAttrIndexRoot = 0x90;
inline constexpr std::uint32_t kAttrIndexAllocation = 0xA0;
inline constexpr std::uint32_t kAttrBitmap = 0xB0;
inline constexpr std::uint32_t kAttrEnd = 0xFFFFFFFF;

// Attribute header flags.
inline constexpr std::uint16_t kAttrCompressionMask = 0x00FF;
inline constexpr std::uint16_t kAttrEncrypted = 0x4000;
inline constexpr std::uint16_t kAttrSparse = 0x8000;

// FILE record header flags.
inline constexpr std::uint16_t kRecordInUse = 0x0001;
inline constexpr std::uint16_t kRecordDirectory = 0x0002;  // has a file-name ($I30) index
inline constexpr std::uint16_t kRecordViewIndex = 0x0008;  // has another index ($Secure, $Extend\$Quota, ...)

// $STANDARD_INFORMATION file attribute bits used by the engine.
inline constexpr std::uint32_t kFileReadOnly = 0x0001;
inline constexpr std::uint32_t kFileHidden = 0x0002;
inline constexpr std::uint32_t kFileSystem = 0x0004;
inline constexpr std::uint32_t kFileArchive = 0x0020;

// The update sequence protects every 512-byte block of a record, whatever the
// sector size.
inline constexpr std::size_t kFixupBlockSize = 512;

// A reference to an MFT record: the record number (48 bits) and the sequence
// number the record had when the reference was made (16 bits).
struct FileReference {
    std::uint64_t record = 0;
    std::uint16_t sequence = 0;

    [[nodiscard]] static constexpr FileReference fromRaw(std::uint64_t raw) noexcept {
        return FileReference{raw & 0x0000FFFFFFFFFFFFULL, static_cast<std::uint16_t>(raw >> 48)};
    }
    friend bool operator==(const FileReference&, const FileReference&) = default;
};

enum class FixupStatus : std::uint8_t {
    Ok,
    // The update sequence array does not fit the record, or its size does not
    // match the number of 512-byte blocks.
    InvalidLayout,
    // A block does not end with the update sequence number: a torn write, or damage.
    Mismatch,
};

[[nodiscard]] std::string_view toString(FixupStatus status) noexcept;

// Checks and removes the update sequence protection of a multi-sector record
// (FILE or INDX) in place: the last two bytes of every 512-byte block must
// hold the update sequence number, and are replaced by the values saved in
// the update sequence array. The record is unchanged unless every block matches.
[[nodiscard]] FixupStatus applyFixups(std::span<std::byte> record) noexcept;

// One run of a non-resident attribute: `length` clusters starting at virtual
// cluster `vcn`, stored from logical cluster `lcn`, or a sparse hole.
struct DataRun {
    std::uint64_t vcn = 0;
    std::uint64_t length = 0;
    std::optional<std::uint64_t> lcn;  // nullopt: sparse (no clusters; reads as zeros)

    friend bool operator==(const DataRun&, const DataRun&) = default;
};

enum class RunListProblem : std::uint8_t {
    None,
    // The list reaches the end of the attribute without a terminating zero byte.
    Truncated,
    // A pair without a length field, or with a length or offset field wider than 8 bytes.
    InvalidHeader,
    ZeroLength,
    // A run would start before cluster 0, or its length does not fit a signed 64-bit value.
    InvalidCluster,
    // The VCN range overflows.
    Overflow,
};

[[nodiscard]] std::string_view toString(RunListProblem problem) noexcept;

struct RunList {
    std::vector<DataRun> runs;
    RunListProblem problem = RunListProblem::None;

    // Virtual clusters covered, holes included.
    [[nodiscard]] std::uint64_t vcnCount() const noexcept;
};

// Decodes a mapping-pairs array (the run list of a non-resident attribute),
// whose first run starts at virtual cluster `firstVcn`. Stops at the
// terminating zero byte, at the end of `bytes`, or at the first malformed
// pair; the runs decoded before a problem are kept. Each pair takes at least
// two bytes, so there are at most bytes.size() / 2 runs. Cluster numbers are
// not checked against a volume here.
[[nodiscard]] RunList decodeRunList(std::span<const std::byte> bytes, std::uint64_t firstVcn);

struct StandardInformation {
    // Windows FILETIME values: 100 ns units since 1601-01-01 UTC.
    std::uint64_t created = 0;
    std::uint64_t modified = 0;
    std::uint64_t recordChanged = 0;
    std::uint64_t accessed = 0;
    std::uint32_t fileAttributes = 0;
};

// Namespace of a $FILE_NAME attribute.
enum class NameSpace : std::uint8_t {
    Posix = 0,
    Win32 = 1,
    Dos = 2,  // the 8.3 name of a file that also has a Win32 name
    Win32AndDos = 3,
};

struct FileNameAttribute {
    FileReference parent;
    // Copies kept for the directory index; Windows does not keep them current.
    std::uint64_t realSize = 0;
    std::uint32_t flags = 0;
    NameSpace nameSpace = NameSpace::Posix;
    std::u16string name;
};

struct Attribute {
    std::uint32_t type = 0;
    std::u16string name;
    std::uint16_t flags = 0;
    std::uint16_t id = 0;
    // Where the attribute starts within the record.
    std::uint32_t recordOffset = 0;
    bool nonResident = false;

    // Resident attributes: the value, copied out of the record.
    std::vector<std::byte> value;

    // Non-resident attributes.
    std::uint64_t firstVcn = 0;
    // Last virtual cluster; firstVcn - 1 (wrapping) when the attribute has no clusters.
    std::uint64_t lastVcn = 0;
    std::uint64_t allocatedSize = 0;
    std::uint64_t realSize = 0;
    std::uint64_t initializedSize = 0;
    std::uint16_t compressionUnit = 0;
    RunList runs;

    [[nodiscard]] bool isCompressed() const noexcept { return (flags & kAttrCompressionMask) != 0; }
    [[nodiscard]] bool isEncrypted() const noexcept { return (flags & kAttrEncrypted) != 0; }
    [[nodiscard]] bool isSparse() const noexcept { return (flags & kAttrSparse) != 0; }
    // Size of the value in bytes.
    [[nodiscard]] std::uint64_t dataSize() const noexcept { return nonResident ? realSize : value.size(); }
};

// A parsed MFT (FILE) record.
struct MftRecord {
    std::uint64_t number = 0;
    std::uint16_t sequence = 0;
    std::uint16_t linkCount = 0;
    std::uint16_t flags = 0;
    // Zero for a base record; an extension record refers to its base record.
    std::uint64_t baseRecordRaw = 0;
    // All attributes, in record order.
    std::vector<Attribute> attributes;
    // The first valid $STANDARD_INFORMATION, if any.
    std::optional<StandardInformation> standardInformation;
    // Every valid $FILE_NAME, in record order.
    std::vector<FileNameAttribute> names;
    // Damage inside the attribute chain. Attributes before a break in the
    // chain are kept; a malformed attribute is skipped.
    std::vector<std::string> problems;

    [[nodiscard]] bool inUse() const noexcept { return (flags & kRecordInUse) != 0; }
    [[nodiscard]] bool isDirectory() const noexcept { return (flags & kRecordDirectory) != 0; }
    [[nodiscard]] bool isViewIndex() const noexcept { return (flags & kRecordViewIndex) != 0; }
    [[nodiscard]] bool isBaseRecord() const noexcept { return baseRecordRaw == 0; }
    [[nodiscard]] FileReference baseRecord() const noexcept { return FileReference::fromRaw(baseRecordRaw); }
    [[nodiscard]] bool has(std::uint32_t type) const noexcept;
    // The attribute of this type and name whose runs start at VCN 0 (resident
    // attributes always do), or nullptr.
    [[nodiscard]] const Attribute* find(std::uint32_t type, std::u16string_view name = {}) const noexcept;
};

// True for a record slot that was never written (no signature at all).
[[nodiscard]] bool isBlankRecord(std::span<const std::byte> record) noexcept;

// Parses one FILE record as stored on disk; `number` is its index in the MFT.
// `record` must hold exactly one record (a multiple of 512 bytes, at most
// 64 KiB); its fixups are applied in place. Returns InvalidFormat when the
// record is not a FILE record, fails its fixups, or has a malformed header;
// damage within the attribute chain is reported in MftRecord::problems.
[[nodiscard]] Result<MftRecord> parseMftRecord(std::span<std::byte> record, std::uint64_t number);

// Converts a FILETIME to a UTC time point. nullopt for 0 (not set) and for
// values beyond the FILETIME range (2^63 and above).
[[nodiscard]] std::optional<std::chrono::sys_time<std::chrono::milliseconds>> fromFileTime(
    std::uint64_t fileTime) noexcept;

// Sequence number a record had before its last deletion. Deleting a record
// increments its sequence number, skipping zero; a zero sequence number is
// never incremented.
[[nodiscard]] constexpr std::uint16_t previousSequence(std::uint16_t sequence) noexcept {
    if (sequence == 0) {
        return 0;
    }
    return sequence == 1 ? std::uint16_t{0xFFFF} : static_cast<std::uint16_t>(sequence - 1);
}

}  // namespace recovery::filesystem::ntfs
