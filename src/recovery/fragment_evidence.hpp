#pragma once

// Private to recovery_fragments: the evidence fragment reconstruction works
// with. Clusters are identified by their index in a volume's cluster area
// (0-based); FragmentCandidate reports them as the volume numbers them.

#include "recovery/cancellation.hpp"
#include "recovery/filesystem_recovery.hpp"
#include "recovery/recovery_candidate.hpp"
#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace recovery::fragments {

// ---------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------

// Run::first of file clusters that no cluster holds (Missing).
inline constexpr std::uint64_t kUnplaced = std::numeric_limits<std::uint64_t>::max();

// `count` file clusters, stored from cluster index `first` on (or unplaced).
struct Run {
    std::uint64_t first = 0;
    std::uint64_t count = 0;

    [[nodiscard]] bool placed() const noexcept { return first != kUnplaced; }
    friend bool operator==(const Run&, const Run&) = default;
    friend auto operator<=>(const Run&, const Run&) = default;
};

// A file's clusters in file order.
using Layout = std::vector<Run>;

// Appends `run`, merging it into the last run when it continues it.
void appendRun(Layout& layout, Run run);
[[nodiscard]] std::uint64_t clustersIn(const Layout& layout) noexcept;
// The first `clusters` file clusters of the layout.
[[nodiscard]] Layout prefixOf(const Layout& layout, std::uint64_t clusters);
// The cluster holding file cluster `fileCluster`; nullopt when it is unplaced
// or beyond the layout.
[[nodiscard]] std::optional<std::uint64_t> clusterAt(const Layout& layout, std::uint64_t fileCluster) noexcept;
// Placed runs that do not continue the placed run before them.
[[nodiscard]] std::size_t fragmentsOf(const Layout& layout) noexcept;
// Whether the layout holds cluster `index`.
[[nodiscard]] bool holds(const Layout& layout, std::uint64_t index) noexcept;

// ---------------------------------------------------------------------------
// A volume's cluster area
// ---------------------------------------------------------------------------

struct Geometry {
    std::uint64_t volumeOffset = 0;
    // Source offset of cluster index 0, and of the end of the area.
    std::uint64_t dataStart = 0;
    std::uint64_t areaEnd = 0;
    std::uint64_t clusterSize = 0;
    // Cluster number of index 0.
    std::uint64_t firstCluster = 0;
    std::uint64_t clusterCount = 0;

    // Nothing when the volume has no clusters or its area does not fit the
    // 64-bit offset range.
    [[nodiscard]] static std::optional<Geometry> of(FilesystemRecovery& volume);

    [[nodiscard]] std::uint64_t offsetOf(std::uint64_t index) const noexcept { return dataStart + index * clusterSize; }
    // The cluster holding source offset `sourceOffset`, if the area has one there.
    [[nodiscard]] std::optional<std::uint64_t> indexAt(std::uint64_t sourceOffset) const noexcept;
    [[nodiscard]] bool startsCluster(std::uint64_t sourceOffset) const noexcept;
    [[nodiscard]] std::uint64_t clusterNumber(std::uint64_t index) const noexcept { return firstCluster + index; }
    // The clusters holding source bytes [begin, end), clipped to the area.
    [[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>> clustersOf(std::uint64_t begin,
                                                                                   std::uint64_t end) const noexcept;
};

// ---------------------------------------------------------------------------
// Claims: what other files' evidence says about clusters
// ---------------------------------------------------------------------------

// Identifies whoever holds a claim: a filesystem candidate, a carve, a seed.
using OwnerId = std::uint64_t;

// Clusters claimed by owners, as disjoint segments.
class ClaimMap {
public:
    // Claims clusters [begin, end) for `owner`.
    void add(std::uint64_t begin, std::uint64_t end, OwnerId owner);
    // Claimed by an owner other than `self`.
    [[nodiscard]] bool claimedByOthers(std::uint64_t index, OwnerId self) const;
    [[nodiscard]] std::size_t segments() const noexcept { return segments_.size(); }

private:
    struct Segment {
        std::uint64_t end = 0;
        // The owner when there is one; `several` once two owners claim it.
        OwnerId owner = 0;
        bool several = false;
    };

    // Splits the segment that holds `at` strictly inside it.
    void split(std::uint64_t at);

    std::map<std::uint64_t, Segment> segments_;  // by first cluster
};

// ---------------------------------------------------------------------------
// One volume
// ---------------------------------------------------------------------------

class VolumeEvidence {
public:
    VolumeEvidence(FilesystemRecovery& recovery, const CandidateScan& scan, Geometry geometry);

    [[nodiscard]] FilesystemRecovery& recovery() noexcept { return *recovery_; }
    [[nodiscard]] const CandidateScan& scan() const noexcept { return *scan_; }
    [[nodiscard]] const Geometry& geometry() const noexcept { return geometry_; }

    // The cluster's state in the allocation now; a filesystem error reads as
    // Unreadable (unknown). Safe to call from several threads at once: the
    // cache and the filesystem are used under a lock.
    [[nodiscard]] filesystem::ClusterState state(std::uint64_t index);
    // Allocated to data, bad or invalid: P7's rule for reallocated clusters.
    [[nodiscard]] bool inUse(std::uint64_t index);

    // Strong claims: active files' data, deleted files' recorded layouts and
    // first clusters, carves (their first cluster, or all of a carve that
    // validated), reconstructions settled in the run. Soft claims: what other
    // files' guesses and broken carves take (the rest of a guessed layout, the
    // consistent part of a broken carve).
    [[nodiscard]] ClaimMap& strong() noexcept { return strong_; }
    [[nodiscard]] ClaimMap& soft() noexcept { return soft_; }
    [[nodiscard]] const ClaimMap& strong() const noexcept { return strong_; }
    [[nodiscard]] const ClaimMap& soft() const noexcept { return soft_; }

    // Claims the clusters holding the stored regions of `regions`.
    void claimRegions(ClaimMap& map, const std::vector<SourceRegion>& regions, OwnerId owner);
    // Claims the clusters of a layout's placed runs.
    void claimLayout(ClaimMap& map, const Layout& layout, OwnerId owner);

private:
    static constexpr std::size_t kPageBits = 16;
    static constexpr std::size_t kMaxPages = 256;  // 16 MiB of cached states

    FilesystemRecovery* recovery_;
    const CandidateScan* scan_;
    Geometry geometry_;
    ClaimMap strong_;
    ClaimMap soft_;
    std::mutex cacheMutex_;
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> pages_;
};

// ---------------------------------------------------------------------------
// Where the next fragment may start
// ---------------------------------------------------------------------------

struct ContinuationQuery {
    // The last cluster of the fragment before.
    std::uint64_t after = 0;
    // The clusters the layout already holds (never offered again).
    const Layout* held = nullptr;
    // Whose claims do not count.
    OwnerId self = 0;
    // Candidates returned, and clusters looked at, at most.
    std::size_t maxCandidates = 0;
    std::uint64_t maxScan = 0;
};

struct Continuations {
    std::vector<std::uint64_t> clusters;
    // The limit that ended the look before every candidate was found
    // ("maxSearchClusters" or "maxContinuations"), or empty.
    std::string_view limit;
};

// The clusters that could start the next fragment, best first: a cluster is
// a candidate when it is free now (or its state is unknown), no other file
// claims it strongly, and the layout does not hold it. They are looked for
// from `after` on, in the order the file's writer would have met them
// (wrapping around at the end of the area): first the next candidate that no
// other file claims at all, then candidates that start a free run (the
// cluster before is in use, claimed or held), then every candidate.
[[nodiscard]] Result<Continuations> continuationsAfter(VolumeEvidence& volume, const ContinuationQuery& query,
                                                       const CancellationToken& cancel);

// ---------------------------------------------------------------------------
// Layouts as source regions
// ---------------------------------------------------------------------------

// File bytes [0, size) under `layout`: placed clusters are Stored, unplaced
// clusters and whatever lies beyond the layout or beyond `placedLimit` are
// Missing. With `markInUse`, stored bytes in clusters in use now are split
// into regions marked reallocated.
[[nodiscard]] std::vector<SourceRegion> regionsOf(VolumeEvidence& volume, const Layout& layout, std::uint64_t size,
                                                  std::uint64_t placedLimit, bool markInUse);

}  // namespace recovery::fragments
