// MBR / GPT detection and parsing.
//
// Everything read from disk is untrusted: every LBA and length is checked
// against the device size with overflow-safe arithmetic before it is used,
// every loop has a fixed upper bound, and all allocations are capped.

#include "partition/partition_table.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"
#include "recovery/crc32.hpp"
#include "recovery/unicode.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <set>
#include <vector>

namespace recovery::partition {

namespace {

using storage::IStorageSource;

constexpr std::size_t kMbrSize = 512;
constexpr std::size_t kMbrTableOffset = 446;
constexpr std::size_t kMbrEntrySize = 16;
constexpr std::uint8_t kProtectiveType = 0xEE;
constexpr std::size_t kGptHeaderMinSize = 92;
constexpr std::size_t kGptNameUnits = 36;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct MbrEntry {
    std::uint8_t status = 0;
    std::uint8_t type = 0;
    std::uint32_t startLba = 0;
    std::uint32_t sectorCount = 0;
};

struct MbrSector {
    bool signatureValid = false;
    // Every entry has status 0x00 or 0x80; otherwise the sector is not an MBR.
    bool statusBytesValid = false;
    std::array<MbrEntry, 4> entries{};
};

MbrSector decodeMbr(std::span<const std::byte> sector) {
    MbrSector mbr;
    mbr.signatureValid = loadU8(sector, 510) == 0x55 && loadU8(sector, 511) == 0xAA;
    mbr.statusBytesValid = true;
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t base = kMbrTableOffset + i * kMbrEntrySize;
        MbrEntry& e = mbr.entries[i];
        e.status = loadU8(sector, base);
        e.type = loadU8(sector, base + 4);
        e.startLba = loadLe32(sector, base + 8);
        e.sectorCount = loadLe32(sector, base + 12);
        if (e.status != 0x00 && e.status != 0x80) {
            mbr.statusBytesValid = false;
        }
    }
    return mbr;
}

bool isExtendedType(std::uint8_t type) noexcept {
    return type == 0x05 || type == 0x0F || type == 0x85;
}

std::string mbrTypeName(std::uint8_t type) {
    switch (type) {
    case 0x01:
        return "FAT12";
    case 0x04:
    case 0x06:
    case 0x0E:
        return "FAT16";
    case 0x07:
        return "NTFS/exFAT";
    case 0x0B:
    case 0x0C:
        return "FAT32";
    case 0x05:
    case 0x0F:
    case 0x85:
        return "Extended";
    case 0x27:
        return "Windows recovery";
    case 0x82:
        return "Linux swap";
    case 0x83:
        return "Linux";
    case 0xEE:
        return "GPT protective";
    case 0xEF:
        return "EFI system";
    default:
        return "Type 0x" + std::format("{:02X}", type);
    }
}

std::string gptTypeName(const Guid& type) {
    if (type == gpt_types::kEfiSystem) {
        return "EFI system";
    }
    if (type == gpt_types::kMicrosoftBasicData) {
        return "Microsoft basic data";
    }
    if (type == gpt_types::kMicrosoftReserved) {
        return "Microsoft reserved";
    }
    if (type == gpt_types::kWindowsRecovery) {
        return "Windows recovery";
    }
    if (type == gpt_types::kLinuxFilesystem) {
        return "Linux filesystem";
    }
    if (type == gpt_types::kAppleHfsPlus) {
        return "Apple HFS+";
    }
    if (type == gpt_types::kAppleApfs) {
        return "Apple APFS";
    }
    return type.toString();
}

bool bytesEqual(std::span<const std::byte> data, std::size_t offset, std::string_view text) {
    if (offset > data.size() || data.size() - offset < text.size()) {
        return false;
    }
    return std::memcmp(data.data() + offset, text.data(), text.size()) == 0;
}

// Reads one sector; nullopt when unreadable.
std::optional<std::vector<std::byte>> readSector(IStorageSource& source, std::uint64_t lba, std::uint32_t sectorSize) {
    const std::optional<std::uint64_t> offset = checkedMul<std::uint64_t>(lba, sectorSize);
    if (!offset) {
        return std::nullopt;
    }
    std::vector<std::byte> sector(sectorSize);
    if (!source.readExact(ByteOffset{*offset}, sector).ok()) {
        return std::nullopt;
    }
    return sector;
}

// ---------------------------------------------------------------------------
// Parser state
// ---------------------------------------------------------------------------

class TableReader {
public:
    TableReader(IStorageSource& source, PartitionTable& table) : source_(source), table_(table) {}

    void run();

private:
    void issue(PartitionIssueKind kind, std::string detail, std::optional<std::uint32_t> index = std::nullopt) {
        table_.issues.push_back(PartitionIssue{kind, index, std::move(detail)});
    }

    // Validates [firstLba, firstLba + sectorCount) against the device and adds it.
    // `sectorCount` may be any value, including ones that overflow.
    bool addPartition(Partition partition, std::uint64_t sectorCount);

    void parseMbr(const MbrSector& mbr);
    void walkExtended(std::uint32_t containerIndex, std::uint64_t start, std::uint64_t count);
    bool tryGpt(const std::optional<MbrSector>& mbr);
    void checkOverlaps();

    IStorageSource& source_;
    PartitionTable& table_;
};

bool TableReader::addPartition(Partition partition, std::uint64_t sectorCount) {
    const std::uint64_t device = table_.deviceSectors;
    if (sectorCount == 0) {
        issue(PartitionIssueKind::MbrInvalidEntry, "partition has zero sectors", partition.index);
        return false;
    }
    if (partition.firstLba >= device) {
        issue(PartitionIssueKind::PartitionOutOfRange,
              "partition starts at LBA " + std::to_string(partition.firstLba) + " beyond the device (" +
                  std::to_string(device) + " sectors)",
              partition.index);
        return false;
    }
    const std::uint64_t available = device - partition.firstLba;
    if (sectorCount > available) {
        issue(PartitionIssueKind::PartitionTruncated,
              "partition claims " + std::to_string(sectorCount) + " sectors but only " + std::to_string(available) +
                  " remain on the device",
              partition.index);
        partition.truncated = true;
        sectorCount = available;
    }
    partition.sectorCount = sectorCount;
    // firstLba < device and firstLba + count <= device, and device * sectorSize
    // is at most the source size, so neither product can overflow.
    partition.offset = partition.firstLba * table_.sectorSize;
    partition.size = sectorCount * table_.sectorSize;
    table_.partitions.push_back(std::move(partition));
    return true;
}

void TableReader::run() {
    const std::optional<std::vector<std::byte>> sector0 = readSector(source_, 0, table_.sectorSize);
    if (!sector0) {
        issue(PartitionIssueKind::SectorUnreadable, "sector 0 (MBR) is unreadable");
        if (tryGpt(std::nullopt)) {
            table_.scheme = PartitionScheme::Gpt;
            checkOverlaps();
        }
        return;
    }
    const std::span<const std::byte> first(sector0->data(), kMbrSize);

    const FilesystemCandidates vbr = sniffVolumeBootRecord(*sector0);
    if (vbr.any()) {
        table_.scheme = PartitionScheme::Unpartitioned;
        Partition whole;
        whole.typeName = "Volume boot record at sector 0";
        whole.candidates = vbr;
        whole.sectorCount = table_.deviceSectors;
        whole.offset = 0;
        whole.size = source_.size();  // includes a trailing partial sector, if any
        table_.partitions.push_back(std::move(whole));
        return;
    }

    const MbrSector mbr = decodeMbr(first);
    if (mbr.signatureValid && mbr.statusBytesValid) {
        const bool protective = std::any_of(mbr.entries.begin(), mbr.entries.end(),
                                            [](const MbrEntry& e) { return e.type == kProtectiveType; });
        if (protective) {
            table_.scheme = PartitionScheme::Gpt;
            if (!tryGpt(mbr)) {
                issue(PartitionIssueKind::GptUnavailable, "protective MBR present but no valid GPT header");
                // Hybrid MBR: the remaining MBR entries are the only map left.
                MbrSector hybrid = mbr;
                for (MbrEntry& e : hybrid.entries) {
                    if (e.type == kProtectiveType) {
                        e.type = 0;
                    }
                }
                parseMbr(hybrid);
            }
        } else {
            table_.scheme = PartitionScheme::Mbr;
            parseMbr(mbr);
        }
    } else if (tryGpt(std::nullopt)) {
        table_.scheme = PartitionScheme::Gpt;
        issue(PartitionIssueKind::ProtectiveMbrMissing, "GPT found without a valid protective MBR");
    } else {
        table_.scheme = PartitionScheme::Unknown;
    }
    checkOverlaps();
}

// ---------------------------------------------------------------------------
// MBR
// ---------------------------------------------------------------------------

void TableReader::parseMbr(const MbrSector& mbr) {
    bool haveExtended = false;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const MbrEntry& e = mbr.entries[i];
        if (e.type == 0) {
            continue;
        }
        if (e.startLba == 0) {
            issue(PartitionIssueKind::MbrInvalidEntry, "partition starts at LBA 0 (overlaps the MBR)", i);
            continue;
        }
        if (isExtendedType(e.type)) {
            if (haveExtended) {
                issue(PartitionIssueKind::MbrInvalidEntry, "more than one extended partition", i);
                continue;
            }
            haveExtended = true;
            walkExtended(i, e.startLba, e.sectorCount);
            continue;
        }
        Partition p;
        p.index = i;
        p.firstLba = e.startLba;
        p.mbrType = e.type;
        p.bootable = e.status == 0x80;
        p.typeName = mbrTypeName(e.type);
        p.candidates = candidatesForMbrType(e.type);
        addPartition(std::move(p), e.sectorCount);
    }
}

void TableReader::walkExtended(std::uint32_t containerIndex, std::uint64_t start, std::uint64_t count) {
    const std::uint64_t device = table_.deviceSectors;
    if (count == 0 || start >= device) {
        issue(PartitionIssueKind::ExtendedChainInvalid, "extended partition lies outside the device", containerIndex);
        return;
    }
    const std::uint64_t end = start + std::min(count, device - start);  // exclusive, clamped

    std::set<std::uint64_t> visited;
    std::uint64_t ebr = start;
    std::uint32_t logicalIndex = 4;
    for (std::uint32_t step = 0;; ++step) {
        if (step >= kMaxLogicalPartitions) {
            issue(PartitionIssueKind::ExtendedChainTooLong,
                  "more than " + std::to_string(kMaxLogicalPartitions) + " logical partitions");
            return;
        }
        if (ebr < start || ebr >= end) {
            issue(PartitionIssueKind::ExtendedChainInvalid,
                  "EBR at LBA " + std::to_string(ebr) + " lies outside the extended partition");
            return;
        }
        if (!visited.insert(ebr).second) {
            issue(PartitionIssueKind::ExtendedChainLoop, "EBR chain revisits LBA " + std::to_string(ebr));
            return;
        }
        const std::optional<std::vector<std::byte>> sector = readSector(source_, ebr, table_.sectorSize);
        if (!sector) {
            issue(PartitionIssueKind::SectorUnreadable, "EBR at LBA " + std::to_string(ebr) + " is unreadable");
            return;
        }
        const MbrSector record = decodeMbr(std::span<const std::byte>(sector->data(), kMbrSize));
        if (!record.signatureValid) {
            issue(PartitionIssueKind::ExtendedChainInvalid, "EBR at LBA " + std::to_string(ebr) + " has no signature");
            return;
        }

        const MbrEntry& logical = record.entries[0];
        if (logical.type != 0 && logical.sectorCount != 0) {
            const std::uint64_t first = ebr + logical.startLba;  // cannot overflow: both < 2^33
            if (logical.startLba == 0 || first >= end) {
                issue(PartitionIssueKind::ExtendedChainInvalid, "logical partition lies outside the extended partition",
                      logicalIndex);
            } else {
                Partition p;
                p.index = logicalIndex;
                p.firstLba = first;
                p.mbrType = logical.type;
                p.bootable = logical.status == 0x80;
                p.logical = true;
                p.typeName = mbrTypeName(logical.type);
                p.candidates = candidatesForMbrType(logical.type);
                // Logical partitions must also stay inside their container.
                std::uint64_t sectors = logical.sectorCount;
                if (sectors > end - first) {
                    issue(PartitionIssueKind::PartitionTruncated, "logical partition extends past the extended partition",
                          logicalIndex);
                    sectors = end - first;
                    p.truncated = true;
                }
                addPartition(std::move(p), sectors);
            }
            ++logicalIndex;
        }

        const MbrEntry& next = record.entries[1];
        if (next.type == 0 || next.sectorCount == 0) {
            return;  // end of chain
        }
        if (!isExtendedType(next.type)) {
            issue(PartitionIssueKind::ExtendedChainInvalid, "EBR link has non-extended type " + mbrTypeName(next.type));
            return;
        }
        ebr = start + next.startLba;  // relative to the extended partition start
    }
}

// ---------------------------------------------------------------------------
// GPT
// ---------------------------------------------------------------------------

struct GptHeader {
    std::uint64_t myLba = 0;
    std::uint64_t alternateLba = 0;
    std::uint64_t firstUsableLba = 0;
    std::uint64_t lastUsableLba = 0;
    std::uint64_t entriesLba = 0;
    std::uint32_t entryCount = 0;
    std::uint32_t entrySize = 0;
    std::uint32_t entriesCrc = 0;
    Guid diskGuid;
    std::vector<std::byte> entries;  // raw entry array, CRC verified
};

Result<GptHeader> readGptHeader(IStorageSource& source, std::uint64_t lba, std::uint32_t sectorSize,
                                std::uint64_t deviceSectors) {
    const auto invalid = [lba](const std::string& why) {
        return makeError(ErrorCode::InvalidFormat, "GPT header at LBA " + std::to_string(lba) + ": " + why);
    };
    if (lba >= deviceSectors) {
        return invalid("beyond the device");
    }
    const std::optional<std::vector<std::byte>> sector = readSector(source, lba, sectorSize);
    if (!sector) {
        return makeError(ErrorCode::IoError, "GPT header at LBA " + std::to_string(lba) + " is unreadable");
    }
    const std::span<const std::byte> s(*sector);

    if (!bytesEqual(s, 0, "EFI PART")) {
        return invalid("missing signature");
    }
    if ((loadLe32(s, 8) >> 16) != 1) {
        return invalid("unsupported revision");
    }
    const std::uint32_t headerSize = loadLe32(s, 12);
    if (headerSize < kGptHeaderMinSize || headerSize > sectorSize) {
        return invalid("invalid header size " + std::to_string(headerSize));
    }
    std::vector<std::byte> crcCopy(s.begin(), s.begin() + headerSize);
    storeLe32(crcCopy, 16, 0);
    if (crc32(crcCopy) != loadLe32(s, 16)) {
        return invalid("header CRC mismatch");
    }

    GptHeader h;
    h.myLba = loadLe64(s, 24);
    h.alternateLba = loadLe64(s, 32);
    h.firstUsableLba = loadLe64(s, 40);
    h.lastUsableLba = loadLe64(s, 48);
    h.diskGuid = Guid::fromDisk(s.subspan(56, 16));
    h.entriesLba = loadLe64(s, 72);
    h.entryCount = loadLe32(s, 80);
    h.entrySize = loadLe32(s, 84);
    h.entriesCrc = loadLe32(s, 88);

    if (h.myLba != lba) {
        return invalid("header records its own LBA as " + std::to_string(h.myLba));
    }
    if (h.firstUsableLba > h.lastUsableLba) {
        return invalid("first usable LBA exceeds last usable LBA");
    }
    if (h.entrySize < 128 || h.entrySize > kMaxGptEntrySize || !std::has_single_bit(h.entrySize)) {
        return invalid("invalid entry size " + std::to_string(h.entrySize));
    }
    if (h.entryCount == 0 || h.entryCount > kMaxGptEntries) {
        return invalid("invalid entry count " + std::to_string(h.entryCount));
    }
    const std::uint64_t arrayBytes = static_cast<std::uint64_t>(h.entryCount) * h.entrySize;  // < 2^27
    if (arrayBytes > kMaxGptEntryArrayBytes) {
        return invalid("entry array too large");
    }
    const std::uint64_t arraySectors = (arrayBytes + sectorSize - 1) / sectorSize;
    const std::optional<std::uint64_t> arrayEnd = checkedAdd(h.entriesLba, arraySectors);
    if (h.entriesLba < 2 || !arrayEnd || *arrayEnd > deviceSectors) {
        return invalid("entry array lies outside the device");
    }
    if (h.entriesLba <= lba && lba < *arrayEnd) {
        return invalid("entry array overlaps the header");
    }

    std::vector<std::byte> entries(static_cast<std::size_t>(arraySectors * sectorSize));
    if (!source.readExact(ByteOffset{h.entriesLba * sectorSize}, entries).ok()) {
        return makeError(ErrorCode::IoError, "GPT entry array at LBA " + std::to_string(h.entriesLba) + " is unreadable");
    }
    entries.resize(static_cast<std::size_t>(arrayBytes));
    if (crc32(entries) != h.entriesCrc) {
        return invalid("entry array CRC mismatch");
    }
    h.entries = std::move(entries);
    return h;
}

bool TableReader::tryGpt(const std::optional<MbrSector>& mbr) {
    const std::uint64_t device = table_.deviceSectors;
    const std::uint32_t sectorSize = table_.sectorSize;
    if (device < 3) {
        return false;
    }

    Result<GptHeader> primary = readGptHeader(source_, 1, sectorSize, device);
    const std::uint64_t backupLba =
        primary.ok() && primary->alternateLba < device && primary->alternateLba > 1 ? primary->alternateLba : device - 1;
    Result<GptHeader> backup = readGptHeader(source_, backupLba, sectorSize, device);

    if (!primary.ok() && !backup.ok()) {
        // Without a protective MBR, a missing GPT simply means "no GPT".
        if (mbr.has_value()) {
            issue(PartitionIssueKind::GptPrimaryInvalid, primary.error().message);
            issue(PartitionIssueKind::GptBackupInvalid, backup.error().message);
        }
        return false;
    }
    if (!primary.ok()) {
        issue(PartitionIssueKind::GptPrimaryInvalid, primary.error().message + "; using the backup header");
    }
    if (!backup.ok()) {
        issue(PartitionIssueKind::GptBackupInvalid, backup.error().message);
    }
    if (primary.ok() && backup.ok() &&
        (primary->diskGuid != backup->diskGuid || primary->entriesCrc != backup->entriesCrc ||
         primary->firstUsableLba != backup->firstUsableLba || primary->lastUsableLba != backup->lastUsableLba)) {
        issue(PartitionIssueKind::GptHeadersDisagree, "primary and backup GPT headers describe different tables");
    }

    if (mbr.has_value()) {
        // A standard protective MBR has one 0xEE entry starting at LBA 1.
        const auto& entries = mbr->entries;
        const bool standard = std::any_of(entries.begin(), entries.end(), [](const MbrEntry& e) {
            return e.type == kProtectiveType && e.startLba == 1;
        });
        const bool hybrid = std::any_of(entries.begin(), entries.end(), [](const MbrEntry& e) {
            return e.type != 0 && e.type != kProtectiveType;
        });
        if (!standard || hybrid) {
            issue(PartitionIssueKind::ProtectiveMbrInconsistent,
                  hybrid ? "hybrid MBR: MBR entries besides the protective entry are ignored"
                         : "protective entry does not start at LBA 1");
        }
    }

    const GptHeader& header = primary.ok() ? primary.value() : backup.value();
    GptInfo info;
    info.diskGuid = header.diskGuid;
    info.headerLba = header.myLba;
    info.firstUsableLba = header.firstUsableLba;
    info.lastUsableLba = header.lastUsableLba;
    info.entryCount = header.entryCount;
    info.entrySize = header.entrySize;
    info.primaryValid = primary.ok();
    info.backupValid = backup.ok();
    table_.gpt = info;

    const std::span<const std::byte> array(header.entries);
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const std::span<const std::byte> e = array.subspan(static_cast<std::size_t>(i) * header.entrySize, 128);
        const Guid type = Guid::fromDisk(e.subspan(0, 16));
        if (type.isZero()) {
            continue;
        }
        const std::uint64_t firstLba = loadLe64(e, 32);
        const std::uint64_t lastLba = loadLe64(e, 40);
        if (firstLba > lastLba) {
            issue(PartitionIssueKind::GptEntryInvalid, "first LBA exceeds last LBA", i);
            continue;
        }
        if (firstLba < header.firstUsableLba || lastLba > header.lastUsableLba) {
            issue(PartitionIssueKind::GptEntryInvalid, "partition lies outside the usable LBA range", i);
        }

        Partition p;
        p.index = i;
        p.firstLba = firstLba;
        p.typeGuid = type;
        p.uniqueGuid = Guid::fromDisk(e.subspan(16, 16));
        p.attributes = loadLe64(e, 48);
        std::u16string name = loadUtf16Le(e.subspan(56, kGptNameUnits * 2), kGptNameUnits);
        name.erase(std::find(name.begin(), name.end(), u'\0'), name.end());
        p.name = utf16ToUtf8(name);
        p.typeName = gptTypeName(type);
        p.candidates = candidatesForGptType(type);

        // lastLba - firstLba + 1 overflows only for the full 64-bit range.
        const std::optional<std::uint64_t> count = checkedAdd<std::uint64_t>(lastLba - firstLba, 1);
        addPartition(std::move(p), count.value_or(std::numeric_limits<std::uint64_t>::max()));
    }
    return true;
}

void TableReader::checkOverlaps() {
    std::vector<const Partition*> sorted;
    for (const Partition& p : table_.partitions) {
        sorted.push_back(&p);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Partition* a, const Partition* b) { return a->offset < b->offset; });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        const Partition& previous = *sorted[i - 1];
        if (sorted[i]->offset < previous.offset + previous.size) {
            issue(PartitionIssueKind::PartitionOverlap,
                  "partition " + std::to_string(sorted[i]->index) + " overlaps partition " +
                      std::to_string(previous.index),
                  sorted[i]->index);
        }
    }
}

}  // namespace

std::string_view toString(PartitionScheme scheme) noexcept {
    switch (scheme) {
    case PartitionScheme::Unknown:
        return "Unknown";
    case PartitionScheme::Unpartitioned:
        return "Unpartitioned";
    case PartitionScheme::Mbr:
        return "MBR";
    case PartitionScheme::Gpt:
        return "GPT";
    }
    return "Invalid";
}

std::string_view toString(PartitionIssueKind kind) noexcept {
    switch (kind) {
    case PartitionIssueKind::SectorUnreadable:
        return "SectorUnreadable";
    case PartitionIssueKind::MbrInvalidEntry:
        return "MbrInvalidEntry";
    case PartitionIssueKind::PartitionOutOfRange:
        return "PartitionOutOfRange";
    case PartitionIssueKind::PartitionTruncated:
        return "PartitionTruncated";
    case PartitionIssueKind::PartitionOverlap:
        return "PartitionOverlap";
    case PartitionIssueKind::ExtendedChainInvalid:
        return "ExtendedChainInvalid";
    case PartitionIssueKind::ExtendedChainLoop:
        return "ExtendedChainLoop";
    case PartitionIssueKind::ExtendedChainTooLong:
        return "ExtendedChainTooLong";
    case PartitionIssueKind::ProtectiveMbrMissing:
        return "ProtectiveMbrMissing";
    case PartitionIssueKind::ProtectiveMbrInconsistent:
        return "ProtectiveMbrInconsistent";
    case PartitionIssueKind::GptPrimaryInvalid:
        return "GptPrimaryInvalid";
    case PartitionIssueKind::GptBackupInvalid:
        return "GptBackupInvalid";
    case PartitionIssueKind::GptHeadersDisagree:
        return "GptHeadersDisagree";
    case PartitionIssueKind::GptEntryInvalid:
        return "GptEntryInvalid";
    case PartitionIssueKind::GptUnavailable:
        return "GptUnavailable";
    }
    return "Invalid";
}

bool PartitionTable::hasIssue(PartitionIssueKind kind) const noexcept {
    return std::any_of(issues.begin(), issues.end(), [kind](const PartitionIssue& i) { return i.kind == kind; });
}

FilesystemCandidates candidatesForMbrType(std::uint8_t type) noexcept {
    FilesystemCandidates c;
    switch (type) {
    case 0x01:
    case 0x04:
    case 0x06:
    case 0x0B:
    case 0x0C:
    case 0x0E:
    case 0xEF:
        c.fat = true;
        break;
    case 0x07:
        c.exfat = true;
        c.ntfs = true;
        break;
    case 0x27:
        c.ntfs = true;
        break;
    default:
        break;
    }
    return c;
}

FilesystemCandidates candidatesForGptType(const Guid& type) noexcept {
    FilesystemCandidates c;
    if (type == gpt_types::kEfiSystem) {
        c.fat = true;
    } else if (type == gpt_types::kMicrosoftBasicData) {
        c.fat = true;
        c.exfat = true;
        c.ntfs = true;
    } else if (type == gpt_types::kWindowsRecovery) {
        c.ntfs = true;
    }
    return c;
}

FilesystemCandidates sniffVolumeBootRecord(std::span<const std::byte> s) noexcept {
    FilesystemCandidates c;
    if (s.size() < kMbrSize || loadU8(s, 510) != 0x55 || loadU8(s, 511) != 0xAA) {
        return c;
    }
    const std::uint8_t jump = loadU8(s, 0);
    if (!((jump == 0xEB && loadU8(s, 2) == 0x90) || jump == 0xE9)) {
        return c;
    }
    if (bytesEqual(s, 3, "EXFAT   ")) {
        c.exfat = true;
        return c;
    }
    if (bytesEqual(s, 3, "NTFS    ")) {
        c.ntfs = true;
        return c;
    }
    const std::uint16_t bytesPerSector = loadLe16(s, 11);
    const std::uint8_t sectorsPerCluster = loadU8(s, 13);
    const std::uint16_t reserved = loadLe16(s, 14);
    const std::uint8_t fats = loadU8(s, 16);
    const bool bpbValid = (bytesPerSector == 512 || bytesPerSector == 1024 || bytesPerSector == 2048 ||
                           bytesPerSector == 4096) &&
                          std::has_single_bit(sectorsPerCluster) && reserved != 0 && (fats == 1 || fats == 2);
    const bool fatLabel = bytesEqual(s, 82, "FAT32   ") || bytesEqual(s, 54, "FAT12   ") ||
                          bytesEqual(s, 54, "FAT16   ") || bytesEqual(s, 54, "FAT     ");
    c.fat = bpbValid && fatLabel;
    return c;
}

Result<PartitionTable> readPartitionTable(IStorageSource& source) {
    if (!source.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "source is not open");
    }
    const std::uint32_t sectorSize = source.sectorSize();
    if (!storage::isValidSectorSize(sectorSize)) {
        return makeError(ErrorCode::InvalidInput, "source has an invalid sector size");
    }
    PartitionTable table;
    table.sectorSize = sectorSize;
    table.deviceSectors = source.size() / sectorSize;
    if (table.deviceSectors == 0) {
        return table;  // Unknown: too small to hold any table
    }
    TableReader(source, table).run();
    return table;
}

}  // namespace recovery::partition
