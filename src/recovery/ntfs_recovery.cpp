// NTFS specifics of filesystem-based recovery: which entries are metadata
// files, and the layout of sparse files, whose holes the
// filesystem-independent allocation does not describe.

#include "recovery/filesystem_recovery.hpp"

#include "recovery/checked_math.hpp"

#include <algorithm>
#include <utility>

namespace recovery {

namespace {

namespace ntfs = filesystem::ntfs;

// Records 0-15 are reserved for the metadata files ($MFT ... $Extend).
constexpr std::uint64_t kSystemRecords = 16;

}  // namespace

NtfsRecovery::NtfsRecovery(std::unique_ptr<ntfs::NtfsFilesystem> fs, std::uint64_t volumeOffset,
                           diagnostics::Logger* logger)
    : FilesystemRecovery(volumeOffset, logger), filesystem_(std::move(fs)) {}

Result<std::unique_ptr<NtfsRecovery>> NtfsRecovery::open(storage::IStorageSource& volume, std::uint64_t volumeOffset,
                                                         ntfs::NtfsOptions options, diagnostics::Logger* logger) {
    Result<std::unique_ptr<ntfs::NtfsFilesystem>> fs = ntfs::NtfsFilesystem::open(volume, options, logger);
    if (!fs.ok()) {
        return fs.error();
    }
    return std::make_unique<NtfsRecovery>(std::move(fs).value(), volumeOffset, logger);
}

bool NtfsRecovery::isUserFile(const filesystem::FileRecord& record) {
    if (!systemRecordOffsets_.has_value()) {
        std::set<std::uint64_t> offsets;
        const std::uint64_t count = std::min(kSystemRecords, filesystem_->recordCount());
        for (std::uint64_t number = 0; number < count; ++number) {
            offsets.insert(filesystem_->recordOffset(number));
        }
        systemRecordOffsets_ = std::move(offsets);
    }
    if (systemRecordOffsets_->contains(record.entry.metadataOffset)) {
        return false;
    }
    // $Extend holds further metadata files ($Quota, $ObjId, $Reparse, $UsnJrnl, ...).
    return !record.path.starts_with("/$Extend/");
}

std::optional<std::vector<FilesystemRecovery::LayoutPiece>> NtfsRecovery::customLayout(
    const filesystem::FileRecord& record) {
    const filesystem::FileAllocation& allocation = record.allocation;
    if (!allocation.hasIssue(filesystem::AllocationIssue::SparseRuns)) {
        return std::nullopt;
    }
    // Sparse data: lay the run list out again, holes included. The record is
    // read once more; if that fails, the base treats the data as Missing.
    const std::optional<std::uint64_t> number = filesystem_->recordNumberAt(record.entry.metadataOffset);
    if (!number.has_value()) {
        return std::nullopt;
    }
    Result<ntfs::MftRecord> parsed = filesystem_->readRecord(*number);
    if (!parsed.ok()) {
        return std::nullopt;
    }
    const ntfs::Attribute* data = parsed->find(ntfs::kAttrData);
    if (data == nullptr || !data->nonResident || data->realSize != record.entry.size) {
        return std::nullopt;
    }

    const ntfs::BootSector& boot = filesystem_->bootSector();
    std::vector<LayoutPiece> pieces;
    std::uint64_t remaining = data->realSize;
    for (const ntfs::DataRun& run : data->runs.runs) {
        if (remaining == 0) {
            break;
        }
        const std::uint64_t runBytes = checkedMul<std::uint64_t>(run.length, boot.clusterSize).value_or(remaining);
        const std::uint64_t length = std::min(runBytes, remaining);
        if (!run.lcn.has_value()) {
            pieces.push_back(LayoutPiece{length, RegionKind::Zeros, 0});
            remaining -= length;
            continue;
        }
        // As in the filesystem's own allocation: stop at an invalid cluster,
        // and clip a run that leaves the volume.
        if (!boot.isValidCluster(*run.lcn)) {
            break;
        }
        const std::uint64_t available = boot.clusterCount - *run.lcn;
        const std::uint64_t clusters = std::min(run.length, available);
        const std::uint64_t stored = std::min(length, clusters * boot.clusterSize);
        pieces.push_back(LayoutPiece{stored, RegionKind::Stored, boot.clusterOffset(*run.lcn)});
        remaining -= stored;
        if (clusters < run.length) {
            break;
        }
    }
    // Whatever the runs do not reach is Missing (added by the caller).
    return pieces;
}

}  // namespace recovery
