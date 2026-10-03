#include "recovery/filesystem_recovery.hpp"

#include "partition/partition_table.hpp"
#include "recovery/checked_math.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

namespace recovery {

namespace {

using diagnostics::field;
using diagnostics::LogLevel;
using filesystem::AllocationIssue;
using filesystem::AllocationMethod;
using filesystem::EntryIssue;
using filesystem::FileRecord;

constexpr std::string_view kComponent = "fsrecovery";

void addWarning(std::vector<CandidateWarning>& warnings, CandidateWarning warning) {
    if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end()) {
        warnings.push_back(warning);
    }
}

std::string extensionOf(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) {
        return {};
    }
    std::string extension(name.substr(dot + 1));
    for (char& c : extension) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return extension;
}

// Appends a region, merging it into the previous one when it continues it.
void appendRegion(std::vector<SourceRegion>& regions, const SourceRegion& region) {
    if (region.length == 0) {
        return;
    }
    if (!regions.empty()) {
        SourceRegion& last = regions.back();
        const bool sameKind = last.kind == region.kind && last.reallocated == region.reallocated;
        const bool continues = region.kind == RegionKind::Zeros || region.kind == RegionKind::Missing ||
                               last.sourceOffset + last.length == region.sourceOffset;
        if (sameKind && continues) {
            last.length += region.length;
            return;
        }
    }
    regions.push_back(region);
}

// Bytes at and beyond the valid data length hold stale data on disk and read as zeros.
std::vector<SourceRegion> applyValidDataLength(const std::vector<SourceRegion>& regions, std::uint64_t validLength) {
    std::vector<SourceRegion> out;
    for (const SourceRegion& region : regions) {
        const bool holdsData = region.kind == RegionKind::Stored || region.kind == RegionKind::Embedded;
        const std::uint64_t end = region.fileOffset + region.length;
        if (!holdsData || end <= validLength) {
            appendRegion(out, region);
            continue;
        }
        if (region.fileOffset < validLength) {
            SourceRegion valid = region;
            valid.length = validLength - region.fileOffset;
            appendRegion(out, valid);
        }
        const std::uint64_t zeroStart = std::max(region.fileOffset, validLength);
        appendRegion(out, SourceRegion{zeroStart, end - zeroStart, RegionKind::Zeros, 0, false});
    }
    return out;
}

bool isDamage(AllocationIssue issue) noexcept {
    switch (issue) {
    case AllocationIssue::InvalidStartCluster:
    case AllocationIssue::InvalidClusterInChain:
    case AllocationIssue::FreeClusterInChain:
    case AllocationIssue::BadClusterInChain:
    case AllocationIssue::ChainLoop:
    case AllocationIssue::ChainLongerThanSize:
    case AllocationIssue::BeyondVolume:
    case AllocationIssue::SizeExceedsVolume:
    case AllocationIssue::ClustersMarkedFree:
    case AllocationIssue::InvalidRunList:
    case AllocationIssue::DataAttributeMissing:
        return true;
    default:
        // ChainShorterThanSize and AttributeListNotFollowed show as missing
        // data; the others have warnings of their own.
        return false;
    }
}

std::vector<CandidateWarning> warningsFor(const RecoveryCandidate& candidate) {
    std::vector<CandidateWarning> warnings;
    const FilesystemEvidence& evidence = candidate.filesystemEvidence;
    const AllocationInfo& allocation = evidence.allocation;

    if (allocation.layout == LayoutEvidence::Guessed) {
        addWarning(warnings, CandidateWarning::LayoutGuessed);
    }
    if (candidate.bytes(RegionKind::Missing) > 0) {
        addWarning(warnings, CandidateWarning::DataMissing);
    }
    if (candidate.reallocatedBytes() > 0) {
        addWarning(warnings, CandidateWarning::ClustersReallocated);
    }
    for (const AllocationIssue issue : allocation.issues) {
        if (issue == AllocationIssue::CompressedData || issue == AllocationIssue::EncryptedData) {
            addWarning(warnings, CandidateWarning::DataNotDecoded);
        } else if (issue == AllocationIssue::CrossLinked) {
            addWarning(warnings, CandidateWarning::CrossLinked);
        } else if (issue == AllocationIssue::UnreadableAllocationTable) {
            addWarning(warnings, CandidateWarning::AllocationUnknown);
        } else if (isDamage(issue)) {
            addWarning(warnings, CandidateWarning::AllocationDamaged);
        }
    }
    for (const EntryIssue issue : evidence.entryIssues) {
        switch (issue) {
        case EntryIssue::InvalidShortName:
        case EntryIssue::LongNameChecksumMismatch:
        case EntryIssue::LongNameUnverified:
        case EntryIssue::NameReconstructed:
        case EntryIssue::NameHashMismatch:
        case EntryIssue::InvalidName:
            addWarning(warnings, CandidateWarning::NameUncertain);
            break;
        case EntryIssue::ParentMissing:
            addWarning(warnings, CandidateWarning::LocationUnknown);
            break;
        case EntryIssue::InvalidTimestamp:
        case EntryIssue::ReservedAttributeBits:
        case EntryIssue::DirectoryWithSize:
        case EntryIssue::EntrySetChecksumMismatch:
        case EntryIssue::MetadataIncomplete:
        case EntryIssue::ValidDataLengthExceedsSize:
        case EntryIssue::DamagedRecord:
            addWarning(warnings, CandidateWarning::MetadataDamaged);
            break;
        }
    }
    return warnings;
}

}  // namespace

FilesystemRecovery::FilesystemRecovery(std::uint64_t volumeOffset, diagnostics::Logger* logger) noexcept
    : volumeOffset_(volumeOffset), logger_(logger) {}

bool FilesystemRecovery::isUserFile(const FileRecord& /*record*/) {
    return true;
}

std::optional<std::vector<FilesystemRecovery::LayoutPiece>> FilesystemRecovery::customLayout(
    const FileRecord& /*record*/) {
    return std::nullopt;
}

Result<CandidateScan> FilesystemRecovery::findCandidates(const CandidateOptions& options,
                                                        const CancellationToken& cancel) {
    filesystem::ScanLimits limits = options.limits;
    if (!options.includeDeleted) {
        limits.includeDeleted = false;
        limits.recurseIntoDeletedDirectories = false;
    }
    Result<filesystem::FileScan> scan = filesystem().scan(limits, cancel);
    if (!scan.ok()) {
        return scan.error();
    }

    CandidateScan result;
    result.filesystemInfo = filesystem().info();
    result.volumeOffset = volumeOffset_;
    result.issues = std::move(scan->issues);
    result.complete = scan->complete;
    std::uint64_t nextId = options.firstId;
    for (const FileRecord& record : scan->records) {
        if (cancel.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "finding recovery candidates was cancelled");
        }
        if (record.entry.isDirectory) {
            ++result.directories;
            continue;
        }
        if (!isUserFile(record)) {
            ++result.systemFiles;
            continue;
        }
        const bool deleted = record.entry.state == filesystem::EntryState::Deleted || record.parentDeleted;
        if (deleted ? !options.includeDeleted : !options.includeActive) {
            continue;
        }
        result.candidates.push_back(buildCandidate(record, CandidateId{nextId++}));
    }

    if (logger_ != nullptr) {
        const auto deletedCount = static_cast<std::uint64_t>(
            std::count_if(result.candidates.begin(), result.candidates.end(),
                          [](const RecoveryCandidate& candidate) { return candidate.isDeleted(); }));
        logger_->log(LogLevel::Info, kComponent, "recovery candidates found",
                     {field("filesystem", filesystem::toString(result.filesystemInfo.type)),
                      field("volume_offset", volumeOffset_), field("candidates", result.candidates.size()),
                      field("deleted", deletedCount), field("directories", result.directories),
                      field("system_files", result.systemFiles), field("scan_issues", result.issues.size()),
                      field("complete", result.complete ? "yes" : "no")});
    }
    return result;
}

RecoveryCandidate FilesystemRecovery::buildCandidate(const FileRecord& record, CandidateId id) {
    const filesystem::DirectoryEntry& entry = record.entry;
    const filesystem::FileAllocation& allocation = record.allocation;
    const filesystem::FilesystemInfo& info = filesystem().info();
    const std::uint64_t size = entry.size;

    RecoveryCandidate candidate;
    candidate.id = id;
    candidate.method = RecoveryMethod::Filesystem;
    candidate.filename = entry.name;
    candidate.extension = extensionOf(entry.name);
    candidate.expectedSize = size;

    FilesystemEvidence& evidence = candidate.filesystemEvidence;
    evidence.type = info.type;
    evidence.volumeOffset = volumeOffset_;
    evidence.metadataOffset = checkedAdd(volumeOffset_, entry.metadataOffset).value_or(entry.metadataOffset);
    evidence.path = record.path;
    evidence.shortName = entry.shortName;
    evidence.state = entry.state;
    evidence.parentDeleted = record.parentDeleted;
    evidence.validDataLength = entry.validDataLength;
    evidence.created = entry.created;
    evidence.modified = entry.modified;
    evidence.accessed = entry.accessed;
    evidence.attributes = entry.attributes;
    evidence.entryIssues = entry.issues;
    AllocationInfo& allocationInfo = evidence.allocation;
    allocationInfo.method = allocation.method;
    allocationInfo.firstCluster = entry.firstCluster;
    allocationInfo.clusterCount = allocation.clusterCount;
    allocationInfo.clusterSize = info.clusterSize;
    allocationInfo.issues = allocation.issues;

    // 1. Where the file's bytes are, in volume coordinates and file order.
    std::vector<LayoutPiece> pieces;
    const bool notDecoded = allocation.hasIssue(AllocationIssue::CompressedData) ||
                            allocation.hasIssue(AllocationIssue::EncryptedData);
    if (notDecoded) {
        // The stored bytes are not the file's content, and their file positions are unknown.
    } else if (std::optional<std::vector<LayoutPiece>> custom = customLayout(record)) {
        pieces = std::move(*custom);
    } else if (allocation.hasIssue(AllocationIssue::SparseRuns)) {
        // The extents omit the holes, so where each extent belongs in the file is unknown.
    } else if (allocation.method == AllocationMethod::Resident) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size, allocation.residentData.size()));
        candidate.embeddedData.assign(allocation.residentData.begin(),
                                      allocation.residentData.begin() + static_cast<std::ptrdiff_t>(count));
        pieces.push_back(LayoutPiece{count, RegionKind::Embedded, 0});
    } else {
        std::uint64_t covered = 0;
        for (const filesystem::Extent& extent : allocation.extents) {
            if (covered >= size) {
                break;
            }
            const std::uint64_t length = std::min(extent.length, size - covered);
            if (length > 0) {
                pieces.push_back(LayoutPiece{length, RegionKind::Stored, extent.offset});
                covered += length;
            }
        }
    }

    // 2. Source regions covering exactly [0, size); whatever the pieces do not cover is Missing.
    // A file cannot store more bytes than the cluster area holds; more means
    // runs that repeat clusters (damaged or crafted metadata), and the excess
    // is treated as Missing so a reconstruction stays bounded by the volume.
    const std::uint64_t clusterArea = checkedMul<std::uint64_t>(info.clusterCount, info.clusterSize)
                                          .value_or(std::numeric_limits<std::uint64_t>::max());
    std::vector<SourceRegion> regions;
    std::uint64_t position = 0;
    std::uint64_t storedTotal = 0;
    std::size_t fragments = 0;
    std::optional<std::uint64_t> previousEnd;  // volume offset where the previous stored piece ends
    bool located = false;
    bool storedBeyondVolume = false;
    for (const LayoutPiece& piece : pieces) {
        if (position >= size) {
            break;
        }
        std::uint64_t length = std::min(piece.length, size - position);
        if (length == 0) {
            continue;
        }
        SourceRegion region{position, length, piece.kind, 0, false};
        if (piece.kind == RegionKind::Stored) {
            if (length > clusterArea - storedTotal) {
                storedBeyondVolume = true;
                length = clusterArea - storedTotal;
                region.length = length;
                if (length == 0) {
                    break;
                }
            }
            const std::optional<std::uint64_t> sourceOffset = checkedAdd(volumeOffset_, piece.volumeOffset);
            if (sourceOffset.has_value() && checkedAdd(*sourceOffset, length).has_value()) {
                region.sourceOffset = *sourceOffset;
                if (previousEnd != piece.volumeOffset) {
                    ++fragments;
                }
                previousEnd = piece.volumeOffset + length;
                storedTotal += length;
            } else {
                region.kind = RegionKind::Missing;
                previousEnd.reset();
            }
        } else {
            region.sourceOffset = piece.kind == RegionKind::Embedded ? piece.volumeOffset : 0;
            previousEnd.reset();
        }
        located = located || region.kind != RegionKind::Missing;
        appendRegion(regions, region);
        position += length;
        if (storedBeyondVolume) {
            break;
        }
    }
    if (position < size) {
        appendRegion(regions, SourceRegion{position, size - position, RegionKind::Missing, 0, false});
    }

    // 3. Valid data length (exFAT, NTFS).
    if (entry.validDataLength.has_value() && *entry.validDataLength < size) {
        regions = applyValidDataLength(regions, *entry.validDataLength);
    }
    // A size larger than the volume is untrustworthy: only stored data within
    // the first clusterArea bytes of the file is kept, so a reconstruction
    // never produces more than the volume holds.
    if (allocation.hasIssue(AllocationIssue::SizeExceedsVolume)) {
        std::vector<SourceRegion> bounded;
        for (SourceRegion region : regions) {
            if (region.kind == RegionKind::Zeros || region.fileOffset >= clusterArea) {
                region.kind = RegionKind::Missing;
                region.sourceOffset = 0;
                region.reallocated = false;
            } else if (region.length > clusterArea - region.fileOffset) {
                SourceRegion beyond = region;
                region.length = clusterArea - region.fileOffset;
                beyond.fileOffset = clusterArea;
                beyond.length -= region.length;
                beyond.kind = RegionKind::Missing;
                beyond.sourceOffset = 0;
                appendRegion(bounded, region);
                appendRegion(bounded, beyond);
                continue;
            }
            appendRegion(bounded, region);
        }
        regions = std::move(bounded);
    }
    // 4. Deleted files: which clusters were allocated again since the deletion.
    if (candidate.isDeleted() && allocation.hasIssue(AllocationIssue::ClustersInUse)) {
        regions = splitReallocated(regions);
    }
    candidate.sourceRegions = std::move(regions);

    if (!located) {
        allocationInfo.layout = LayoutEvidence::None;
    } else if (allocation.method == AllocationMethod::ContiguousGuess && allocation.clusterCount > 1) {
        allocationInfo.layout = LayoutEvidence::Guessed;
    } else {
        allocationInfo.layout = LayoutEvidence::Recorded;
    }
    candidate.fragmentation.fragmentCount = fragments;
    candidate.fragmentation.known = allocationInfo.layout != LayoutEvidence::Guessed;
    candidate.warnings = warningsFor(candidate);
    if (storedBeyondVolume) {
        addWarning(candidate.warnings, CandidateWarning::AllocationDamaged);
    }
    return candidate;
}

std::vector<SourceRegion> FilesystemRecovery::splitReallocated(const std::vector<SourceRegion>& regions) {
    filesystem::IFilesystem& fs = filesystem();
    const filesystem::FilesystemInfo& info = fs.info();
    if (info.clusterSize == 0) {
        return regions;
    }
    std::vector<SourceRegion> out;
    for (const SourceRegion& region : regions) {
        // Stored regions were built as volumeOffset_ + a volume offset.
        if (region.kind != RegionKind::Stored || region.sourceOffset - volumeOffset_ < info.dataOffset) {
            appendRegion(out, region);
            continue;
        }
        const std::uint64_t volumeStart = region.sourceOffset - volumeOffset_;
        std::uint64_t done = 0;
        while (done < region.length) {
            const std::uint64_t volumePosition = volumeStart + done;
            const std::uint64_t index = (volumePosition - info.dataOffset) / info.clusterSize;
            const std::uint64_t clusterEnd = info.dataOffset + (index + 1) * info.clusterSize;
            const std::uint64_t take = std::min(region.length - done, clusterEnd - volumePosition);
            bool reallocated = false;
            if (const std::optional<std::uint64_t> number = checkedAdd(info.firstCluster, index)) {
                const Result<filesystem::ClusterState> state = fs.clusterState(filesystem::ClusterNumber{*number});
                // Unreadable means unknown (AllocationUnknown); anything but free is in use.
                reallocated = state.ok() && *state != filesystem::ClusterState::Free &&
                              *state != filesystem::ClusterState::Unreadable;
            }
            appendRegion(out, SourceRegion{region.fileOffset + done, take, RegionKind::Stored,
                                           region.sourceOffset + done, reallocated});
            done += take;
        }
    }
    return out;
}

Fat32Recovery::Fat32Recovery(std::unique_ptr<filesystem::fat32::Fat32Filesystem> fs, std::uint64_t volumeOffset,
                             diagnostics::Logger* logger)
    : FilesystemRecovery(volumeOffset, logger), filesystem_(std::move(fs)) {}

Result<std::unique_ptr<Fat32Recovery>> Fat32Recovery::open(storage::IStorageSource& volume,
                                                           std::uint64_t volumeOffset,
                                                           filesystem::fat32::Fat32Options options,
                                                           diagnostics::Logger* logger) {
    Result<std::unique_ptr<filesystem::fat32::Fat32Filesystem>> fs =
        filesystem::fat32::Fat32Filesystem::open(volume, options, logger);
    if (!fs.ok()) {
        return fs.error();
    }
    return std::make_unique<Fat32Recovery>(std::move(fs).value(), volumeOffset, logger);
}

ExFatRecovery::ExFatRecovery(std::unique_ptr<filesystem::exfat::ExFatFilesystem> fs, std::uint64_t volumeOffset,
                             diagnostics::Logger* logger)
    : FilesystemRecovery(volumeOffset, logger), filesystem_(std::move(fs)) {}

Result<std::unique_ptr<ExFatRecovery>> ExFatRecovery::open(storage::IStorageSource& volume,
                                                           std::uint64_t volumeOffset,
                                                           filesystem::exfat::ExFatOptions options,
                                                           diagnostics::Logger* logger) {
    Result<std::unique_ptr<filesystem::exfat::ExFatFilesystem>> fs =
        filesystem::exfat::ExFatFilesystem::open(volume, options, logger);
    if (!fs.ok()) {
        return fs.error();
    }
    return std::make_unique<ExFatRecovery>(std::move(fs).value(), volumeOffset, logger);
}

Result<std::unique_ptr<FilesystemRecovery>> openFilesystemRecovery(storage::IStorageSource& volume,
                                                                   std::uint64_t volumeOffset,
                                                                   const FilesystemRecoveryOptions& options,
                                                                   diagnostics::Logger* logger) {
    if (!volume.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "volume is not open");
    }

    partition::FilesystemCandidates sniffed;
    const std::uint32_t sectorSize = volume.sectorSize();
    if (storage::isValidSectorSize(sectorSize) && volume.size() >= sectorSize) {
        std::vector<std::byte> sector(sectorSize);
        if (volume.readExact(ByteOffset{0}, sector).ok()) {
            sniffed = partition::sniffVolumeBootRecord(sector);
        }
    }

    enum class Module : std::uint8_t { Ntfs, ExFat, Fat32 };
    std::array<Module, 3> order{Module::Ntfs, Module::ExFat, Module::Fat32};
    const auto matches = [&](Module module) {
        switch (module) {
        case Module::Ntfs:
            return sniffed.ntfs;
        case Module::ExFat:
            return sniffed.exfat;
        case Module::Fat32:
            return sniffed.fat;
        }
        return false;
    };
    std::stable_partition(order.begin(), order.end(), matches);

    const auto tryOpen = [&](Module module) -> Result<std::unique_ptr<FilesystemRecovery>> {
        switch (module) {
        case Module::Ntfs: {
            Result<std::unique_ptr<NtfsRecovery>> opened =
                NtfsRecovery::open(volume, volumeOffset, options.ntfs, logger);
            if (!opened.ok()) {
                return opened.error();
            }
            return std::unique_ptr<FilesystemRecovery>(std::move(opened).value());
        }
        case Module::ExFat: {
            Result<std::unique_ptr<ExFatRecovery>> opened =
                ExFatRecovery::open(volume, volumeOffset, options.exfat, logger);
            if (!opened.ok()) {
                return opened.error();
            }
            return std::unique_ptr<FilesystemRecovery>(std::move(opened).value());
        }
        case Module::Fat32: {
            Result<std::unique_ptr<Fat32Recovery>> opened =
                Fat32Recovery::open(volume, volumeOffset, options.fat32, logger);
            if (!opened.ok()) {
                return opened.error();
            }
            return std::unique_ptr<FilesystemRecovery>(std::move(opened).value());
        }
        }
        return makeError(ErrorCode::InternalError, "unknown filesystem module");
    };

    // The most telling failure wins: a module that recognised the volume but
    // could not use it, then any other error (I/O), then "not recognised".
    std::optional<Error> corrupted;
    std::optional<Error> other;
    for (const Module module : order) {
        Result<std::unique_ptr<FilesystemRecovery>> opened = tryOpen(module);
        if (opened.ok()) {
            return opened;
        }
        const Error& error = opened.error();
        if (error.code == ErrorCode::CorruptedFilesystem) {
            if (!corrupted.has_value()) {
                corrupted = error;
            }
        } else if (error.code != ErrorCode::UnsupportedFilesystem && !other.has_value()) {
            other = error;
        }
    }
    if (corrupted.has_value()) {
        return *corrupted;
    }
    if (other.has_value()) {
        return *other;
    }
    return makeError(ErrorCode::UnsupportedFilesystem, "no FAT32, exFAT or NTFS filesystem found on the volume");
}

}  // namespace recovery
