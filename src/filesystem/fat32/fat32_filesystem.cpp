#include "filesystem/fat32/fat32_filesystem.hpp"

#include "directory_parser.hpp"
#include "fat_table.hpp"
#include "recovery/byte_order.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace recovery::filesystem::fat32 {

namespace {

using diagnostics::field;
using diagnostics::LogLevel;

constexpr std::string_view kComponent = "fat32";
// FAT directories hold at most 65,536 entries of 32 bytes.
constexpr std::uint64_t kMaxDirectoryBytes = 65536ULL * kDirectoryEntrySize;
constexpr std::uint32_t kAnalysisChunk = 256 * 1024;  // FAT entries per read during analysis

std::uint64_t clustersFor(std::uint64_t bytes, std::uint32_t clusterSize) noexcept {
    return bytes == 0 ? 0 : (bytes - 1) / clusterSize + 1;
}

void addIssue(std::vector<AllocationIssue>& issues, AllocationIssue issue) {
    if (std::find(issues.begin(), issues.end(), issue) == issues.end()) {
        issues.push_back(issue);
    }
}

// One bit per cluster; records which clusters active entries own.
class ClaimMap {
public:
    explicit ClaimMap(std::uint64_t clusters) : bits_((clusters + 63) / 64, 0) {}

    // Returns false if the cluster was already claimed.
    bool claim(std::uint32_t cluster) {
        std::uint64_t& word = bits_[cluster / 64];
        const std::uint64_t mask = 1ULL << (cluster % 64);
        if ((word & mask) != 0) {
            return false;
        }
        word |= mask;
        ++count_;
        return true;
    }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

private:
    std::vector<std::uint64_t> bits_;
    std::uint64_t count_ = 0;
};

ClusterState toClusterState(FatEntryKind kind) noexcept {
    switch (kind) {
    case FatEntryKind::Free:
        return ClusterState::Free;
    case FatEntryKind::Next:
    case FatEntryKind::EndOfChain:
        return ClusterState::Allocated;
    case FatEntryKind::Bad:
        return ClusterState::Bad;
    case FatEntryKind::Reserved:
    case FatEntryKind::OutOfRange:
        return ClusterState::Invalid;
    case FatEntryKind::Unreadable:
        return ClusterState::Unreadable;
    }
    return ClusterState::Invalid;
}

std::optional<AllocationIssue> issueForTerminator(FatEntryKind kind) noexcept {
    switch (kind) {
    case FatEntryKind::Free:
        return AllocationIssue::FreeClusterInChain;
    case FatEntryKind::Bad:
        return AllocationIssue::BadClusterInChain;
    case FatEntryKind::Reserved:
    case FatEntryKind::OutOfRange:
        return AllocationIssue::InvalidClusterInChain;
    case FatEntryKind::Unreadable:
        return AllocationIssue::UnreadableAllocationTable;
    case FatEntryKind::Next:
    case FatEntryKind::EndOfChain:
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

Fat32Filesystem::~Fat32Filesystem() = default;

Fat32Filesystem::Fat32Filesystem(storage::IStorageSource& volume, BootSector boot, Fat32Options options,
                                 diagnostics::Logger* logger)
    : volume_(volume), boot_(std::move(boot)), logger_(logger) {
    // FatTable keeps a reference to boot_; this object is heap-allocated and never moved.
    fat_ = std::make_unique<FatTable>(volume_, boot_, options.fatCacheBytes);
    info_.type = FilesystemType::Fat32;
    info_.label = boot_.volumeLabel;
    info_.serialNumber = boot_.volumeId;
    info_.bytesPerSector = boot_.bytesPerSector;
    info_.clusterSize = boot_.clusterSize;
    info_.clusterCount = boot_.clusterCount;
    info_.firstCluster = 2;
    info_.volumeSize = boot_.volumeBytes;
    info_.dataOffset = boot_.dataOffset;
    info_.warnings = boot_.warnings;
}

Result<std::unique_ptr<Fat32Filesystem>> Fat32Filesystem::open(storage::IStorageSource& volume, Fat32Options options,
                                                               diagnostics::Logger* logger) {
    if (!volume.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "volume source is not open");
    }
    std::array<std::byte, 512> sector{};
    if (volume.size() < sector.size()) {
        return makeError(ErrorCode::UnsupportedFilesystem, "volume is too small for a FAT32 boot sector");
    }

    Result<BootSector> parsed = makeError(ErrorCode::IoError, "boot sector unreadable");
    if (Status read = volume.readExact(ByteOffset{0}, sector); read.ok()) {
        parsed = parseBootSector(sector, volume.size());
    } else {
        parsed = read.error();
    }

    if (!parsed.ok()) {
        // The backup boot sector normally lives at sector 6 of the reserved region.
        const Error primaryError = parsed.error();
        for (const std::uint32_t bps : {512U, 1024U, 2048U, 4096U}) {
            const std::uint64_t offset = 6ULL * bps;
            if (offset + sector.size() > volume.size() || !volume.readExact(ByteOffset{offset}, sector).ok()) {
                continue;
            }
            Result<BootSector> backup = parseBootSector(sector, volume.size());
            if (backup.ok() && backup->bytesPerSector == bps) {
                backup->warnings.insert(backup->warnings.begin(),
                                        "primary boot sector invalid (" + primaryError.message +
                                            "); using the backup boot sector at sector 6");
                parsed = std::move(backup);
                break;
            }
        }
        if (!parsed.ok()) {
            return primaryError;
        }
    }

    BootSector boot = std::move(parsed).value();
    std::optional<std::uint32_t> recordedFree;
    if (boot.fsInfoSector != 0 && boot.fsInfoSector < boot.reservedSectors) {
        std::array<std::byte, 512> fsInfo{};
        const std::uint64_t offset = static_cast<std::uint64_t>(boot.fsInfoSector) * boot.bytesPerSector;
        if (volume.readExact(ByteOffset{offset}, fsInfo).ok()) {
            if (const std::optional<FsInfo> info = parseFsInfo(fsInfo, boot)) {
                recordedFree = info->freeClusters;
            } else {
                boot.warnings.push_back("FSInfo sector has invalid signatures");
            }
        } else {
            boot.warnings.push_back("FSInfo sector is unreadable");
        }
    }

    std::unique_ptr<Fat32Filesystem> fs(new Fat32Filesystem(volume, std::move(boot), options, logger));
    fs->recordedFree_ = recordedFree;

    // The volume label entry in the root directory takes precedence over the BPB copy.
    if (const Result<DirectoryListing> root = fs->readDirectory(fs->rootDirectory()); root.ok() && fs->rootLabel_) {
        fs->info_.label = *fs->rootLabel_;
    }

    fs->log(LogLevel::Info, "FAT32 volume opened",
            {field("cluster_size", fs->boot_.clusterSize), field("clusters", fs->boot_.clusterCount),
             field("fat_count", fs->boot_.fatCount), field("warnings", fs->boot_.warnings.size())});
    for (const std::string& warning : fs->boot_.warnings) {
        fs->log(LogLevel::Warning, "FAT32 boot sector warning", {field("detail", warning)});
    }
    return fs;
}

DirectoryEntry Fat32Filesystem::rootDirectory() const {
    DirectoryEntry root;
    root.isDirectory = true;
    root.state = EntryState::Active;
    root.firstCluster = ClusterNumber{boot_.rootCluster};
    return root;
}

std::uint64_t Fat32Filesystem::maxDirectoryClusters() const noexcept {
    return std::max<std::uint64_t>(1, clustersFor(kMaxDirectoryBytes, boot_.clusterSize));
}

Fat32Filesystem::ChainWalk Fat32Filesystem::walkChain(std::uint32_t first, std::uint64_t maxClusters,
                                                      bool isDirectory) {
    ChainWalk walk;
    if (!boot_.isValidCluster(first)) {
        addIssue(walk.issues, AllocationIssue::InvalidStartCluster);
        return walk;
    }
    std::unordered_set<std::uint32_t> seen;
    std::uint32_t current = first;
    // Bounded: every iteration adds a distinct cluster, and at most maxClusters are taken.
    while (true) {
        if (!seen.insert(current).second) {
            addIssue(walk.issues, AllocationIssue::ChainLoop);
            break;
        }
        walk.clusters.push_back(current);
        const FatEntry next = fat_->entry(current);
        if (walk.clusters.size() >= maxClusters) {
            if (next.kind == FatEntryKind::Next) {
                addIssue(walk.issues, AllocationIssue::ChainLongerThanSize);
            } else if (const auto issue = issueForTerminator(next.kind)) {
                addIssue(walk.issues, *issue);
            }
            break;
        }
        if (next.kind == FatEntryKind::Next) {
            current = next.value;
            continue;
        }
        if (const auto issue = issueForTerminator(next.kind)) {
            addIssue(walk.issues, *issue);
        }
        break;
    }
    if (!isDirectory && walk.clusters.size() < maxClusters) {
        addIssue(walk.issues, AllocationIssue::ChainShorterThanSize);
    }
    return walk;
}

FileAllocation Fat32Filesystem::toAllocation(const std::vector<std::uint32_t>& clusters,
                                             std::optional<std::uint64_t> trimTo) const {
    FileAllocation allocation;
    allocation.clusterCount = clusters.size();
    std::uint64_t remaining = trimTo.value_or(std::numeric_limits<std::uint64_t>::max());
    for (std::size_t i = 0; i < clusters.size() && remaining > 0;) {
        std::size_t run = 1;
        while (i + run < clusters.size() && clusters[i + run] == clusters[i] + run) {
            ++run;
        }
        const std::uint64_t length = std::min<std::uint64_t>(run * static_cast<std::uint64_t>(boot_.clusterSize), remaining);
        allocation.extents.push_back(Extent{boot_.clusterOffset(clusters[i]), length});
        remaining -= length;
        i += run;
    }
    return allocation;
}

Fat32Filesystem::Resolved Fat32Filesystem::resolve(const DirectoryEntry& entry) {
    Resolved resolved;
    FileAllocation& allocation = resolved.allocation;
    std::vector<AllocationIssue> issues;
    const std::uint64_t first = entry.firstCluster.value();
    const bool deleted = entry.state == EntryState::Deleted;
    const std::uint64_t volumeData = static_cast<std::uint64_t>(boot_.clusterCount) * boot_.clusterSize;

    if (!entry.isDirectory && entry.size > volumeData) {
        addIssue(issues, AllocationIssue::SizeExceedsVolume);
    }

    if (entry.isDirectory && !deleted) {
        if (!boot_.isValidCluster(first)) {
            addIssue(issues, AllocationIssue::InvalidStartCluster);
        } else {
            ChainWalk walk = walkChain(static_cast<std::uint32_t>(first), maxDirectoryClusters(), true);
            allocation = toAllocation(walk.clusters, std::nullopt);
            allocation.method = AllocationMethod::ClusterChain;
            for (const AllocationIssue issue : walk.issues) {
                addIssue(issues, issue);
            }
            resolved.ownedClusters = std::move(walk.clusters);
        }
    } else if (!entry.isDirectory && entry.size == 0) {
        if (first != 0 && !deleted) {
            addIssue(issues, AllocationIssue::ChainLongerThanSize);  // clusters allocated to an empty file
        }
    } else if (first == 0 || !boot_.isValidCluster(first)) {
        addIssue(issues, AllocationIssue::InvalidStartCluster);
    } else if (!deleted) {
        const std::uint64_t needed = clustersFor(entry.size, boot_.clusterSize);
        ChainWalk walk = walkChain(static_cast<std::uint32_t>(first), needed, false);
        allocation = toAllocation(walk.clusters, entry.size);
        allocation.method = AllocationMethod::ClusterChain;
        for (const AllocationIssue issue : walk.issues) {
            addIssue(issues, issue);
        }
        resolved.ownedClusters = std::move(walk.clusters);
    } else {
        // Deleted: the chain was cleared, so assume the data is contiguous.
        // Deleted directories have no size; their first cluster is all that is known.
        const std::uint64_t needed = entry.isDirectory ? 1 : clustersFor(entry.size, boot_.clusterSize);
        const std::uint64_t available = static_cast<std::uint64_t>(boot_.lastCluster()) - first + 1;
        const std::uint64_t count = std::min(needed, available);
        if (count < needed) {
            addIssue(issues, AllocationIssue::BeyondVolume);
        }
        for (std::uint64_t i = 0; i < count; ++i) {
            const FatEntryKind kind = fat_->entry(static_cast<std::uint32_t>(first + i)).kind;
            if (kind == FatEntryKind::Unreadable) {
                addIssue(issues, AllocationIssue::UnreadableAllocationTable);
            } else if (kind != FatEntryKind::Free) {
                addIssue(issues, AllocationIssue::ClustersInUse);
                break;  // one reused cluster is enough to distrust the guess
            }
        }
        const std::uint64_t runBytes = count * boot_.clusterSize;
        allocation.method = AllocationMethod::ContiguousGuess;
        allocation.clusterCount = count;
        allocation.extents.push_back(Extent{boot_.clusterOffset(static_cast<std::uint32_t>(first)),
                                            entry.isDirectory ? runBytes : std::min(entry.size, runBytes)});
    }
    allocation.issues = std::move(issues);
    return resolved;
}

Result<FileAllocation> Fat32Filesystem::resolveAllocation(const DirectoryEntry& entry) {
    return resolve(entry).allocation;
}

Result<DirectoryListing> Fat32Filesystem::readDirectory(const DirectoryEntry& directory) {
    return listDirectory(directory, nullptr);
}

Result<DirectoryListing> Fat32Filesystem::listDirectory(const DirectoryEntry& directory,
                                                        std::vector<std::uint32_t>* chainOut) {
    if (!directory.isDirectory) {
        return makeError(ErrorCode::InvalidInput, "not a directory");
    }
    const std::uint64_t first = directory.firstCluster.value();
    if (!boot_.isValidCluster(first)) {
        return makeError(ErrorCode::CorruptedFilesystem,
                         "directory start cluster " + std::to_string(first) + " is out of range");
    }
    const auto start = static_cast<std::uint32_t>(first);
    const bool deleted = directory.state == EntryState::Deleted;

    DirectoryListing listing;
    std::vector<std::uint32_t> clusters;
    if (deleted) {
        // Only the first cluster is known, and only if nothing reused it.
        if (fat_->entry(start).kind != FatEntryKind::Free) {
            return makeError(ErrorCode::InvalidFormat, "deleted directory's cluster has been reused");
        }
        clusters.push_back(start);
    } else {
        ChainWalk walk = walkChain(start, maxDirectoryClusters(), true);
        clusters = std::move(walk.clusters);
        for (const AllocationIssue issue : walk.issues) {
            listing.issues.push_back(
                ScanIssue{ScanIssueKind::DirectoryInvalid, {}, "directory chain: " + std::string(toString(issue))});
        }
        if (chainOut != nullptr) {
            *chainOut = clusters;
        }
    }

    DirectoryParser parser(boot_);
    std::vector<std::byte> buffer(boot_.clusterSize);
    const std::size_t slots = boot_.clusterSize / kDirectoryEntrySize;
    bool reachedEnd = false;
    for (std::size_t index = 0; index < clusters.size() && !reachedEnd; ++index) {
        const std::uint64_t offset = boot_.clusterOffset(clusters[index]);
        if (const Status read = volume_.readExact(ByteOffset{offset}, buffer); !read.ok()) {
            listing.issues.push_back(ScanIssue{ScanIssueKind::DirectoryUnreadable, {},
                                               "cluster " + std::to_string(clusters[index]) + ": " +
                                                   describe(read.error())});
            if (deleted) {
                return makeError(ErrorCode::IoError, "deleted directory cluster is unreadable");
            }
            continue;
        }
        if (deleted && index == 0) {
            // A directory's first entry is "." pointing at itself.
            const std::span<const std::byte> dot(buffer.data(), kDirectoryEntrySize);
            const std::uint64_t self = (static_cast<std::uint64_t>(loadLe16(dot, 20)) << 16) | loadLe16(dot, 26);
            if (std::memcmp(dot.data(), ".          ", 11) != 0 || (loadU8(dot, 11) & 0x10) == 0 || self != first) {
                return makeError(ErrorCode::InvalidFormat, "deleted directory's cluster no longer holds that directory");
            }
        }
        for (std::size_t slot = 0; slot < slots; ++slot) {
            const std::span<const std::byte> entry(buffer.data() + slot * kDirectoryEntrySize, kDirectoryEntrySize);
            if (!parser.feed(entry, offset + slot * kDirectoryEntrySize, listing.entries)) {
                reachedEnd = true;
                break;
            }
        }
    }

    if (!deleted && start == boot_.rootCluster) {
        rootLabel_ = parser.volumeLabel();
    }
    return listing;
}

Result<FileScan> Fat32Filesystem::scan(const ScanLimits& limits, const CancellationToken& cancel) {
    struct Work {
        DirectoryEntry directory;
        std::string path;
        std::uint32_t depth = 0;
        bool deleted = false;
    };

    FileScan result;
    ClaimMap claims(static_cast<std::uint64_t>(boot_.lastCluster()) + 1);
    std::vector<std::uint32_t> conflicts;
    const auto claimAll = [&](const std::vector<std::uint32_t>& clusters) {
        for (const std::uint32_t cluster : clusters) {
            if (!claims.claim(cluster)) {
                conflicts.push_back(cluster);
            }
        }
    };

    std::unordered_set<std::uint64_t> visited{boot_.rootCluster};
    std::vector<Work> stack{Work{rootDirectory(), "", 0, false}};
    std::uint64_t entryCount = 0;

    while (!stack.empty()) {
        if (cancel.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "FAT32 scan cancelled");
        }
        Work work = std::move(stack.back());
        stack.pop_back();
        const std::string shownPath = work.path.empty() ? "/" : work.path;

        std::vector<std::uint32_t> chain;
        const Result<DirectoryListing> listing = listDirectory(work.directory, work.deleted ? nullptr : &chain);
        if (!listing.ok()) {
            const ScanIssueKind kind = listing.error().code == ErrorCode::IoError ? ScanIssueKind::DirectoryUnreadable
                                                                                  : ScanIssueKind::DirectoryInvalid;
            result.issues.push_back(ScanIssue{kind, shownPath, listing.error().message});
            continue;
        }
        claimAll(chain);
        for (ScanIssue issue : listing->issues) {
            issue.path = shownPath;
            result.issues.push_back(std::move(issue));
        }

        std::vector<Work> subdirectories;
        for (const DirectoryEntry& entry : listing->entries) {
            if (entry.state == EntryState::Deleted && !limits.includeDeleted) {
                continue;
            }
            if (++entryCount > limits.maxEntries) {
                result.issues.push_back(ScanIssue{ScanIssueKind::EntryLimit, shownPath,
                                                  "stopped after " + std::to_string(limits.maxEntries) + " entries"});
                result.complete = false;
                stack.clear();
                subdirectories.clear();
                break;
            }

            FileRecord record;
            record.entry = entry;
            record.path = work.path + "/" + entry.name;
            record.parentDeleted = work.deleted;
            if (work.deleted) {
                record.entry.state = EntryState::Deleted;  // its chain was freed with the parent
            }
            Resolved resolved = resolve(record.entry);
            const bool active = record.entry.state == EntryState::Active;
            if (active && !record.entry.isDirectory) {
                claimAll(resolved.ownedClusters);  // directories claim their chain when listed
            }
            record.allocation = std::move(resolved.allocation);

            if (record.entry.isDirectory) {
                const std::uint64_t cluster = record.entry.firstCluster.value();
                const bool recurse = active || limits.recurseIntoDeletedDirectories;
                if (recurse && boot_.isValidCluster(cluster)) {
                    if (visited.contains(cluster)) {
                        if (active) {
                            result.issues.push_back(ScanIssue{ScanIssueKind::DirectoryLoop, record.path,
                                                              "directory refers to an already visited directory"});
                        }
                    } else if (work.depth + 1 > limits.maxDepth) {
                        result.issues.push_back(ScanIssue{ScanIssueKind::DepthLimit, record.path,
                                                          "maximum depth " + std::to_string(limits.maxDepth)});
                        result.complete = false;
                    } else {
                        visited.insert(cluster);
                        subdirectories.push_back(Work{record.entry, record.path, work.depth + 1, !active});
                    }
                }
            }
            result.records.push_back(std::move(record));
        }
        // Depth-first, visiting subdirectories in on-disk order.
        stack.insert(stack.end(), std::make_move_iterator(subdirectories.rbegin()),
                     std::make_move_iterator(subdirectories.rend()));
    }

    // Cross-links: clusters owned by more than one active entry.
    std::sort(conflicts.begin(), conflicts.end());
    conflicts.erase(std::unique(conflicts.begin(), conflicts.end()), conflicts.end());
    result.crossLinkedClusters = conflicts.size();
    result.referencedClusters = claims.count();
    if (!conflicts.empty()) {
        std::vector<std::uint64_t> conflictOffsets;
        for (const std::uint32_t cluster : conflicts) {
            conflictOffsets.push_back(boot_.clusterOffset(cluster));
        }
        for (FileRecord& record : result.records) {
            if (record.entry.state != EntryState::Active ||
                record.allocation.method != AllocationMethod::ClusterChain) {
                continue;
            }
            for (const Extent& extent : record.allocation.extents) {
                const auto hit = std::lower_bound(conflictOffsets.begin(), conflictOffsets.end(), extent.offset);
                if (hit != conflictOffsets.end() && *hit < extent.offset + extent.length) {
                    addIssue(record.allocation.issues, AllocationIssue::CrossLinked);
                    break;
                }
            }
        }
    }

    log(LogLevel::Info, "FAT32 scan finished",
        {field("records", result.records.size()), field("issues", result.issues.size()),
         field("cross_linked_clusters", result.crossLinkedClusters), field("complete", result.complete ? 1 : 0)});
    return result;
}

Result<ClusterUsage> Fat32Filesystem::analyzeClusters(const CancellationToken& cancel) {
    ClusterUsage usage;
    usage.total = boot_.clusterCount;
    usage.recordedFree = recordedFree_;

    std::vector<std::uint32_t> active(kAnalysisChunk);
    std::vector<std::uint32_t> other(kAnalysisChunk);
    const std::uint32_t last = boot_.lastCluster();
    // Entries stored beyond the end of the volume (truncated image) cannot be
    // read from any copy; count them without attempting reads.
    std::uint64_t readableLimit = 0;
    for (std::uint32_t copy = 0; copy < boot_.fatCount; ++copy) {
        readableLimit = std::max(readableLimit, fat_->entriesInsideVolume(copy));
    }
    for (std::uint32_t first = 2; first <= last;) {
        if (cancel.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "FAT32 cluster analysis cancelled");
        }
        if (first >= readableLimit) {
            usage.unreadable += static_cast<std::uint64_t>(last) - first + 1;
            break;
        }
        const std::uint32_t count = std::min<std::uint32_t>(kAnalysisChunk, last - first + 1);
        const std::span<std::uint32_t> chunk(active.data(), count);
        const bool bulk = fat_->readRaw(boot_.activeFat, first, chunk);

        for (std::uint32_t i = 0; i < count; ++i) {
            // Unreadable chunks go through the table, which narrows failures
            // down to sectors and falls back to other FAT copies.
            const FatEntryKind kind = bulk ? fat_->classify(chunk[i]).kind : fat_->entry(first + i).kind;
            switch (toClusterState(kind)) {
            case ClusterState::Free:
                ++usage.free;
                break;
            case ClusterState::Allocated:
                ++usage.allocated;
                break;
            case ClusterState::Bad:
                ++usage.bad;
                break;
            case ClusterState::Invalid:
                ++usage.invalid;
                break;
            case ClusterState::Unreadable:
                ++usage.unreadable;
                break;
            }
        }

        if (bulk && boot_.mirrored) {
            for (std::uint32_t copy = 0; copy < boot_.fatCount; ++copy) {
                const std::span<std::uint32_t> mirror(other.data(), count);
                if (copy == boot_.activeFat || !fat_->readRaw(copy, first, mirror)) {
                    continue;
                }
                for (std::uint32_t i = 0; i < count; ++i) {
                    if ((chunk[i] & 0x0FFFFFFF) != (mirror[i] & 0x0FFFFFFF)) {
                        ++usage.mirrorMismatches;
                    }
                }
            }
        }
        first += count;
        if (count == 0) {
            break;
        }
    }
    return usage;
}

Result<ClusterState> Fat32Filesystem::clusterState(ClusterNumber cluster) {
    if (!boot_.isValidCluster(cluster.value())) {
        return makeError(ErrorCode::InvalidInput, "cluster " + std::to_string(cluster.value()) + " is out of range");
    }
    return toClusterState(fat_->entry(static_cast<std::uint32_t>(cluster.value())).kind);
}

void Fat32Filesystem::log(LogLevel level, std::string_view message,
                          std::initializer_list<diagnostics::LogField> fields) const {
    if (logger_ != nullptr) {
        logger_->log(level, kComponent, message, fields);
    }
}

}  // namespace recovery::filesystem::fat32
