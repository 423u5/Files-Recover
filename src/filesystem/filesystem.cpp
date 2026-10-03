#include "filesystem/filesystem.hpp"

#include <algorithm>

namespace recovery::filesystem {

std::string_view toString(FilesystemType type) noexcept {
    switch (type) {
    case FilesystemType::Fat32:
        return "FAT32";
    case FilesystemType::ExFat:
        return "exFAT";
    case FilesystemType::Ntfs:
        return "NTFS";
    }
    return "Unknown";
}

std::string_view toString(AllocationMethod method) noexcept {
    switch (method) {
    case AllocationMethod::None:
        return "None";
    case AllocationMethod::ClusterChain:
        return "ClusterChain";
    case AllocationMethod::ContiguousGuess:
        return "ContiguousGuess";
    case AllocationMethod::Contiguous:
        return "Contiguous";
    case AllocationMethod::RunList:
        return "RunList";
    case AllocationMethod::Resident:
        return "Resident";
    }
    return "Unknown";
}

std::string_view toString(AllocationIssue issue) noexcept {
    switch (issue) {
    case AllocationIssue::InvalidStartCluster:
        return "InvalidStartCluster";
    case AllocationIssue::InvalidClusterInChain:
        return "InvalidClusterInChain";
    case AllocationIssue::FreeClusterInChain:
        return "FreeClusterInChain";
    case AllocationIssue::BadClusterInChain:
        return "BadClusterInChain";
    case AllocationIssue::ChainLoop:
        return "ChainLoop";
    case AllocationIssue::CrossLinked:
        return "CrossLinked";
    case AllocationIssue::ChainShorterThanSize:
        return "ChainShorterThanSize";
    case AllocationIssue::ChainLongerThanSize:
        return "ChainLongerThanSize";
    case AllocationIssue::UnreadableAllocationTable:
        return "UnreadableAllocationTable";
    case AllocationIssue::ClustersInUse:
        return "ClustersInUse";
    case AllocationIssue::BeyondVolume:
        return "BeyondVolume";
    case AllocationIssue::SizeExceedsVolume:
        return "SizeExceedsVolume";
    case AllocationIssue::ClustersMarkedFree:
        return "ClustersMarkedFree";
    case AllocationIssue::InvalidRunList:
        return "InvalidRunList";
    case AllocationIssue::SparseRuns:
        return "SparseRuns";
    case AllocationIssue::CompressedData:
        return "CompressedData";
    case AllocationIssue::EncryptedData:
        return "EncryptedData";
    case AllocationIssue::AttributeListNotFollowed:
        return "AttributeListNotFollowed";
    case AllocationIssue::DataAttributeMissing:
        return "DataAttributeMissing";
    }
    return "Unknown";
}

std::string_view toString(EntryIssue issue) noexcept {
    switch (issue) {
    case EntryIssue::InvalidShortName:
        return "InvalidShortName";
    case EntryIssue::LongNameChecksumMismatch:
        return "LongNameChecksumMismatch";
    case EntryIssue::LongNameUnverified:
        return "LongNameUnverified";
    case EntryIssue::InvalidTimestamp:
        return "InvalidTimestamp";
    case EntryIssue::ReservedAttributeBits:
        return "ReservedAttributeBits";
    case EntryIssue::DirectoryWithSize:
        return "DirectoryWithSize";
    case EntryIssue::NameReconstructed:
        return "NameReconstructed";
    case EntryIssue::EntrySetChecksumMismatch:
        return "EntrySetChecksumMismatch";
    case EntryIssue::NameHashMismatch:
        return "NameHashMismatch";
    case EntryIssue::MetadataIncomplete:
        return "MetadataIncomplete";
    case EntryIssue::InvalidName:
        return "InvalidName";
    case EntryIssue::ValidDataLengthExceedsSize:
        return "ValidDataLengthExceedsSize";
    case EntryIssue::ParentMissing:
        return "ParentMissing";
    case EntryIssue::DamagedRecord:
        return "DamagedRecord";
    }
    return "Unknown";
}

std::string_view toString(ScanIssueKind kind) noexcept {
    switch (kind) {
    case ScanIssueKind::DirectoryLoop:
        return "DirectoryLoop";
    case ScanIssueKind::DirectoryUnreadable:
        return "DirectoryUnreadable";
    case ScanIssueKind::DirectoryInvalid:
        return "DirectoryInvalid";
    case ScanIssueKind::DepthLimit:
        return "DepthLimit";
    case ScanIssueKind::EntryLimit:
        return "EntryLimit";
    case ScanIssueKind::RecordInvalid:
        return "RecordInvalid";
    case ScanIssueKind::RecordUnreadable:
        return "RecordUnreadable";
    }
    return "Unknown";
}

std::uint64_t FileAllocation::dataBytes() const noexcept {
    std::uint64_t total = residentData.size();
    for (const Extent& extent : extents) {
        total += extent.length;
    }
    return total;
}

bool FileAllocation::hasIssue(AllocationIssue issue) const noexcept {
    return std::find(issues.begin(), issues.end(), issue) != issues.end();
}

bool DirectoryEntry::hasIssue(EntryIssue issue) const noexcept {
    return std::find(issues.begin(), issues.end(), issue) != issues.end();
}

}  // namespace recovery::filesystem
