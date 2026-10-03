#include "filesystem/exfat/exfat_filesystem.hpp"

#include "allocation_bitmap.hpp"
#include "directory_parser.hpp"
#include "fat_table.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <unordered_set>

namespace recovery::filesystem::exfat {

namespace {

using diagnostics::field;
using diagnostics::LogLevel;

constexpr std::string_view kComponent = "exfat";
// The specification limits a directory to 256 MiB (2^23 entries).
constexpr std::uint64_t kMaxDirectoryBytes = 256ULL * 1024 * 1024;
constexpr std::size_t kDirectoryChunk = 64 * 1024;
// Clusters counted per step of the cluster analysis (1 MiB of bitmap).
constexpr std::uint64_t kAnalysisSlice = 8ULL * 1024 * 1024;
constexpr std::size_t kBootSectorSize = 512;

std::uint64_t clustersFor(std::uint64_t bytes, std::uint32_t clusterSize) noexcept {
    return bytes == 0 ? 0 : (bytes - 1) / clusterSize + 1;
}

void addIssue(std::vector<AllocationIssue>& issues, AllocationIssue issue) {
    if (std::find(issues.begin(), issues.end(), issue) == issues.end()) {
        issues.push_back(issue);
    }
}

std::optional<AllocationIssue> issueForTerminator(FatEntryKind kind) noexcept {
    switch (kind) {
    case FatEntryKind::Bad:
        return AllocationIssue::BadClusterInChain;
    case FatEntryKind::Invalid:
        return AllocationIssue::InvalidClusterInChain;
    case FatEntryKind::Unreadable:
        return AllocationIssue::UnreadableAllocationTable;
    case FatEntryKind::Next:
    case FatEntryKind::EndOfChain:
        return std::nullopt;
    }
    return std::nullopt;
}

// Calls `visit(first, count)` for each run of consecutive clusters.
template <typename Visit>
void forEachRun(const std::vector<std::uint32_t>& clusters, Visit visit) {
    for (std::size_t i = 0; i < clusters.size();) {
        std::size_t run = 1;
        while (i + run < clusters.size() && clusters[i + run] == clusters[i] + run) {
            ++run;
        }
        visit(static_cast<std::uint64_t>(clusters[i]), static_cast<std::uint64_t>(run));
        i += run;
    }
}

struct BootCandidate {
    Result<BootSector> boot;
    bool checksumMatches = false;
};

BootCandidate readBootRegion(storage::IStorageSource& volume, std::uint64_t offset) {
    std::array<std::byte, kBootSectorSize> sector{};
    if (!rangeWithin<std::uint64_t>(offset, sector.size(), volume.size())) {
        return {makeError(ErrorCode::UnsupportedFilesystem, "volume is too small for this boot region"), false};
    }
    if (const Status read = volume.readExact(ByteOffset{offset}, sector); !read.ok()) {
        return {read.error(), false};
    }
    BootCandidate candidate{parseBootSector(sector, volume.size()), false};
    if (candidate.boot.ok()) {
        const std::uint64_t bytes = static_cast<std::uint64_t>(candidate.boot->bytesPerSector) * kBootRegionSectors;
        if (rangeWithin<std::uint64_t>(offset, bytes, volume.size())) {
            std::vector<std::byte> region(static_cast<std::size_t>(bytes));
            candidate.checksumMatches = volume.readExact(ByteOffset{offset}, region).ok() &&
                                        bootChecksumMatches(region, candidate.boot->bytesPerSector);
        }
    }
    return candidate;
}

}  // namespace

ExFatFilesystem::~ExFatFilesystem() = default;

ExFatFilesystem::ExFatFilesystem(storage::IStorageSource& volume, BootSector boot, ExFatOptions options,
                                 diagnostics::Logger* logger)
    : volume_(volume), boot_(std::move(boot)), options_(options), logger_(logger) {
    // FatTable keeps a reference to boot_; this object is heap-allocated and never moved.
    fat_ = std::make_unique<FatTable>(volume_, boot_, options_.fatCacheBytes);
    info_.type = FilesystemType::ExFat;
    info_.serialNumber = boot_.volumeSerial;
    info_.bytesPerSector = boot_.bytesPerSector;
    info_.clusterSize = boot_.clusterSize;
    info_.clusterCount = boot_.clusterCount;
    info_.firstCluster = 2;
    info_.volumeSize = boot_.volumeBytes;
    info_.dataOffset = boot_.dataOffset;
    info_.warnings = boot_.warnings;
}

Result<std::unique_ptr<ExFatFilesystem>> ExFatFilesystem::open(storage::IStorageSource& volume, ExFatOptions options,
                                                               diagnostics::Logger* logger) {
    if (!volume.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "volume source is not open");
    }
    if (volume.size() < kBootSectorSize) {
        return makeError(ErrorCode::UnsupportedFilesystem, "volume is too small for an exFAT boot sector");
    }

    BootCandidate main = readBootRegion(volume, 0);
    std::optional<BootSector> chosen;
    std::vector<std::string> notes;
    if (main.boot.ok() && main.checksumMatches) {
        chosen = std::move(main.boot).value();
    } else {
        // The backup boot region immediately follows the main one.
        std::optional<BootCandidate> backup;
        const std::vector<std::uint32_t> sectorSizes = main.boot.ok()
                                                           ? std::vector<std::uint32_t>{main.boot->bytesPerSector}
                                                           : std::vector<std::uint32_t>{512, 1024, 2048, 4096};
        for (const std::uint32_t bytesPerSector : sectorSizes) {
            BootCandidate candidate =
                readBootRegion(volume, static_cast<std::uint64_t>(bytesPerSector) * kBootRegionSectors);
            if (candidate.boot.ok() && candidate.boot->bytesPerSector == bytesPerSector) {
                backup = std::move(candidate);
                break;
            }
        }
        const std::string mainProblem = main.boot.ok() ? "checksum mismatch" : main.boot.error().message;
        if (backup && backup->checksumMatches) {
            chosen = std::move(backup->boot).value();
            notes.push_back("main boot region invalid (" + mainProblem + "); using the backup boot region");
        } else if (main.boot.ok()) {
            chosen = std::move(main.boot).value();
            notes.push_back("boot region checksum mismatch, and no valid backup boot region");
        } else if (backup) {
            chosen = std::move(backup->boot).value();
            notes.push_back("main boot sector invalid (" + mainProblem +
                            "); using the backup boot region although its checksum does not match");
        } else {
            return main.boot.error();
        }
    }
    chosen->warnings.insert(chosen->warnings.begin(), notes.begin(), notes.end());

    std::unique_ptr<ExFatFilesystem> fs(new ExFatFilesystem(volume, std::move(*chosen), options, logger));
    fs->loadSystemStructures();

    fs->log(LogLevel::Info, "exFAT volume opened",
            {field("cluster_size", fs->boot_.clusterSize), field("clusters", fs->boot_.clusterCount),
             field("fat_count", fs->boot_.fatCount), field("warnings", fs->info_.warnings.size())});
    for (const std::string& warning : fs->info_.warnings) {
        fs->log(LogLevel::Warning, "exFAT volume warning", {field("detail", warning)});
    }
    return fs;
}

void ExFatFilesystem::loadSystemStructures() {
    // The root directory holds the allocation bitmap, up-case table and label
    // entries. It is parsed once with the basic up-case table to find them.
    ParseOutput system;
    bool truncated = false;
    if (const Result<DirectoryListing> root =
            listDirectory(rootDirectory(), options_.maxListingEntries, truncated, &system);
        !root.ok()) {
        info_.warnings.push_back("root directory unreadable: " + root.error().message);
    }

    // Allocation bitmap: the one belonging to the active FAT.
    const BitmapEntryInfo* bitmap = nullptr;
    for (const BitmapEntryInfo& candidate : system.bitmaps) {
        if ((candidate.flags & 1U) == boot_.activeFat) {
            bitmap = &candidate;
            break;
        }
    }
    if (bitmap == nullptr && !system.bitmaps.empty()) {
        bitmap = &system.bitmaps.front();
        info_.warnings.push_back("no allocation bitmap for the active FAT; using the first bitmap entry");
    }
    if (bitmap == nullptr) {
        info_.warnings.push_back("allocation bitmap entry is missing; cluster allocation is unknown");
    } else {
        const std::uint64_t needed = (static_cast<std::uint64_t>(boot_.clusterCount) + 7) / 8;
        if (bitmap->length < needed) {
            info_.warnings.push_back("allocation bitmap covers only " + std::to_string(bitmap->length * 8) + " of " +
                                     std::to_string(boot_.clusterCount) + " clusters");
        }
        const std::uint64_t bytes = std::min(bitmap->length, needed);
        if (bytes == 0) {
            info_.warnings.push_back("allocation bitmap is empty; cluster allocation is unknown");
        } else if (const std::optional<std::vector<Run>> runs =
                       systemRuns(bitmap->firstCluster, bytes, "allocation bitmap")) {
            bitmap_ = std::make_unique<AllocationBitmap>(volume_, extentsOf(*runs, bytes), boot_.clusterCount,
                                                         boot_.bytesPerSector, options_.bitmapCacheBytes);
            systemRuns_.insert(systemRuns_.end(), runs->begin(), runs->end());
        }
    }

    // Up-case table, needed to verify name hashes.
    constexpr std::string_view kFallback = "; name hashes are checked with a basic built-in table";
    if (system.upcaseTables.empty()) {
        info_.warnings.push_back("up-case table entry is missing" + std::string(kFallback));
    } else if (const UpcaseEntryInfo& table = system.upcaseTables.front();
               table.length == 0 || table.length > kMaxUpcaseTableBytes || table.length % 2 != 0) {
        info_.warnings.push_back("up-case table length " + std::to_string(table.length) + " is invalid" +
                                 std::string(kFallback));
    } else if (const std::optional<std::vector<Run>> runs =
                   systemRuns(table.firstCluster, table.length, "up-case table")) {
        systemRuns_.insert(systemRuns_.end(), runs->begin(), runs->end());
        std::vector<std::byte> data(static_cast<std::size_t>(table.length));
        bool readable = true;
        std::size_t done = 0;
        for (const Extent& extent : extentsOf(*runs, table.length)) {
            const std::span<std::byte> piece(data.data() + done, static_cast<std::size_t>(extent.length));
            if (!volume_.readExact(ByteOffset{extent.offset}, piece).ok()) {
                readable = false;
                break;
            }
            done += piece.size();
        }
        std::optional<UpcaseTable> decoded;
        if (!readable) {
            info_.warnings.push_back("up-case table is unreadable" + std::string(kFallback));
        } else if (upcaseTableChecksum(data) != table.checksum) {
            info_.warnings.push_back("up-case table checksum mismatch" + std::string(kFallback));
        } else if (decoded = UpcaseTable::decode(data); !decoded) {
            info_.warnings.push_back("up-case table is malformed" + std::string(kFallback));
        } else {
            upcase_ = std::move(*decoded);
        }
    }

    if (system.label) {
        info_.label = *system.label;
    }
}

std::optional<std::vector<ExFatFilesystem::Run>> ExFatFilesystem::systemRuns(std::uint32_t first, std::uint64_t bytes,
                                                                             std::string_view what) {
    if (!boot_.isValidCluster(first)) {
        info_.warnings.push_back(std::string(what) + " start cluster " + std::to_string(first) + " is out of range");
        return std::nullopt;
    }
    const std::uint64_t needed = clustersFor(bytes, boot_.clusterSize);
    const ChainWalk walk = walkChain(first, needed, true);
    const bool chainUsable =
        walk.clusters.size() == needed &&
        std::all_of(walk.issues.begin(), walk.issues.end(),
                    [](AllocationIssue issue) { return issue == AllocationIssue::ChainLongerThanSize; });
    std::vector<Run> runs;
    if (chainUsable) {
        forEachRun(walk.clusters, [&](std::uint64_t start, std::uint64_t count) { runs.push_back(Run{start, count}); });
        return runs;
    }
    const std::uint64_t available = static_cast<std::uint64_t>(boot_.lastCluster()) - first + 1;
    if (needed > available) {
        info_.warnings.push_back(std::string(what) + " extends beyond the cluster heap");
        return std::nullopt;
    }
    info_.warnings.push_back(std::string(what) + " has no valid cluster chain; assuming it is contiguous");
    runs.push_back(Run{first, needed});
    return runs;
}

std::vector<Extent> ExFatFilesystem::extentsOf(const std::vector<Run>& runs, std::uint64_t bytes) const {
    std::vector<Extent> extents;
    std::uint64_t remaining = bytes;
    for (const Run& run : runs) {
        if (remaining == 0) {
            break;
        }
        const std::uint64_t length = std::min(run.count * boot_.clusterSize, remaining);
        extents.push_back(Extent{boot_.clusterOffset(run.first), length});
        remaining -= length;
    }
    return extents;
}

DirectoryEntry ExFatFilesystem::rootDirectory() const {
    DirectoryEntry root;
    root.isDirectory = true;
    root.state = EntryState::Active;
    root.firstCluster = ClusterNumber{boot_.rootCluster};
    return root;
}

bool ExFatFilesystem::isRoot(const DirectoryEntry& entry) const noexcept {
    // The root has no Stream Extension, so no size: its chain is its extent.
    return entry.isDirectory && entry.state == EntryState::Active && entry.size == 0 && !entry.contiguousData &&
           entry.firstCluster.value() == boot_.rootCluster;
}

std::uint64_t ExFatFilesystem::maxDirectoryClusters() const noexcept {
    return std::max<std::uint64_t>(1, clustersFor(kMaxDirectoryBytes, boot_.clusterSize));
}

ExFatFilesystem::ChainWalk ExFatFilesystem::walkChain(std::uint32_t first, std::uint64_t maxClusters,
                                                      bool expectExact) {
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
    if (expectExact && walk.clusters.size() < maxClusters) {
        addIssue(walk.issues, AllocationIssue::ChainShorterThanSize);
    }
    return walk;
}

void ExFatFilesystem::checkRun(const Run& run, bool deleted, std::vector<AllocationIssue>& issues) {
    if (run.count == 0) {
        return;
    }
    if (bitmap_ == nullptr) {
        addIssue(issues, AllocationIssue::UnreadableAllocationTable);
        return;
    }
    // One reused (or, for active entries, unallocated) cluster is enough to flag the entry.
    const BitCounts counts = bitmap_->count(run.first, run.count, deleted ? BitState::Allocated : BitState::Free);
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

FileAllocation ExFatFilesystem::toAllocation(const std::vector<std::uint32_t>& clusters,
                                             std::optional<std::uint64_t> trimTo) const {
    FileAllocation allocation;
    allocation.clusterCount = clusters.size();
    std::uint64_t remaining = trimTo.value_or(std::numeric_limits<std::uint64_t>::max());
    forEachRun(clusters, [&](std::uint64_t first, std::uint64_t count) {
        if (remaining == 0) {
            return;
        }
        const std::uint64_t length = std::min(count * boot_.clusterSize, remaining);
        allocation.extents.push_back(Extent{boot_.clusterOffset(first), length});
        remaining -= length;
    });
    return allocation;
}

ExFatFilesystem::Resolved ExFatFilesystem::resolve(const DirectoryEntry& entry) {
    Resolved resolved;
    FileAllocation& allocation = resolved.allocation;
    std::vector<AllocationIssue> issues;
    const std::uint64_t first = entry.firstCluster.value();
    const bool deleted = entry.state == EntryState::Deleted;
    const std::uint32_t clusterSize = boot_.clusterSize;
    const auto claimChain = [&](const std::vector<std::uint32_t>& clusters, bool own) {
        forEachRun(clusters, [&](std::uint64_t start, std::uint64_t count) {
            checkRun(Run{start, count}, deleted, issues);
            if (own) {
                resolved.ownedRuns.push_back(Run{start, count});
            }
        });
    };

    if (isRoot(entry)) {
        ChainWalk walk = walkChain(static_cast<std::uint32_t>(first), maxDirectoryClusters(), false);
        allocation = toAllocation(walk.clusters, std::nullopt);
        allocation.method = AllocationMethod::ClusterChain;
        issues = walk.issues;
        claimChain(walk.clusters, true);
        allocation.issues = std::move(issues);
        return resolved;
    }

    if (entry.size > static_cast<std::uint64_t>(boot_.clusterCount) * clusterSize) {
        addIssue(issues, AllocationIssue::SizeExceedsVolume);
    }
    // Directories are read whole clusters at a time, and at most 256 MiB.
    const std::uint64_t dataBytes = entry.isDirectory ? std::min(entry.size, kMaxDirectoryBytes) : entry.size;
    const std::uint64_t needed = clustersFor(dataBytes, clusterSize);
    const std::optional<std::uint64_t> trimTo =
        entry.isDirectory ? std::nullopt : std::optional<std::uint64_t>(entry.size);
    const auto contiguousRun = [&](AllocationMethod method) {
        const std::uint64_t available = static_cast<std::uint64_t>(boot_.lastCluster()) - first + 1;
        const std::uint64_t count = std::min(needed, available);
        if (count < needed) {
            addIssue(issues, AllocationIssue::BeyondVolume);
        }
        const std::uint64_t runBytes = count * clusterSize;
        allocation.method = method;
        allocation.clusterCount = count;
        allocation.extents.push_back(Extent{boot_.clusterOffset(first), std::min(trimTo.value_or(runBytes), runBytes)});
        checkRun(Run{first, count}, deleted, issues);
        if (!deleted) {
            resolved.ownedRuns.push_back(Run{first, count});
        }
    };

    if (needed == 0) {
        if (first != 0 && !deleted) {
            addIssue(issues, AllocationIssue::ChainLongerThanSize);  // clusters allocated to an empty file
        }
    } else if (!boot_.isValidCluster(first)) {
        addIssue(issues, AllocationIssue::InvalidStartCluster);
    } else if (entry.contiguousData) {
        contiguousRun(AllocationMethod::Contiguous);
    } else {
        ChainWalk walk = walkChain(static_cast<std::uint32_t>(first), needed, true);
        if (!deleted) {
            allocation = toAllocation(walk.clusters, trimTo);
            allocation.method = AllocationMethod::ClusterChain;
            for (const AllocationIssue issue : walk.issues) {
                addIssue(issues, issue);
            }
            claimChain(walk.clusters, true);
        } else if (walk.issues.empty() && walk.clusters.size() == needed) {
            // Deletion only has to free the clusters in the bitmap, so the old
            // chain may survive; it is used only if it is complete and ends
            // exactly where the size says it should.
            allocation = toAllocation(walk.clusters, trimTo);
            allocation.method = AllocationMethod::ClusterChain;
            claimChain(walk.clusters, false);
        } else {
            // The chain is gone or damaged: guess that the data is contiguous.
            contiguousRun(AllocationMethod::ContiguousGuess);
        }
    }
    allocation.issues = std::move(issues);
    return resolved;
}

Result<FileAllocation> ExFatFilesystem::resolveAllocation(const DirectoryEntry& entry) {
    return resolve(entry).allocation;
}

Result<DirectoryListing> ExFatFilesystem::readDirectory(const DirectoryEntry& directory) {
    bool truncated = false;
    Result<DirectoryListing> listing = listDirectory(directory, options_.maxListingEntries, truncated, nullptr);
    if (listing.ok() && truncated) {
        listing->issues.push_back(ScanIssue{ScanIssueKind::EntryLimit, {},
                                            "stopped after " + std::to_string(listing->entries.size()) + " entries"});
    }
    return listing;
}

Result<DirectoryListing> ExFatFilesystem::listDirectory(const DirectoryEntry& directory, std::uint64_t budget,
                                                        bool& truncated, ParseOutput* systemEntries) {
    truncated = false;
    if (!directory.isDirectory) {
        return makeError(ErrorCode::InvalidInput, "not a directory");
    }
    const std::uint64_t first = directory.firstCluster.value();
    if (!boot_.isValidCluster(first)) {
        return makeError(ErrorCode::CorruptedFilesystem,
                         "directory start cluster " + std::to_string(first) + " is out of range");
    }
    const bool deleted = directory.state == EntryState::Deleted;

    DirectoryListing listing;
    const Resolved resolved = resolve(directory);
    const FileAllocation& allocation = resolved.allocation;
    if (deleted) {
        if (allocation.hasIssue(AllocationIssue::ClustersInUse)) {
            return makeError(ErrorCode::InvalidFormat, "deleted directory's clusters have been reused");
        }
        if (allocation.hasIssue(AllocationIssue::UnreadableAllocationTable)) {
            return makeError(ErrorCode::InvalidFormat, "deleted directory's clusters cannot be verified as unused");
        }
    } else {
        for (const AllocationIssue issue : allocation.issues) {
            listing.issues.push_back(ScanIssue{
                ScanIssueKind::DirectoryInvalid, {}, "directory allocation: " + std::string(toString(issue))});
        }
    }
    if (directory.size > kMaxDirectoryBytes) {
        listing.issues.push_back(ScanIssue{ScanIssueKind::DirectoryInvalid, {},
                                           "directory size " + std::to_string(directory.size) +
                                               " exceeds the 256 MiB limit; only the first 256 MiB are read"});
    }
    if (allocation.extents.empty()) {
        return makeError(ErrorCode::CorruptedFilesystem, "directory has no clusters");
    }

    ParseOutput out;
    DirectoryParser parser(boot_, upcase_);
    std::vector<std::byte> buffer(kDirectoryChunk);
    bool ended = false;
    std::uint64_t unreadableSectors = 0;
    std::uint64_t firstUnreadable = 0;
    const auto feedAll = [&](std::span<const std::byte> bytes, std::uint64_t offset) {
        for (std::size_t pos = 0; pos + kDirectoryEntrySize <= bytes.size(); pos += kDirectoryEntrySize) {
            if (!parser.feed(bytes.subspan(pos, kDirectoryEntrySize), offset + pos, out)) {
                ended = true;
                return;
            }
        }
    };
    for (const Extent& extent : allocation.extents) {
        for (std::uint64_t pos = 0; pos < extent.length && !ended && !truncated;) {
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(kDirectoryChunk, extent.length - pos));
            const std::uint64_t offset = extent.offset + pos;
            const std::span<std::byte> chunk(buffer.data(), length);
            if (volume_.readExact(ByteOffset{offset}, chunk).ok()) {
                feedAll(chunk, offset);
            } else {
                // Narrow the failure down to sectors; an entry set cannot span a gap.
                const std::size_t sector = boot_.bytesPerSector;
                for (std::size_t s = 0; s < length && !ended; s += sector) {
                    const std::span<std::byte> piece(buffer.data() + s, std::min(sector, length - s));
                    if (volume_.readExact(ByteOffset{offset + s}, piece).ok()) {
                        feedAll(piece, offset + s);
                    } else {
                        if (unreadableSectors++ == 0) {
                            firstUnreadable = offset + s;
                        }
                        parser.interrupt(out);
                    }
                }
            }
            pos += length;
            truncated = out.entries.size() >= budget;
        }
        if (ended || truncated) {
            break;
        }
    }
    parser.finish(out);

    if (unreadableSectors > 0) {
        listing.issues.push_back(ScanIssue{ScanIssueKind::DirectoryUnreadable, {},
                                           std::format("{} unreadable sector(s), the first at volume offset {}",
                                                       unreadableSectors, firstUnreadable)});
    }
    for (std::string& problem : out.problems) {
        listing.issues.push_back(ScanIssue{ScanIssueKind::DirectoryInvalid, {}, std::move(problem)});
    }
    listing.entries = std::move(out.entries);
    if (systemEntries != nullptr) {
        systemEntries->bitmaps = std::move(out.bitmaps);
        systemEntries->upcaseTables = std::move(out.upcaseTables);
        systemEntries->label = std::move(out.label);
    }
    return listing;
}

Result<FileScan> ExFatFilesystem::scan(const ScanLimits& limits, const CancellationToken& cancel) {
    struct Work {
        DirectoryEntry directory;
        std::string path;
        std::uint32_t depth = 0;
        bool deleted = false;
    };

    FileScan result;
    std::vector<Run> claims = systemRuns_;
    const DirectoryEntry root = rootDirectory();
    {
        const Resolved resolvedRoot = resolve(root);
        claims.insert(claims.end(), resolvedRoot.ownedRuns.begin(), resolvedRoot.ownedRuns.end());
    }

    std::unordered_set<std::uint64_t> visited{boot_.rootCluster};
    std::vector<Work> stack{Work{root, "", 0, false}};
    std::uint64_t entryCount = 0;

    while (!stack.empty()) {
        if (cancel.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "exFAT scan cancelled");
        }
        Work work = std::move(stack.back());
        stack.pop_back();
        const std::string shownPath = work.path.empty() ? "/" : work.path;

        // One entry more than allowed, so that the limit is noticed below.
        const std::uint64_t remaining = limits.maxEntries - std::min(entryCount, limits.maxEntries);
        const std::uint64_t budget = remaining == std::numeric_limits<std::uint64_t>::max() ? remaining : remaining + 1;
        bool truncated = false;
        const Result<DirectoryListing> listing = listDirectory(work.directory, budget, truncated, nullptr);
        if (!listing.ok()) {
            const ScanIssueKind kind = listing.error().code == ErrorCode::IoError ? ScanIssueKind::DirectoryUnreadable
                                                                                  : ScanIssueKind::DirectoryInvalid;
            result.issues.push_back(ScanIssue{kind, shownPath, listing.error().message});
            continue;
        }
        for (ScanIssue issue : listing->issues) {
            issue.path = shownPath;
            result.issues.push_back(std::move(issue));
        }

        std::vector<Work> subdirectories;
        bool stopped = false;
        for (const DirectoryEntry& entry : listing->entries) {
            if (entry.state == EntryState::Deleted && !limits.includeDeleted) {
                continue;
            }
            if (++entryCount > limits.maxEntries) {
                stopped = true;
                break;
            }

            FileRecord record;
            record.entry = entry;
            record.path = work.path + "/" + entry.name;
            record.parentDeleted = work.deleted;
            if (work.deleted) {
                record.entry.state = EntryState::Deleted;  // its clusters were freed with the parent
            }
            Resolved resolved = resolve(record.entry);
            const bool active = record.entry.state == EntryState::Active;
            claims.insert(claims.end(), resolved.ownedRuns.begin(), resolved.ownedRuns.end());
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
        if (stopped || truncated) {
            result.issues.push_back(ScanIssue{ScanIssueKind::EntryLimit, shownPath,
                                              "stopped after " + std::to_string(limits.maxEntries) + " entries"});
            result.complete = false;
            break;
        }
        // Depth-first, visiting subdirectories in on-disk order.
        stack.insert(stack.end(), std::make_move_iterator(subdirectories.rbegin()),
                     std::make_move_iterator(subdirectories.rend()));
    }

    // Cross-links: clusters claimed by more than one active entry (or by an
    // entry and a system structure). Runs are merged after sorting, so memory
    // stays proportional to the number of runs, not to the volume size.
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
            result.referencedClusters += unionEnd - unionStart;
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
    result.referencedClusters += unionEnd - unionStart;
    for (const auto& [start, end] : conflicts) {
        result.crossLinkedClusters += end - start;
    }
    if (!conflicts.empty()) {
        std::vector<std::uint64_t> conflictEnds;
        for (const auto& conflict : conflicts) {
            conflictEnds.push_back(boot_.clusterOffset(conflict.second));
        }
        for (FileRecord& record : result.records) {
            const AllocationMethod method = record.allocation.method;
            if (record.entry.state != EntryState::Active ||
                (method != AllocationMethod::ClusterChain && method != AllocationMethod::Contiguous)) {
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

    log(LogLevel::Info, "exFAT scan finished",
        {field("records", result.records.size()), field("issues", result.issues.size()),
         field("cross_linked_clusters", result.crossLinkedClusters), field("complete", result.complete ? 1 : 0)});
    return result;
}

Result<ClusterUsage> ExFatFilesystem::analyzeClusters(const CancellationToken& cancel) {
    if (cancel.isCancellationRequested()) {
        return makeError(ErrorCode::Cancelled, "exFAT cluster analysis cancelled");
    }
    ClusterUsage usage;
    usage.total = boot_.clusterCount;
    const std::uint64_t last = boot_.lastCluster();
    if (bitmap_ != nullptr) {
        for (std::uint64_t first = 2; first <= last;) {
            if (cancel.isCancellationRequested()) {
                return makeError(ErrorCode::Cancelled, "exFAT cluster analysis cancelled");
            }
            const std::uint64_t count = std::min(kAnalysisSlice, last - first + 1);
            const BitCounts counts = bitmap_->count(first, count);
            usage.free += counts.free;
            usage.allocated += counts.allocated;
            usage.unreadable += counts.unreadable;
            first += count;
        }
    } else {
        usage.unreadable = usage.total;
    }

    // Bad clusters are marked in the FAT (and normally allocated in the bitmap).
    PagedRegion& fat = fat_->region();
    constexpr std::uint64_t kEntriesPerPage = PagedRegion::kPageBytes / 4;
    for (std::uint64_t index = 0; index < fat.pageCount(); ++index) {
        if (cancel.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "exFAT cluster analysis cancelled");
        }
        const PagedRegion::Page& page = fat.page(index);
        if (page.noneReadable) {
            continue;
        }
        for (std::size_t i = 0; i + 4 <= page.length; i += 4) {
            const std::uint64_t cluster = index * kEntriesPerPage + i / 4;
            if (cluster < 2 || cluster > last || !page.readable(i, fat.sectorSize()) ||
                loadLe32(page.bytes, i) != kFatBadCluster) {
                continue;
            }
            ++usage.bad;
            switch (bitmap_ != nullptr ? bitmap_->state(static_cast<std::uint32_t>(cluster)) : BitState::Unreadable) {
            case BitState::Free:
                --usage.free;
                break;
            case BitState::Allocated:
                --usage.allocated;
                break;
            case BitState::Unreadable:
                --usage.unreadable;
                break;
            }
        }
    }
    return usage;
}

Result<ClusterState> ExFatFilesystem::clusterState(ClusterNumber cluster) {
    if (!boot_.isValidCluster(cluster.value())) {
        return makeError(ErrorCode::InvalidInput, "cluster " + std::to_string(cluster.value()) + " is out of range");
    }
    const auto number = static_cast<std::uint32_t>(cluster.value());
    if (fat_->entry(number).kind == FatEntryKind::Bad) {
        return ClusterState::Bad;
    }
    if (bitmap_ == nullptr) {
        return ClusterState::Unreadable;
    }
    switch (bitmap_->state(number)) {
    case BitState::Free:
        return ClusterState::Free;
    case BitState::Allocated:
        return ClusterState::Allocated;
    case BitState::Unreadable:
        return ClusterState::Unreadable;
    }
    return ClusterState::Unreadable;
}

void ExFatFilesystem::log(LogLevel level, std::string_view message,
                          std::initializer_list<diagnostics::LogField> fields) const {
    if (logger_ != nullptr) {
        logger_->log(level, kComponent, message, fields);
    }
}

}  // namespace recovery::filesystem::exfat
