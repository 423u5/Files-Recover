#include "fragment_evidence.hpp"

#include "recovery/checked_math.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace recovery::fragments {

namespace {

// Unknown state in the cache.
constexpr std::uint8_t kNotCached = 0xFF;

// Appends a region, merging it into the previous one when it continues it.
void appendRegion(std::vector<SourceRegion>& regions, const SourceRegion& region) {
    if (region.length == 0) {
        return;
    }
    if (!regions.empty()) {
        SourceRegion& last = regions.back();
        const bool sameKind = last.kind == region.kind && last.reallocated == region.reallocated;
        const bool continues =
            region.kind != RegionKind::Stored || last.sourceOffset + last.length == region.sourceOffset;
        if (sameKind && continues) {
            last.length += region.length;
            return;
        }
    }
    regions.push_back(region);
}

}  // namespace

// ---------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------

void appendRun(Layout& layout, Run run) {
    if (run.count == 0) {
        return;
    }
    if (!layout.empty()) {
        Run& last = layout.back();
        const bool continues = last.placed() ? run.placed() && last.first + last.count == run.first : !run.placed();
        if (continues) {
            last.count += run.count;
            return;
        }
    }
    layout.push_back(run);
}

std::uint64_t clustersIn(const Layout& layout) noexcept {
    std::uint64_t total = 0;
    for (const Run& run : layout) {
        total += run.count;
    }
    return total;
}

Layout prefixOf(const Layout& layout, std::uint64_t clusters) {
    Layout prefix;
    std::uint64_t left = clusters;
    for (const Run& run : layout) {
        if (left == 0) {
            break;
        }
        const std::uint64_t take = std::min(left, run.count);
        prefix.push_back(Run{run.first, take});
        left -= take;
    }
    return prefix;
}

std::optional<std::uint64_t> clusterAt(const Layout& layout, std::uint64_t fileCluster) noexcept {
    std::uint64_t position = 0;
    for (const Run& run : layout) {
        if (fileCluster < position + run.count) {
            if (!run.placed()) {
                return std::nullopt;
            }
            return run.first + (fileCluster - position);
        }
        position += run.count;
    }
    return std::nullopt;
}

std::size_t fragmentsOf(const Layout& layout) noexcept {
    std::size_t fragments = 0;
    std::optional<std::uint64_t> previousEnd;
    for (const Run& run : layout) {
        if (!run.placed()) {
            continue;
        }
        if (previousEnd != run.first) {
            ++fragments;
        }
        previousEnd = run.first + run.count;
    }
    return fragments;
}

bool holds(const Layout& layout, std::uint64_t index) noexcept {
    return std::any_of(layout.begin(), layout.end(), [&](const Run& run) {
        return run.placed() && index >= run.first && index - run.first < run.count;
    });
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

std::optional<Geometry> Geometry::of(FilesystemRecovery& volume) {
    const filesystem::FilesystemInfo& info = volume.filesystem().info();
    if (info.clusterSize == 0 || info.clusterCount == 0) {
        return std::nullopt;
    }
    Geometry geometry;
    geometry.volumeOffset = volume.volumeOffset();
    geometry.clusterSize = info.clusterSize;
    geometry.firstCluster = info.firstCluster;
    geometry.clusterCount = info.clusterCount;
    const std::optional<std::uint64_t> start = checkedAdd(volume.volumeOffset(), info.dataOffset);
    const std::optional<std::uint64_t> area = checkedMul(info.clusterCount, std::uint64_t{info.clusterSize});
    if (!start.has_value() || !area.has_value()) {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> end = checkedAdd(*start, *area);
    if (!end.has_value() || !checkedAdd(info.firstCluster, info.clusterCount).has_value()) {
        return std::nullopt;
    }
    geometry.dataStart = *start;
    geometry.areaEnd = *end;
    return geometry;
}

std::optional<std::uint64_t> Geometry::indexAt(std::uint64_t sourceOffset) const noexcept {
    if (sourceOffset < dataStart || sourceOffset >= areaEnd) {
        return std::nullopt;
    }
    return (sourceOffset - dataStart) / clusterSize;
}

bool Geometry::startsCluster(std::uint64_t sourceOffset) const noexcept {
    return sourceOffset >= dataStart && sourceOffset < areaEnd && (sourceOffset - dataStart) % clusterSize == 0;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> Geometry::clustersOf(std::uint64_t begin,
                                                                            std::uint64_t end) const noexcept {
    const std::uint64_t from = std::max(begin, dataStart);
    const std::uint64_t to = std::min(end, areaEnd);
    if (from >= to) {
        return std::nullopt;
    }
    return std::pair<std::uint64_t, std::uint64_t>((from - dataStart) / clusterSize,
                                                   (to - 1 - dataStart) / clusterSize + 1);
}

// ---------------------------------------------------------------------------
// Claims
// ---------------------------------------------------------------------------

void ClaimMap::split(std::uint64_t at) {
    auto after = segments_.upper_bound(at);
    if (after == segments_.begin()) {
        return;
    }
    const auto holder = std::prev(after);
    if (holder->first < at && at < holder->second.end) {
        Segment tail = holder->second;
        holder->second.end = at;
        segments_.emplace_hint(after, at, tail);
    }
}

void ClaimMap::add(std::uint64_t begin, std::uint64_t end, OwnerId owner) {
    if (begin >= end) {
        return;
    }
    split(begin);
    split(end);
    std::uint64_t position = begin;
    auto it = segments_.lower_bound(begin);
    while (position < end) {
        if (it == segments_.end() || it->first > position) {
            const std::uint64_t gapEnd = it == segments_.end() ? end : std::min(end, it->first);
            segments_.emplace_hint(it, position, Segment{gapEnd, owner, false});
            position = gapEnd;
            continue;
        }
        Segment& segment = it->second;
        if (segment.owner != owner) {
            segment.several = true;
        }
        position = segment.end;
        ++it;
    }
}

bool ClaimMap::claimedByOthers(std::uint64_t index, OwnerId self) const {
    auto after = segments_.upper_bound(index);
    if (after == segments_.begin()) {
        return false;
    }
    const auto holder = std::prev(after);
    if (index >= holder->second.end) {
        return false;
    }
    return holder->second.several || holder->second.owner != self;
}

// ---------------------------------------------------------------------------
// One volume
// ---------------------------------------------------------------------------

VolumeEvidence::VolumeEvidence(FilesystemRecovery& recovery, const CandidateScan& scan, Geometry geometry)
    : recovery_(&recovery), scan_(&scan), geometry_(geometry) {}

filesystem::ClusterState VolumeEvidence::state(std::uint64_t index) {
    if (index >= geometry_.clusterCount) {
        return filesystem::ClusterState::Invalid;
    }
    const std::lock_guard lock(cacheMutex_);
    const std::uint64_t pageNumber = index >> kPageBits;
    auto page = pages_.find(pageNumber);
    if (page == pages_.end()) {
        if (pages_.size() >= kMaxPages) {
            pages_.clear();
        }
        page = pages_.emplace(pageNumber, std::vector<std::uint8_t>(std::size_t{1} << kPageBits, kNotCached)).first;
    }
    std::uint8_t& cached = page->second[static_cast<std::size_t>(index & ((std::uint64_t{1} << kPageBits) - 1))];
    if (cached == kNotCached) {
        const Result<filesystem::ClusterState> read =
            recovery_->filesystem().clusterState(filesystem::ClusterNumber{geometry_.clusterNumber(index)});
        cached = static_cast<std::uint8_t>(read.ok() ? *read : filesystem::ClusterState::Unreadable);
    }
    return static_cast<filesystem::ClusterState>(cached);
}

bool VolumeEvidence::inUse(std::uint64_t index) {
    const filesystem::ClusterState now = state(index);
    return now != filesystem::ClusterState::Free && now != filesystem::ClusterState::Unreadable;
}

void VolumeEvidence::claimRegions(ClaimMap& map, const std::vector<SourceRegion>& regions, OwnerId owner) {
    for (const SourceRegion& region : regions) {
        if (region.kind != RegionKind::Stored || region.length == 0) {
            continue;
        }
        const std::uint64_t end = checkedAdd(region.sourceOffset, region.length).value_or(geometry_.areaEnd);
        if (const auto clusters = geometry_.clustersOf(region.sourceOffset, end); clusters.has_value()) {
            map.add(clusters->first, clusters->second, owner);
        }
    }
}

void VolumeEvidence::claimLayout(ClaimMap& map, const Layout& layout, OwnerId owner) {
    for (const Run& run : layout) {
        if (run.placed()) {
            map.add(run.first, run.first + run.count, owner);
        }
    }
}

// ---------------------------------------------------------------------------
// Continuations
// ---------------------------------------------------------------------------

Result<Continuations> continuationsAfter(VolumeEvidence& volume, const ContinuationQuery& query,
                                         const CancellationToken& cancel) {
    Continuations found;
    const std::uint64_t count = volume.geometry().clusterCount;
    if (count < 2 || query.maxCandidates == 0) {
        return found;
    }
    // The layout's clusters (disjoint runs), sorted for lookups.
    std::vector<Run> held;
    if (query.held != nullptr) {
        for (const Run& run : *query.held) {
            if (run.placed()) {
                held.push_back(run);
            }
        }
    }
    std::sort(held.begin(), held.end());
    const auto isHeld = [&](std::uint64_t index) {
        const auto after = std::upper_bound(held.begin(), held.end(), index,
                                            [](std::uint64_t value, const Run& run) { return value < run.first; });
        return after != held.begin() && index - std::prev(after)->first < std::prev(after)->count;
    };
    const auto candidate = [&](std::uint64_t index) {
        if (isHeld(index) || volume.inUse(index)) {
            return false;
        }
        return !volume.strong().claimedByOthers(index, query.self);
    };
    const auto softClaimed = [&](std::uint64_t index) { return volume.soft().claimedByOthers(index, query.self); };

    std::optional<std::uint64_t> firstClean;
    std::vector<std::uint64_t> runStarts;
    std::vector<std::uint64_t> all;
    const std::uint64_t span = count - 1;  // every cluster but `after`
    const std::uint64_t scan = std::min(span, query.maxScan);
    if (scan < span) {
        found.limit = "maxSearchClusters";
    }
    bool previousUsable = false;  // for the cluster before the first one looked at: `after`, which the layout holds
    for (std::uint64_t step = 1; step <= scan; ++step) {
        if ((step & 0xFFF) == 0 && cancel.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "fragment reconstruction cancelled");
        }
        const std::uint64_t index = (query.after + step) % count;
        if (index == 0) {
            previousUsable = false;  // the area's first cluster starts a run
        }
        const bool usable = candidate(index);
        if (usable) {
            const bool soft = softClaimed(index);
            if (!soft && !firstClean.has_value()) {
                firstClean = index;
            }
            if (!soft && !previousUsable && runStarts.size() < query.maxCandidates) {
                runStarts.push_back(index);
            }
            if (all.size() < query.maxCandidates) {
                all.push_back(index);
            }
            previousUsable = !soft;
        } else {
            previousUsable = false;
        }
        if (firstClean.has_value() && runStarts.size() >= query.maxCandidates &&
            all.size() >= query.maxCandidates) {
            found.limit = "maxContinuations";
            break;
        }
    }
    const auto add = [&](std::uint64_t index) {
        if (found.clusters.size() < query.maxCandidates &&
            std::find(found.clusters.begin(), found.clusters.end(), index) == found.clusters.end()) {
            found.clusters.push_back(index);
        }
    };
    if (firstClean.has_value()) {
        add(*firstClean);
    }
    for (const std::uint64_t index : runStarts) {
        add(index);
    }
    for (const std::uint64_t index : all) {
        add(index);
    }
    if (found.limit.empty() && all.size() >= query.maxCandidates) {
        found.limit = "maxContinuations";
    }
    return found;
}

// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------

std::vector<SourceRegion> regionsOf(VolumeEvidence& volume, const Layout& layout, std::uint64_t size,
                                    std::uint64_t placedLimit, bool markInUse) {
    const Geometry& geometry = volume.geometry();
    const std::uint64_t cs = geometry.clusterSize;
    const std::uint64_t placedEnd = std::min(size, placedLimit);
    std::vector<SourceRegion> regions;
    std::uint64_t position = 0;  // file offset
    for (const Run& run : layout) {
        if (position >= placedEnd) {
            break;
        }
        const std::uint64_t runBytes = checkedMul(run.count, cs).value_or(size);
        const std::uint64_t runEnd = std::min(placedEnd, position + std::min(runBytes, size - position));
        // Clusters beyond the area hold nothing of the volume's: Missing.
        const bool inside = run.placed() && run.first < geometry.clusterCount;
        const std::uint64_t inArea = inside ? std::min(run.count, geometry.clusterCount - run.first) : 0;
        const std::uint64_t storedEnd = position + std::min(inArea * cs, runEnd - position);
        if (!markInUse && storedEnd > position) {
            appendRegion(regions, SourceRegion{position, storedEnd - position, RegionKind::Stored,
                                               geometry.offsetOf(run.first), false});
            position = storedEnd;
        }
        for (std::uint64_t i = 0; position < storedEnd; ++i) {
            const std::uint64_t index = run.first + i;
            const std::uint64_t take = std::min(cs, storedEnd - position);
            appendRegion(regions, SourceRegion{position, take, RegionKind::Stored, geometry.offsetOf(index),
                                               volume.inUse(index)});
            position += take;
        }
        if (position < runEnd) {
            appendRegion(regions, SourceRegion{position, runEnd - position, RegionKind::Missing, 0, false});
            position = runEnd;
        }
    }
    if (position < size) {
        appendRegion(regions, SourceRegion{position, size - position, RegionKind::Missing, 0, false});
    }
    return regions;
}

}  // namespace recovery::fragments
