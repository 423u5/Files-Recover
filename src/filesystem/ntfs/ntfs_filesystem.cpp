#include "filesystem/ntfs/ntfs_filesystem.hpp"

#include "../exfat/paged_region.hpp"
#include "cluster_bitmap.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"
#include "recovery/unicode.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace recovery::filesystem::ntfs {

namespace {

using diagnostics::field;
using diagnostics::LogLevel;

constexpr std::string_view kComponent = "ntfs";
constexpr std::size_t kBootSectorSize = 512;
// Clusters counted per step of the cluster analysis (1 MiB of bitmap).
constexpr std::uint64_t kAnalysisSlice = 8ULL * 1024 * 1024;
// Invalid records reported one by one; the rest are only counted.
constexpr std::size_t kMaxRecordIssues = 100;
constexpr std::uint16_t kVolumeDirty = 0x0001;
constexpr std::uint64_t kMaxCatalogRecords = std::numeric_limits<std::uint32_t>::max();

std::uint64_t clustersFor(std::uint64_t bytes, std::uint32_t clusterSize) noexcept {
    return bytes == 0 ? 0 : (bytes - 1) / clusterSize + 1;
}

template <typename Issue>
void addIssue(std::vector<Issue>& issues, Issue issue) {
    if (std::find(issues.begin(), issues.end(), issue) == issues.end()) {
        issues.push_back(issue);
    }
}

// Names NTFS cannot hold in any namespace, and names that would make paths ambiguous.
bool isInvalidName(std::u16string_view name) noexcept {
    return name.empty() || name == u"." || name == u".." || name.find(u'/') != std::u16string_view::npos ||
           name.find(u'\0') != std::u16string_view::npos;
}

// Copies the record at byte `within` of a cached MFT page into `out`; false
// when any sector of it is unreadable.
bool copyRecord(const exfat::PagedRegion::Page& page, std::size_t within, std::uint32_t sectorSize,
                std::span<std::byte> out) {
    if (page.noneReadable || within + out.size() > page.length) {
        return false;
    }
    const std::size_t step = std::min<std::size_t>(sectorSize, out.size());
    for (std::size_t offset = within; offset < within + out.size(); offset += step) {
        if (!page.readable(offset, sectorSize)) {
            return false;
        }
    }
    std::copy_n(page.bytes.begin() + static_cast<std::ptrdiff_t>(within), out.size(), out.begin());
    return true;
}

std::optional<Timestamp> timestampOf(std::uint64_t fileTime, std::vector<EntryIssue>& issues) {
    if (fileTime == 0) {
        return std::nullopt;
    }
    const auto time = fromFileTime(fileTime);
    if (!time) {
        addIssue(issues, EntryIssue::InvalidTimestamp);
        return std::nullopt;
    }
    return Timestamp{*time, false};  // NTFS stores UTC
}

}  // namespace

// Every named record of the MFT, read once. Directories are rebuilt from the
// parent reference of each $FILE_NAME ("placement").
struct NtfsFilesystem::Catalog {
    struct Record {
        std::uint64_t number = 0;
        std::uint16_t sequence = 0;
        DirectoryEntry entry;  // without names
        FileAllocation allocation;
        std::uint32_t firstPlacement = 0;
        std::uint32_t placementCount = 0;

        [[nodiscard]] bool active() const noexcept { return entry.state == EntryState::Active; }
    };
    // One name of a record in one directory (a hard link has several).
    struct Placement {
        std::uint32_t record = 0;  // index into `records`
        FileReference parent;
        std::string name;
        std::string shortName;
        bool invalidName = false;
    };

    std::vector<Record> records;  // in MFT order
    std::unordered_map<std::uint64_t, std::uint32_t> byNumber;
    std::vector<Placement> placements;  // in MFT order
    // Parent record number -> placements naming it, in MFT order.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> children;
    std::vector<ScanIssue> issues;
    std::uint64_t referencedClusters = 0;
    std::uint64_t crossLinkedClusters = 0;

    // True when `placement` belongs in `directory`: the reference carries the
    // directory's sequence number. Deleting a record increments its sequence
    // number, so the entries of a deleted directory carry the previous one
    // (both are accepted, in case an implementation does not increment).
    [[nodiscard]] static bool holds(const Record& directory, const Placement& placement) noexcept {
        if (!directory.entry.isDirectory) {
            return false;
        }
        return placement.parent.sequence == directory.sequence ||
               (!directory.active() && placement.parent.sequence == previousSequence(directory.sequence));
    }

    [[nodiscard]] bool hasParent(const Placement& placement) const {
        const auto parent = byNumber.find(placement.parent.record);
        return parent != byNumber.end() && parent->second != placement.record && holds(records[parent->second], placement);
    }

    [[nodiscard]] DirectoryEntry entryFor(const Placement& placement) const {
        DirectoryEntry entry = records[placement.record].entry;
        entry.name = placement.name;
        entry.shortName = placement.shortName;
        if (placement.invalidName) {
            addIssue(entry.issues, EntryIssue::InvalidName);
        }
        return entry;
    }

    void addPlacements(std::uint32_t index, const MftRecord& record) {
        const auto first = static_cast<std::uint32_t>(placements.size());
        const auto sameName = [&](const FileReference& parent, const std::string& name) {
            return std::any_of(placements.begin() + first, placements.end(), [&](const Placement& p) {
                return p.parent == parent && p.name == name;
            });
        };
        for (const FileNameAttribute& name : record.names) {
            // The root names itself "." in itself.
            if (name.nameSpace == NameSpace::Dos || (record.number == kRootRecord && name.parent.record == kRootRecord)) {
                continue;
            }
            std::string utf8 = utf16ToUtf8(name.name);
            if (sameName(name.parent, utf8)) {
                continue;
            }
            std::string shortName = name.nameSpace == NameSpace::Win32AndDos ? utf8 : std::string();
            placements.push_back(Placement{index, name.parent, std::move(utf8), std::move(shortName), isInvalidName(name.name)});
        }
        // A DOS name is the 8.3 alias of the long name in the same directory.
        for (const FileNameAttribute& name : record.names) {
            if (name.nameSpace != NameSpace::Dos || (record.number == kRootRecord && name.parent.record == kRootRecord)) {
                continue;
            }
            std::string utf8 = utf16ToUtf8(name.name);
            const auto owner = std::find_if(placements.begin() + first, placements.end(), [&](const Placement& p) {
                return p.parent == name.parent && p.shortName.empty();
            });
            if (owner != placements.end()) {
                owner->shortName = std::move(utf8);
            } else if (!sameName(name.parent, utf8)) {
                placements.push_back(Placement{index, name.parent, utf8, utf8, isInvalidName(name.name)});
            }
        }
        records[index].firstPlacement = first;
        records[index].placementCount = static_cast<std::uint32_t>(placements.size()) - first;
    }

    // Records to list under $OrphanFiles: those no directory still holds, and
    // one record of each group that only name each other (a cycle).
    [[nodiscard]] std::vector<std::uint32_t> orphanRoots() const {
        std::vector<std::uint8_t> reached(records.size(), 0);
        std::vector<std::uint32_t> pending;
        const auto reach = [&](std::uint32_t start) {
            reached[start] = 1;
            pending.push_back(start);
            while (!pending.empty()) {
                const Record& current = records[pending.back()];
                pending.pop_back();
                const auto kids = children.find(current.number);
                if (!current.entry.isDirectory || kids == children.end()) {
                    continue;
                }
                for (const std::uint32_t p : kids->second) {
                    const Placement& placement = placements[p];
                    if (reached[placement.record] == 0 && holds(current, placement)) {
                        reached[placement.record] = 1;
                        pending.push_back(placement.record);
                    }
                }
            }
        };

        std::optional<std::uint32_t> root;
        if (const auto found = byNumber.find(kRootRecord); found != byNumber.end()) {
            root = found->second;
            if (records[found->second].entry.isDirectory) {
                reach(found->second);
            }
            reached[found->second] = 1;  // never listed as an entry of its own
        }
        std::vector<std::uint8_t> anchored(records.size(), 0);
        for (const Placement& placement : placements) {
            if (hasParent(placement)) {
                anchored[placement.record] = 1;
            }
        }
        std::vector<std::uint32_t> roots;
        for (std::uint32_t i = 0; i < records.size(); ++i) {
            if (reached[i] == 0 && anchored[i] == 0 && records[i].placementCount > 0) {
                roots.push_back(i);
                reach(i);
            }
        }
        for (std::uint32_t i = 0; i < records.size(); ++i) {
            if (reached[i] == 0 && records[i].placementCount > 0) {
                roots.push_back(i);
                reach(i);
            }
        }
        std::sort(roots.begin(), roots.end());
        return roots;
    }
};

NtfsFilesystem::~NtfsFilesystem() = default;

NtfsFilesystem::NtfsFilesystem(storage::IStorageSource& volume, BootSector boot, NtfsOptions options,
                               diagnostics::Logger* logger)
    : volume_(volume), boot_(std::move(boot)), options_(options), logger_(logger) {
    info_.type = FilesystemType::Ntfs;
    info_.serialNumber = boot_.volumeSerial;
    info_.bytesPerSector = boot_.bytesPerSector;
    info_.clusterSize = boot_.clusterSize;
    info_.clusterCount = boot_.clusterCount;
    info_.firstCluster = 0;
    info_.volumeSize = boot_.volumeBytes;
    info_.dataOffset = 0;
    info_.warnings = boot_.warnings;
}

Result<std::unique_ptr<NtfsFilesystem>> NtfsFilesystem::open(storage::IStorageSource& volume, NtfsOptions options,
                                                             diagnostics::Logger* logger) {
    if (!volume.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "volume source is not open");
    }
    if (volume.size() < kBootSectorSize) {
        return makeError(ErrorCode::UnsupportedFilesystem, "volume is too small for an NTFS boot sector");
    }

    std::array<std::byte, kBootSectorSize> sector{};
    Result<BootSector> primary = makeError(ErrorCode::IoError, "boot sector unreadable");
    if (const Status read = volume.readExact(ByteOffset{0}, sector); read.ok()) {
        primary = parseBootSector(sector, volume.size());
    } else {
        primary = read.error();
    }
    // The backup boot sector is the last sector of the volume, just after the
    // sectors the boot sector counts.
    std::optional<BootSector> backup;
    for (const std::uint64_t bytesPerSector : {512ULL, 1024ULL, 2048ULL, 4096ULL}) {
        if (volume.size() < bytesPerSector + kBootSectorSize) {
            continue;
        }
        const std::uint64_t offset = volume.size() - bytesPerSector;
        if (!volume.readExact(ByteOffset{offset}, sector).ok()) {
            continue;
        }
        Result<BootSector> candidate = parseBootSector(sector, volume.size());
        if (candidate.ok() && candidate->bytesPerSector == bytesPerSector && candidate->volumeBytes == offset) {
            backup = std::move(candidate).value();
            break;
        }
    }

    // The primary boot sector first; the backup when the primary is invalid,
    // or describes an MFT that cannot be used while the backup differs.
    std::vector<BootSector> candidates;
    if (primary.ok()) {
        candidates.push_back(primary.value());
    }
    if (backup && (!primary.ok() || backup->mftCluster != primary->mftCluster ||
                   backup->mftMirrorCluster != primary->mftMirrorCluster ||
                   backup->clusterSize != primary->clusterSize || backup->recordSize != primary->recordSize ||
                   backup->totalSectors != primary->totalSectors)) {
        candidates.push_back(std::move(*backup));
    }
    if (candidates.empty()) {
        return primary.error();
    }

    std::optional<Error> firstError;
    for (BootSector& boot : candidates) {
        const bool isBackup = &boot != &candidates.front() || !primary.ok();
        if (isBackup) {
            const std::string reason =
                primary.ok() ? "the primary boot sector's MFT is unusable (" + firstError->message + ")"
                             : "primary boot sector invalid (" + primary.error().message + ")";
            boot.warnings.insert(boot.warnings.begin(),
                                 reason + "; using the backup boot sector at the end of the volume");
        }
        std::unique_ptr<NtfsFilesystem> fs(new NtfsFilesystem(volume, std::move(boot), options, logger));
        if (const Status mft = fs->loadMft(); !mft.ok()) {
            if (!firstError) {
                firstError = mft.error();
            }
            continue;
        }
        fs->loadSystemFiles();

        fs->log(LogLevel::Info, "NTFS volume opened",
                {field("cluster_size", fs->boot_.clusterSize), field("clusters", fs->boot_.clusterCount),
                 field("record_size", fs->boot_.recordSize), field("mft_records", fs->recordCount_),
                 field("warnings", fs->info_.warnings.size())});
        for (const std::string& warning : fs->info_.warnings) {
            fs->log(LogLevel::Warning, "NTFS volume warning", {field("detail", warning)});
        }
        return fs;
    }
    return *firstError;
}

Status NtfsFilesystem::loadMft() {
    std::vector<std::byte> buffer(boot_.recordSize);
    // Record 0 ($MFT) describes the MFT itself, so it is validated as such.
    const auto load = [&](std::uint64_t cluster) -> Result<MftRecord> {
        const std::uint64_t offset = boot_.clusterOffset(cluster);
        if (!rangeWithin<std::uint64_t>(offset, buffer.size(), volume_.size())) {
            return makeError(ErrorCode::IoError, "it lies beyond the end of the source");
        }
        if (const Status read = volume_.readExact(ByteOffset{offset}, buffer); !read.ok()) {
            return read.error();
        }
        Result<MftRecord> record = parseMftRecord(buffer, kMftRecord);
        if (!record.ok()) {
            return record;
        }
        if (!record->inUse() || !record->isBaseRecord()) {
            return makeError(ErrorCode::InvalidFormat, "it is not an in-use base record");
        }
        const Attribute* data = record->find(kAttrData);
        if (data == nullptr || !data->nonResident || data->runs.runs.empty()) {
            return makeError(ErrorCode::InvalidFormat, "it has no data runs");
        }
        if (data->runs.runs.front().lcn != boot_.mftCluster) {
            return makeError(ErrorCode::InvalidFormat, "its data does not start at the MFT cluster");
        }
        return record;
    };

    Result<MftRecord> record = load(boot_.mftCluster);
    if (!record.ok()) {
        const std::string problem = record.error().message;
        Result<MftRecord> mirror = boot_.isValidCluster(boot_.mftMirrorCluster)
                                       ? load(boot_.mftMirrorCluster)
                                       : Result<MftRecord>(makeError(ErrorCode::InvalidFormat, "no valid location"));
        if (!mirror.ok()) {
            return makeError(ErrorCode::CorruptedFilesystem, "MFT record 0 is unusable (" + problem +
                                                                 "), and so is its copy in $MFTMirr (" +
                                                                 mirror.error().message + ")");
        }
        info_.warnings.push_back("MFT record 0 is unusable (" + problem + "); using its copy in $MFTMirr");
        usedMirror_ = true;
        record = std::move(mirror);
    }

    const Attribute& data = *record->find(kAttrData);
    if (record->has(kAttrAttributeList)) {
        info_.warnings.push_back("$MFT has an attribute list; records beyond the runs stored in record 0 are "
                                 "not reachable");
    }
    const std::uint64_t bytes = std::min(data.realSize, data.initializedSize);
    mftExtents_ = contiguousExtents(data.runs, bytes, "$MFT");
    std::uint64_t stored = 0;
    for (const Extent& extent : mftExtents_) {
        stored += extent.length;
    }
    recordCount_ = stored / boot_.recordSize;
    const std::uint64_t limit = std::min(options_.maxRecords, kMaxCatalogRecords);
    if (recordCount_ > limit) {
        info_.warnings.push_back(std::format("the MFT holds {} records; only the first {} are examined", recordCount_,
                                             limit));
        recordCount_ = limit;
    }
    if (recordCount_ == 0) {
        return makeError(ErrorCode::CorruptedFilesystem, "the MFT holds no complete record");
    }
    if (recordCount_ <= kRootRecord) {
        info_.warnings.push_back(std::format("the MFT holds only {} records", recordCount_));
    }

    // Trim the extents to whole records and index them.
    std::uint64_t remaining = recordCount_ * boot_.recordSize;
    std::vector<Extent> trimmed;
    for (const Extent& extent : mftExtents_) {
        if (remaining == 0) {
            break;
        }
        mftStarts_.push_back(recordCount_ * boot_.recordSize - remaining);
        trimmed.push_back(Extent{extent.offset, std::min(extent.length, remaining)});
        remaining -= trimmed.back().length;
    }
    mftExtents_ = std::move(trimmed);
    mft_ = std::make_unique<exfat::PagedRegion>(volume_, mftExtents_, boot_.bytesPerSector, options_.mftCacheBytes);
    reachableRecords_ = std::min(recordCount_, bytesInSource(mftExtents_, volume_.size()) / boot_.recordSize);
    return success();
}

void NtfsFilesystem::loadSystemFiles() {
    // $Volume: label, version and state.
    if (Result<MftRecord> record = readRecord(kVolumeRecord); record.ok() && record->inUse()) {
        if (const Attribute* name = record->find(kAttrVolumeName); name != nullptr && !name->nonResident) {
            info_.label = utf16ToUtf8(loadUtf16Le(name->value, name->value.size() / 2));
        }
        if (const Attribute* volume = record->find(kAttrVolumeInformation);
            volume != nullptr && !volume->nonResident && volume->value.size() >= 12) {
            majorVersion_ = loadU8(volume->value, 8);
            minorVersion_ = loadU8(volume->value, 9);
            if (majorVersion_ != 3) {
                info_.warnings.push_back(std::format("NTFS version {}.{}; the engine is written for version 3.x",
                                                     majorVersion_, minorVersion_));
            }
            if ((loadLe16(volume->value, 10) & kVolumeDirty) != 0) {
                info_.warnings.push_back("volume is marked dirty (it was not cleanly unmounted)");
            }
        } else {
            info_.warnings.push_back("$Volume has no volume information; the NTFS version is unknown");
        }
    } else {
        info_.warnings.push_back("$Volume record is unusable" +
                                 (record.ok() ? std::string(" (not in use)") : " (" + record.error().message + ")"));
    }

    // $Bitmap: which clusters are in use.
    constexpr std::string_view kNoBitmap = "; cluster allocation is unknown";
    if (Result<MftRecord> record = readRecord(kBitmapRecord); !record.ok()) {
        info_.warnings.push_back("$Bitmap record is unusable (" + record.error().message + ")" + std::string(kNoBitmap));
    } else if (const Attribute* data = record->find(kAttrData); data == nullptr || !data->nonResident) {
        info_.warnings.push_back("$Bitmap has no non-resident data" + std::string(kNoBitmap));
    } else {
        const std::uint64_t needed = (boot_.clusterCount + 7) / 8;
        if (data->realSize < needed) {
            info_.warnings.push_back(std::format("cluster bitmap covers only {} of {} clusters", data->realSize * 8,
                                                 boot_.clusterCount));
        }
        std::vector<Extent> extents = contiguousExtents(data->runs, std::min(data->realSize, needed), "$Bitmap");
        if (extents.empty()) {
            info_.warnings.push_back("cluster bitmap is empty" + std::string(kNoBitmap));
        } else {
            bitmap_ = std::make_unique<ClusterBitmap>(volume_, std::move(extents), boot_.clusterCount,
                                                      boot_.bytesPerSector, options_.bitmapCacheBytes);
        }
    }

    // $BadClus:$Bad is a sparse stream as large as the volume whose stored
    // runs are the bad clusters.
    if (Result<MftRecord> record = readRecord(kBadClusterRecord); !record.ok()) {
        info_.warnings.push_back("$BadClus record is unusable (" + record.error().message +
                                 "); bad clusters are unknown");
    } else if (const Attribute* bad = record->find(kAttrData, u"$Bad"); bad != nullptr && bad->nonResident) {
        for (const DataRun& run : bad->runs.runs) {
            if (run.lcn && boot_.isValidCluster(*run.lcn)) {
                badRuns_.push_back(Run{*run.lcn, std::min(run.length, boot_.clusterCount - *run.lcn)});
            }
        }
        std::sort(badRuns_.begin(), badRuns_.end(), [](const Run& a, const Run& b) { return a.first < b.first; });
        std::vector<Run> merged;
        for (const Run& run : badRuns_) {
            if (!merged.empty() && run.first <= merged.back().first + merged.back().count) {
                merged.back().count = std::max(merged.back().first + merged.back().count, run.first + run.count) -
                                      merged.back().first;
            } else {
                merged.push_back(run);
            }
        }
        badRuns_ = std::move(merged);
    }

    if (Result<MftRecord> root = readRecord(kRootRecord); !root.ok() || !root->inUse() || !root->isDirectory()) {
        info_.warnings.push_back("root directory record is unusable" +
                                 (root.ok() ? std::string(" (not an in-use directory)")
                                            : " (" + root.error().message + ")"));
    }
}

std::vector<Extent> NtfsFilesystem::contiguousExtents(const RunList& runs, std::uint64_t bytes,
                                                      std::string_view what) {
    std::vector<Extent> extents;
    if (runs.problem != RunListProblem::None) {
        info_.warnings.push_back(std::format("{} run list is damaged ({})", what, toString(runs.problem)));
    }
    std::uint64_t remaining = bytes;
    for (const DataRun& run : runs.runs) {
        if (remaining == 0) {
            break;
        }
        if (!run.lcn) {
            info_.warnings.push_back(std::string(what) + " has a sparse run; the data after it is not reachable");
            break;
        }
        if (!boot_.isValidCluster(*run.lcn)) {
            info_.warnings.push_back(std::string(what) + " has a run outside the volume; the data after it is not "
                                                         "reachable");
            break;
        }
        const std::uint64_t available = boot_.clusterCount - *run.lcn;
        const std::uint64_t clusters = std::min(run.length, available);
        const std::uint64_t offset = boot_.clusterOffset(*run.lcn);
        const std::uint64_t length = std::min(clusters * boot_.clusterSize, remaining);
        if (!extents.empty() && extents.back().offset + extents.back().length == offset) {
            extents.back().length += length;
        } else {
            extents.push_back(Extent{offset, length});
        }
        remaining -= length;
        if (clusters < run.length) {
            info_.warnings.push_back(std::string(what) + " extends beyond the volume");
            break;
        }
    }
    return extents;
}

Status NtfsFilesystem::readRecordBytes(std::uint64_t number, std::span<std::byte> out) {
    if (number >= reachableRecords_) {
        return makeError(ErrorCode::IoError, std::format("MFT record {} lies beyond the end of the source", number));
    }
    const std::uint64_t regionOffset = number * boot_.recordSize;
    const exfat::PagedRegion::Page& page = mft_->page(regionOffset / exfat::PagedRegion::kPageBytes);
    const auto within = static_cast<std::size_t>(regionOffset % exfat::PagedRegion::kPageBytes);
    if (!copyRecord(page, within, boot_.bytesPerSector, out)) {
        return makeError(ErrorCode::IoError, std::format("MFT record {} is unreadable", number));
    }
    return success();
}

Result<MftRecord> NtfsFilesystem::readRecord(std::uint64_t number) {
    if (number >= recordCount_) {
        return makeError(ErrorCode::InvalidInput, std::format("MFT record {} is beyond the MFT", number));
    }
    std::vector<std::byte> buffer(boot_.recordSize);
    if (Status read = readRecordBytes(number, buffer); !read.ok()) {
        return read.error();
    }
    if (isBlankRecord(buffer)) {
        return makeError(ErrorCode::InvalidFormat, std::format("MFT record {} was never used", number));
    }
    return parseMftRecord(buffer, number);
}

std::uint64_t NtfsFilesystem::recordOffset(std::uint64_t number) const noexcept {
    const std::uint64_t regionOffset = number * boot_.recordSize;
    const auto next = std::upper_bound(mftStarts_.begin(), mftStarts_.end(), regionOffset);
    const auto index = static_cast<std::size_t>(next - mftStarts_.begin()) - 1;  // number < recordCount_
    return mftExtents_[index].offset + (regionOffset - mftStarts_[index]);
}

std::optional<std::uint64_t> NtfsFilesystem::recordAt(std::uint64_t volumeOffset) const noexcept {
    for (std::size_t i = 0; i < mftExtents_.size(); ++i) {
        const Extent& extent = mftExtents_[i];
        if (volumeOffset < extent.offset || volumeOffset - extent.offset >= extent.length) {
            continue;
        }
        const std::uint64_t regionOffset = mftStarts_[i] + (volumeOffset - extent.offset);
        if (regionOffset % boot_.recordSize != 0) {
            return std::nullopt;
        }
        return regionOffset / boot_.recordSize;
    }
    return std::nullopt;
}

DirectoryEntry NtfsFilesystem::rootDirectory() const {
    DirectoryEntry root;
    root.isDirectory = true;
    root.state = EntryState::Active;
    // An MFT too short to hold the root gets an offset that matches no record.
    root.metadataOffset =
        kRootRecord < recordCount_ ? recordOffset(kRootRecord) : std::numeric_limits<std::uint64_t>::max();
    return root;
}

void NtfsFilesystem::checkRun(std::uint64_t first, std::uint64_t count, bool deleted,
                              std::vector<AllocationIssue>& issues) {
    if (count == 0) {
        return;
    }
    if (bitmap_ == nullptr) {
        addIssue(issues, AllocationIssue::UnreadableAllocationTable);
        return;
    }
    // One reused (or, for active entries, unallocated) cluster is enough to flag the entry.
    const BitCounts counts = bitmap_->count(first, count, deleted ? BitState::Allocated : BitState::Free);
    if (deleted && counts.allocated > 0) {
        addIssue(issues, AllocationIssue::ClustersInUse);
    }
    if (!deleted && counts.free > 0) {
        addIssue(issues, AllocationIssue::ClustersMarkedFree);
    }
    if (counts.unreadable > 0) {
        addIssue(issues, AllocationIssue::UnreadableAllocationTable);
    }
}

FileAllocation NtfsFilesystem::runAllocation(const Attribute& attribute, std::optional<std::uint64_t> size,
                                             bool deleted) {
    FileAllocation allocation;
    allocation.method = AllocationMethod::RunList;
    std::vector<AllocationIssue>& issues = allocation.issues;
    const RunList& list = attribute.runs;
    const std::uint32_t clusterSize = boot_.clusterSize;

    // The runs must cover exactly the VCN range the attribute header states.
    if (list.problem != RunListProblem::None || list.vcnCount() != attribute.lastVcn + 1 - attribute.firstVcn) {
        addIssue(issues, AllocationIssue::InvalidRunList);
    }
    if (attribute.isCompressed()) {
        addIssue(issues, AllocationIssue::CompressedData);
    }
    if (attribute.isEncrypted()) {
        addIssue(issues, AllocationIssue::EncryptedData);
    }
    if (size && *size > boot_.clusterCount * clusterSize) {
        addIssue(issues, AllocationIssue::SizeExceedsVolume);
    }

    // Files are read up to their size; directories (no size) in whole clusters.
    const std::uint64_t neededVcns = size ? clustersFor(*size, clusterSize) : list.vcnCount();
    std::uint64_t remaining = size.value_or(std::numeric_limits<std::uint64_t>::max());
    std::uint64_t done = 0;  // virtual clusters accounted for
    bool afterHole = false;
    for (const DataRun& run : list.runs) {
        if (done >= neededVcns) {
            break;  // preallocated clusters beyond the size
        }
        std::uint64_t take = std::min(run.length, neededVcns - done);
        if (!run.lcn) {
            addIssue(issues, AllocationIssue::SparseRuns);
            afterHole = true;
            const std::uint64_t holeBytes = take > remaining / clusterSize ? remaining : take * clusterSize;
            remaining -= std::min(remaining, holeBytes);
            done += take;
            continue;
        }
        const std::uint64_t lcn = *run.lcn;
        if (!boot_.isValidCluster(lcn)) {
            addIssue(issues, AllocationIssue::InvalidClusterInChain);
            break;
        }
        const std::uint64_t available = boot_.clusterCount - lcn;
        const bool clipped = take > available;
        if (clipped) {
            addIssue(issues, AllocationIssue::BeyondVolume);
            take = available;
        }
        // take <= available, so the product stays below the volume size.
        const std::uint64_t offset = boot_.clusterOffset(lcn);
        const std::uint64_t length = std::min(take * clusterSize, remaining);
        if (length > 0) {
            Extent* last = allocation.extents.empty() ? nullptr : &allocation.extents.back();
            if (last != nullptr && !afterHole && last->offset + last->length == offset) {
                last->length += length;
            } else {
                allocation.extents.push_back(Extent{offset, length});
            }
        }
        afterHole = false;
        remaining -= length;
        allocation.clusterCount += take;
        checkRun(lcn, take, deleted, issues);
        done += take;
        if (clipped) {
            break;
        }
    }
    if (done < neededVcns) {
        addIssue(issues, AllocationIssue::ChainShorterThanSize);
    }
    if (neededVcns == 0) {
        allocation.method = AllocationMethod::None;  // nothing to locate
    }
    return allocation;
}

NtfsFilesystem::Described NtfsFilesystem::describe(const MftRecord& record) {
    Described described;
    DirectoryEntry& entry = described.entry;
    FileAllocation& allocation = described.allocation;
    const bool deleted = !record.inUse();
    entry.state = deleted ? EntryState::Deleted : EntryState::Active;
    entry.isDirectory = record.isDirectory();
    entry.metadataOffset = recordOffset(record.number);
    if (record.standardInformation) {
        const StandardInformation& si = *record.standardInformation;
        entry.attributes.readOnly = (si.fileAttributes & kFileReadOnly) != 0;
        entry.attributes.hidden = (si.fileAttributes & kFileHidden) != 0;
        entry.attributes.system = (si.fileAttributes & kFileSystem) != 0;
        entry.attributes.archive = (si.fileAttributes & kFileArchive) != 0;
        entry.created = timestampOf(si.created, entry.issues);
        entry.modified = timestampOf(si.modified, entry.issues);
        entry.accessed = timestampOf(si.accessed, entry.issues);
    } else {
        addIssue(entry.issues, EntryIssue::MetadataIncomplete);
    }
    if (!record.problems.empty()) {
        addIssue(entry.issues, EntryIssue::DamagedRecord);
    }

    const bool hasAttributeList = record.has(kAttrAttributeList);
    if (entry.isDirectory) {
        // Small directories keep their whole index in the record ($INDEX_ROOT) and own no clusters.
        if (const Attribute* index = record.find(kAttrIndexAllocation, u"$I30");
            index != nullptr && index->nonResident) {
            allocation = runAllocation(*index, std::nullopt, deleted);
        }
        return described;
    }
    const Attribute* data = record.find(kAttrData);
    if (data == nullptr) {
        if (hasAttributeList) {
            addIssue(allocation.issues, AllocationIssue::AttributeListNotFollowed);
        } else if (!record.isViewIndex()) {
            addIssue(allocation.issues, AllocationIssue::DataAttributeMissing);
        }
        return described;
    }
    if (!data->nonResident) {
        entry.size = data->value.size();
        entry.validDataLength = entry.size;
        if (!data->value.empty()) {  // an empty file has nothing to locate
            allocation.method = AllocationMethod::Resident;
            allocation.residentData = data->value;
        }
        return described;
    }
    entry.size = data->realSize;
    entry.validDataLength = data->initializedSize;
    if (data->initializedSize > data->realSize) {
        addIssue(entry.issues, EntryIssue::ValidDataLengthExceedsSize);
    }
    allocation = runAllocation(*data, data->realSize, deleted);
    if (hasAttributeList && allocation.hasIssue(AllocationIssue::ChainShorterThanSize)) {
        addIssue(allocation.issues, AllocationIssue::AttributeListNotFollowed);
    }
    const auto stored = std::find_if(data->runs.runs.begin(), data->runs.runs.end(),
                                     [](const DataRun& run) { return run.lcn.has_value(); });
    if (stored != data->runs.runs.end() && boot_.isValidCluster(*stored->lcn)) {
        entry.firstCluster = ClusterNumber{*stored->lcn};
    }
    entry.contiguousData = allocation.method == AllocationMethod::RunList && allocation.extents.size() == 1 &&
                           !allocation.hasIssue(AllocationIssue::SparseRuns) &&
                           !allocation.hasIssue(AllocationIssue::ChainShorterThanSize);
    return described;
}

void NtfsFilesystem::claimRuns(const MftRecord& record, std::vector<Run>& claims) const {
    for (const Attribute& attribute : record.attributes) {
        if (!attribute.nonResident) {
            continue;
        }
        for (const DataRun& run : attribute.runs.runs) {
            if (run.lcn && boot_.isValidCluster(*run.lcn)) {
                claims.push_back(Run{*run.lcn, std::min(run.length, boot_.clusterCount - *run.lcn)});
            }
        }
    }
}

Result<const NtfsFilesystem::Catalog*> NtfsFilesystem::catalog(const CancellationToken& cancel) {
    if (catalog_ == nullptr) {
        Result<std::unique_ptr<Catalog>> built = buildCatalog(cancel);
        if (!built.ok()) {
            return built.error();
        }
        catalog_ = std::move(built).value();
    }
    return static_cast<const Catalog*>(catalog_.get());
}

Result<std::unique_ptr<NtfsFilesystem::Catalog>> NtfsFilesystem::buildCatalog(const CancellationToken& cancel) {
    if (cancel.isCancellationRequested()) {
        return makeError(ErrorCode::Cancelled, "NTFS scan cancelled");
    }
    auto catalog = std::make_unique<Catalog>();
    std::vector<Run> claims;
    std::size_t invalidRecords = 0;
    std::uint64_t unreadableRecords = 0;
    std::uint64_t firstUnreadable = 0;
    const auto reportInvalid = [&](std::uint64_t number, const std::string& detail) {
        if (invalidRecords++ < kMaxRecordIssues) {
            catalog->issues.push_back(
                ScanIssue{ScanIssueKind::RecordInvalid, {}, std::format("MFT record {}: {}", number, detail)});
        }
    };

    // Sequentially, one cache page at a time.
    std::vector<std::byte> buffer(boot_.recordSize);
    const std::uint64_t perPage = exfat::PagedRegion::kPageBytes / boot_.recordSize;
    for (std::uint64_t first = 0; first < reachableRecords_; first += perPage) {
        if (cancel.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "NTFS scan cancelled");
        }
        const std::uint64_t end = std::min(first + perPage, reachableRecords_);
        const exfat::PagedRegion::Page& page = mft_->page(first * boot_.recordSize / exfat::PagedRegion::kPageBytes);
        if (page.noneReadable) {
            if (unreadableRecords == 0) {
                firstUnreadable = first;
            }
            unreadableRecords += end - first;
            continue;
        }
        for (std::uint64_t number = first; number < end; ++number) {
            const auto within = static_cast<std::size_t>((number - first) * boot_.recordSize);
            if (!copyRecord(page, within, boot_.bytesPerSector, buffer)) {
                if (unreadableRecords++ == 0) {
                    firstUnreadable = number;
                }
                continue;
            }
            if (isBlankRecord(buffer)) {
                continue;
            }
            Result<MftRecord> parsed = parseMftRecord(buffer, number);
            if (!parsed.ok()) {
                reportInvalid(number, parsed.error().message);
                continue;
            }
            const MftRecord& record = parsed.value();
            if (record.inUse()) {
                claimRuns(record, claims);  // extension records own clusters too
            }
            if (!record.isBaseRecord() || record.names.empty()) {
                continue;  // extension, reserved or never-named records
            }
            for (const std::string& problem : record.problems) {
                reportInvalid(number, problem);
            }
            Described described = describe(record);
            const auto index = static_cast<std::uint32_t>(catalog->records.size());
            catalog->byNumber.emplace(number, index);
            catalog->records.push_back(
                Catalog::Record{number, record.sequence, std::move(described.entry), std::move(described.allocation)});
            catalog->addPlacements(index, record);
        }
    }
    if (reachableRecords_ < recordCount_) {
        // Stored beyond the end of the source: counted, never read.
        if (unreadableRecords == 0) {
            firstUnreadable = reachableRecords_;
        }
        unreadableRecords += recordCount_ - reachableRecords_;
    }
    if (invalidRecords > kMaxRecordIssues) {
        catalog->issues.push_back(ScanIssue{ScanIssueKind::RecordInvalid, {},
                                            std::format("{} more invalid MFT records not listed",
                                                        invalidRecords - kMaxRecordIssues)});
    }
    if (unreadableRecords > 0) {
        catalog->issues.push_back(ScanIssue{ScanIssueKind::RecordUnreadable, {},
                                            std::format("{} unreadable MFT record(s), the first is record {}",
                                                        unreadableRecords, firstUnreadable)});
    }

    // Cross-links: clusters claimed twice by in-use records (any attribute).
    // Runs are merged after sorting, so memory stays proportional to the
    // number of runs, not to the volume size.
    std::sort(claims.begin(), claims.end(), [](const Run& a, const Run& b) { return a.first < b.first; });
    std::vector<std::pair<std::uint64_t, std::uint64_t>> conflicts;  // [first, end), merged
    std::uint64_t unionStart = 0;
    std::uint64_t unionEnd = 0;
    for (const Run& run : claims) {
        if (run.count == 0) {
            continue;
        }
        const std::uint64_t end = run.first + run.count;
        if (run.first >= unionEnd) {
            catalog->referencedClusters += unionEnd - unionStart;
            unionStart = run.first;
            unionEnd = end;
            continue;
        }
        const std::uint64_t overlapEnd = std::min(end, unionEnd);
        if (!conflicts.empty() && run.first <= conflicts.back().second) {
            conflicts.back().second = std::max(conflicts.back().second, overlapEnd);
        } else {
            conflicts.emplace_back(run.first, overlapEnd);
        }
        unionEnd = std::max(unionEnd, end);
    }
    catalog->referencedClusters += unionEnd - unionStart;
    for (const auto& [start, end] : conflicts) {
        catalog->crossLinkedClusters += end - start;
    }
    if (!conflicts.empty()) {
        std::vector<std::uint64_t> conflictEnds;
        for (const auto& conflict : conflicts) {
            conflictEnds.push_back(boot_.clusterOffset(conflict.second - 1) + boot_.clusterSize);
        }
        for (Catalog::Record& record : catalog->records) {
            if (!record.active() || record.allocation.method != AllocationMethod::RunList) {
                continue;
            }
            for (const Extent& extent : record.allocation.extents) {
                const auto hit = std::upper_bound(conflictEnds.begin(), conflictEnds.end(), extent.offset);
                if (hit != conflictEnds.end() &&
                    boot_.clusterOffset(conflicts[static_cast<std::size_t>(hit - conflictEnds.begin())].first) <
                        extent.offset + extent.length) {
                    addIssue(record.allocation.issues, AllocationIssue::CrossLinked);
                    break;
                }
            }
        }
    }

    for (std::uint32_t i = 0; i < catalog->placements.size(); ++i) {
        catalog->children[catalog->placements[i].parent.record].push_back(i);
    }
    log(LogLevel::Info, "MFT read",
        {field("records", recordCount_), field("named_records", catalog->records.size()),
         field("invalid_records", invalidRecords), field("unreadable_records", unreadableRecords)});
    return catalog;
}

Result<DirectoryListing> NtfsFilesystem::readDirectory(const DirectoryEntry& directory) {
    if (!directory.isDirectory) {
        return makeError(ErrorCode::InvalidInput, "not a directory");
    }
    const std::optional<std::uint64_t> number = recordAt(directory.metadataOffset);
    if (!number) {
        return makeError(ErrorCode::InvalidInput, "the entry's metadata offset is not an MFT record of this volume");
    }
    Result<const Catalog*> built = catalog({});
    if (!built.ok()) {
        return built.error();
    }
    const Catalog& c = *built.value();
    const auto found = c.byNumber.find(*number);
    if (found == c.byNumber.end()) {
        return makeError(ErrorCode::CorruptedFilesystem,
                         std::format("directory record {} is unreadable, invalid or unnamed", *number));
    }
    const Catalog::Record& record = c.records[found->second];
    if (!record.entry.isDirectory) {
        return makeError(ErrorCode::InvalidFormat, std::format("record {} does not hold a directory", *number));
    }

    DirectoryListing listing;
    if (const auto kids = c.children.find(*number); kids != c.children.end()) {
        for (const std::uint32_t p : kids->second) {
            const Catalog::Placement& placement = c.placements[p];
            if (placement.record == found->second || !Catalog::holds(record, placement)) {
                continue;
            }
            if (listing.entries.size() >= options_.maxListingEntries) {
                listing.issues.push_back(ScanIssue{ScanIssueKind::EntryLimit, {},
                                                   std::format("stopped after {} entries", listing.entries.size())});
                break;
            }
            listing.entries.push_back(c.entryFor(placement));
        }
    }
    return listing;
}

Result<FileAllocation> NtfsFilesystem::resolveAllocation(const DirectoryEntry& entry) {
    const std::optional<std::uint64_t> number = recordAt(entry.metadataOffset);
    if (!number) {
        return makeError(ErrorCode::InvalidInput, "the entry's metadata offset is not an MFT record of this volume");
    }
    Result<MftRecord> record = readRecord(*number);
    if (!record.ok()) {
        return record.error();
    }
    if (!record->isBaseRecord()) {
        return makeError(ErrorCode::InvalidFormat, std::format("record {} is an extension record", *number));
    }
    return describe(record.value()).allocation;
}

Result<FileScan> NtfsFilesystem::scan(const ScanLimits& limits, const CancellationToken& cancel) {
    struct Work {
        std::uint32_t directory = 0;  // catalog record
        std::string path;
        std::uint32_t depth = 0;
        bool deleted = false;  // the directory or one of its ancestors is deleted
    };

    Result<const Catalog*> built = catalog(cancel);
    if (!built.ok()) {
        return built.error();
    }
    const Catalog& c = *built.value();
    FileScan result;
    result.issues = c.issues;
    result.referencedClusters = c.referencedClusters;
    result.crossLinkedClusters = c.crossLinkedClusters;

    std::unordered_set<std::uint64_t> visited;  // directory records already queued
    std::uint64_t entryCount = 0;
    bool stopped = false;

    // Adds one entry; queues it when it is a directory to descend into.
    const auto emit = [&](const Catalog::Placement& placement, std::string path, bool parentDeleted, bool orphan,
                          std::uint32_t depth, std::vector<Work>& subdirectories) {
        const Catalog::Record& record = c.records[placement.record];
        FileRecord out;
        out.entry = c.entryFor(placement);
        if (orphan) {
            addIssue(out.entry.issues, EntryIssue::ParentMissing);
        }
        out.path = std::move(path);
        out.parentDeleted = parentDeleted;
        out.allocation = record.allocation;
        if (record.entry.isDirectory && (record.active() || limits.recurseIntoDeletedDirectories)) {
            if (visited.contains(record.number)) {
                if (record.active()) {
                    result.issues.push_back(ScanIssue{ScanIssueKind::DirectoryLoop, out.path,
                                                      "directory is reachable through more than one name"});
                }
            } else if (depth + 1 > limits.maxDepth) {
                result.issues.push_back(
                    ScanIssue{ScanIssueKind::DepthLimit, out.path, "maximum depth " + std::to_string(limits.maxDepth)});
                result.complete = false;
            } else {
                visited.insert(record.number);
                subdirectories.push_back(Work{placement.record, out.path, depth + 1, parentDeleted || !record.active()});
            }
        }
        result.records.push_back(std::move(out));
    };
    const auto admit = [&](const Catalog::Record& record, const std::string& where) {
        if (!record.active() && !limits.includeDeleted) {
            return false;
        }
        if (++entryCount > limits.maxEntries) {
            result.issues.push_back(ScanIssue{ScanIssueKind::EntryLimit, where,
                                              "stopped after " + std::to_string(limits.maxEntries) + " entries"});
            result.complete = false;
            stopped = true;
            return false;
        }
        return true;
    };
    // Depth-first, visiting subdirectories in MFT order.
    std::vector<Work> stack;
    const auto drain = [&]() -> Status {
        while (!stack.empty() && !stopped) {
            if (cancel.isCancellationRequested()) {
                return makeError(ErrorCode::Cancelled, "NTFS scan cancelled");
            }
            Work work = std::move(stack.back());
            stack.pop_back();
            const Catalog::Record& directory = c.records[work.directory];
            const std::string shownPath = work.path.empty() ? "/" : work.path;
            std::vector<Work> subdirectories;
            if (const auto kids = c.children.find(directory.number); kids != c.children.end()) {
                for (const std::uint32_t p : kids->second) {
                    const Catalog::Placement& placement = c.placements[p];
                    if (placement.record == work.directory || !Catalog::holds(directory, placement)) {
                        continue;
                    }
                    if (!admit(c.records[placement.record], shownPath)) {
                        if (stopped) {
                            break;
                        }
                        continue;
                    }
                    emit(placement, work.path + "/" + placement.name, work.deleted, false, work.depth, subdirectories);
                }
            }
            stack.insert(stack.end(), std::make_move_iterator(subdirectories.rbegin()),
                         std::make_move_iterator(subdirectories.rend()));
        }
        return success();
    };

    const auto root = c.byNumber.find(kRootRecord);
    if (root != c.byNumber.end() && c.records[root->second].entry.isDirectory) {
        visited.insert(kRootRecord);
        stack.push_back(Work{root->second, "", 0, !c.records[root->second].active()});
        if (Status drained = drain(); !drained.ok()) {
            return drained.error();
        }
    } else {
        result.issues.push_back(ScanIssue{ScanIssueKind::DirectoryInvalid, "/",
                                          "root directory record is missing, damaged or not a directory"});
    }

    // Entries no directory holds any more, each with its subtree.
    const std::string orphanPath = "/" + std::string(kOrphanDirectory);
    for (const std::uint32_t index : c.orphanRoots()) {
        if (stopped) {
            break;
        }
        if (cancel.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "NTFS scan cancelled");
        }
        const Catalog::Record& record = c.records[index];
        if (!admit(record, orphanPath)) {
            continue;
        }
        const Catalog::Placement& placement = c.placements[record.firstPlacement];
        std::vector<Work> subdirectories;
        emit(placement, orphanPath + "/" + placement.name, false, true, 0, subdirectories);
        stack.assign(std::make_move_iterator(subdirectories.begin()), std::make_move_iterator(subdirectories.end()));
        if (Status drained = drain(); !drained.ok()) {
            return drained.error();
        }
    }

    log(LogLevel::Info, "NTFS scan finished",
        {field("records", result.records.size()), field("issues", result.issues.size()),
         field("cross_linked_clusters", result.crossLinkedClusters), field("complete", result.complete ? 1 : 0)});
    return result;
}

Result<ClusterUsage> NtfsFilesystem::analyzeClusters(const CancellationToken& cancel) {
    if (cancel.isCancellationRequested()) {
        return makeError(ErrorCode::Cancelled, "NTFS cluster analysis cancelled");
    }
    ClusterUsage usage;
    usage.total = boot_.clusterCount;
    if (bitmap_ != nullptr) {
        // Clusters whose bits lie beyond the end of the source are counted at
        // once, so a boot sector claiming a huge volume costs nothing.
        const std::uint64_t covered = bitmap_->coveredClusters();
        for (std::uint64_t first = 0; first < covered;) {
            if (cancel.isCancellationRequested()) {
                return makeError(ErrorCode::Cancelled, "NTFS cluster analysis cancelled");
            }
            const std::uint64_t count = std::min(kAnalysisSlice, covered - first);
            const BitCounts counts = bitmap_->count(first, count);
            usage.free += counts.free;
            usage.allocated += counts.allocated;
            usage.unreadable += counts.unreadable;
            first += count;
        }
        usage.unreadable += boot_.clusterCount - covered;
    } else {
        usage.unreadable = usage.total;
    }
    // Bad clusters are listed in $BadClus (and normally allocated in $Bitmap).
    for (const Run& run : badRuns_) {
        usage.bad += run.count;
        if (bitmap_ == nullptr) {
            usage.unreadable -= run.count;
            continue;
        }
        const BitCounts counts = bitmap_->count(run.first, run.count);
        usage.free -= counts.free;
        usage.allocated -= counts.allocated;
        usage.unreadable -= counts.unreadable;
    }
    return usage;  // NTFS records no free-cluster count of its own
}

Result<ClusterState> NtfsFilesystem::clusterState(ClusterNumber cluster) {
    if (!boot_.isValidCluster(cluster.value())) {
        return makeError(ErrorCode::InvalidInput, "cluster " + std::to_string(cluster.value()) + " is out of range");
    }
    const auto bad = std::upper_bound(badRuns_.begin(), badRuns_.end(), cluster.value(),
                                      [](std::uint64_t value, const Run& run) { return value < run.first; });
    if (bad != badRuns_.begin() && cluster.value() - std::prev(bad)->first < std::prev(bad)->count) {
        return ClusterState::Bad;
    }
    if (bitmap_ == nullptr) {
        return ClusterState::Unreadable;
    }
    switch (bitmap_->state(cluster.value())) {
    case BitState::Free:
        return ClusterState::Free;
    case BitState::Allocated:
        return ClusterState::Allocated;
    case BitState::Unreadable:
        return ClusterState::Unreadable;
    }
    return ClusterState::Unreadable;
}

void NtfsFilesystem::log(LogLevel level, std::string_view message,
                         std::initializer_list<diagnostics::LogField> fields) const {
    if (logger_ != nullptr) {
        logger_->log(level, kComponent, message, fields);
    }
}

}  // namespace recovery::filesystem::ntfs
