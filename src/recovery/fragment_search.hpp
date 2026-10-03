#pragma once

// Private to recovery_fragments: the search for one seed's layout. The
// generic part (fragment_search.cpp) validates whole layouts with the format's
// validator: the clusters after the start, those not in use, those not
// claimed, and continuations found where the structure breaks. MP4 and M4A
// files add layouts placed by their sample tables (fragment_mp4.cpp).

#include "carving/content_reader.hpp"
#include "carving/file_candidate.hpp"
#include "carving/file_format.hpp"
#include "fragment_evidence.hpp"
#include "recovery/candidate_content.hpp"
#include "recovery/fragment_recovery.hpp"
#include "recovery/result.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace recovery::fragments {

// A file whose start is known and whose layout is not.
struct Seed {
    SeedOrigin origin = SeedOrigin::Filesystem;
    std::size_t volume = 0;
    const carving::IFileFormat* format = nullptr;
    // The cluster index of its first cluster.
    std::uint64_t start = 0;
    // Its length when the metadata records it.
    std::optional<std::uint64_t> size;
    // Its own claims (its first cluster, its guess, its carve) do not count against it.
    OwnerId owner = 0;
    // Filesystem seeds: the P7 candidate (in its scan, which outlives the run).
    const RecoveryCandidate* metadata = nullptr;
    // The carve that starts where the file does, if any.
    std::optional<carving::FileCandidate> carve;
    // Set when nothing can be reconstructed at all: why.
    std::optional<std::string> unrecoverable;
};

// How a layout continues beyond the clusters chosen for it.
enum class Policy : std::uint8_t { Contiguous, SkipAllocated, SkipClaimed };

// A layout's content as the format sees it.
struct Assessment {
    carving::ValidationStatus status = carving::ValidationStatus::Invalid;
    // Consistent bytes from the start.
    std::uint64_t progress = 0;
    // The file's length under the layout (the recorded size, or where the
    // structure ends for a carve).
    std::uint64_t length = 0;
    std::string detail;
    // The structure ends before the recorded size: the layout skipped part of
    // the file (or the file carries data after its structure). Continuing it
    // cannot help.
    bool endedEarly = false;
};

struct Hypothesis {
    // The file's clusters (all of them, or as many as the window holds).
    Layout layout;
    LayoutSource source = LayoutSource::Contiguous;
    Policy policy = Policy::Contiguous;
    // Children cut the layout after this many file clusters (it chose them).
    std::uint64_t fixed = 1;
    Assessment assessment;
    // The file bytes the evidence confirms, which a layout that does not
    // validate delivers: for a base layout, the clusters before the one where
    // its structure broke; for a continuation found by the gap search, the
    // file up to the boundary it starts at, once it held for
    // FragmentSearchLimits::minimumContinuation bytes (a structure can pass
    // over foreign data for a while: JPEG entropy-coded data, a segment whose
    // length foreign bytes give). MP4 layouts placed by the sample tables:
    // the bytes of the file they get right as far as the samples tell.
    std::uint64_t confirmed = 0;
    // MP4 layouts placed by the sample tables deliver the whole file, with
    // Missing where no fragment was found; the others deliver what the
    // evidence confirms.
    bool wholeFile = false;
    // In generation order.
    std::uint64_t order = 0;
    // The evidence over the delivered part, computed when needed (a cache:
    // the ranking compares const hypotheses).
    mutable std::optional<HypothesisEvidence> evidence;

    // The file bytes the hypothesis delivers.
    [[nodiscard]] std::uint64_t deliveredLength() const noexcept {
        return assessment.status == carving::ValidationStatus::Valid || wholeFile ? assessment.length : confirmed;
    }
};

// Reads the first `size` bytes of another reader.
class PrefixReader final : public carving::IContentReader {
public:
    PrefixReader(carving::IContentReader& inner, std::uint64_t size) noexcept;

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }
    [[nodiscard]] Result<std::span<const std::byte>> read(std::uint64_t offset, std::size_t length) override;

private:
    carving::IContentReader* inner_;
    std::uint64_t size_;
};

// One seed's search: every layout it validated, in `pool`.
class Search {
public:
    Search(storage::IStorageSource& source, const FragmentRecoveryOptions& options, VolumeEvidence& volume,
           const Seed& seed, const std::vector<std::uint64_t>& moovAnchors);

    // Base layouts, the MP4 placement for MP4 and M4A files, the gap search.
    [[nodiscard]] Status run();

    [[nodiscard]] std::vector<Hypothesis>& pool() noexcept { return pool_; }
    [[nodiscard]] SearchStats& stats() noexcept { return stats_; }
    [[nodiscard]] const Seed& seed() const noexcept { return *seed_; }
    [[nodiscard]] VolumeEvidence& volume() noexcept { return *volume_; }
    [[nodiscard]] storage::IStorageSource& source() noexcept { return *source_; }
    [[nodiscard]] const FragmentRecoveryOptions& options() const noexcept { return *options_; }
    [[nodiscard]] const std::vector<std::uint64_t>& moovAnchors() const noexcept { return *moovAnchors_; }
    [[nodiscard]] std::uint64_t clusterSize() const noexcept { return volume_->geometry().clusterSize; }

    // The file clusters a layout needs: the recorded size's, or the window's.
    [[nodiscard]] std::uint64_t targetClusters() const noexcept { return target_; }
    // The file's length when the metadata records it, or the window.
    [[nodiscard]] std::uint64_t contentLength() const noexcept;

    // `layout` continued from cluster `from` on, cluster after cluster as
    // `policy` says, to targetClusters() (fewer at the end of the area, or at
    // a cluster the layout already holds when contiguous).
    [[nodiscard]] Result<Layout> extend(Layout layout, std::uint64_t from, Policy policy);
    // The content of a layout, as the recovered file would read.
    [[nodiscard]] Result<std::unique_ptr<CandidateContentReader>> open(const Layout& layout, std::uint64_t size,
                                                                       std::uint64_t placedLimit);
    // The format's structure of the layout. Nothing when a limit stopped the
    // search (stats().complete is then false).
    [[nodiscard]] Result<std::optional<Assessment>> assess(const Layout& layout);
    // Adds a validated layout to the pool, unless the pool holds it already.
    // Returns its index in the pool, or nothing when it was there.
    std::optional<std::size_t> admit(Hypothesis hypothesis);
    [[nodiscard]] bool validated(const Layout& layout) const { return seen_.contains(layout); }

    // Records that a limit stopped the search.
    void limitReached(std::string_view limit);
    // Records a limit that left candidates unseen: it stopped the search if
    // the search ends without a valid layout.
    void candidatesCut(std::string_view limit) {
        if (cut_.empty()) {
            cut_ = limit;
        }
    }
    [[nodiscard]] bool limited() const noexcept { return !stats_.complete; }
    [[nodiscard]] Status checkCancelled() const;
    // Counts source bytes read outside assess().
    void addBytesRead(std::uint64_t bytes) noexcept { stats_.bytesRead += bytes; }

    // The evidence of a hypothesis over what it delivers (computed once).
    const HypothesisEvidence& evidenceOf(const Hypothesis& hypothesis);

private:
    [[nodiscard]] Status baseLayouts();
    [[nodiscard]] Status gapSearch();

    storage::IStorageSource* source_;
    const FragmentRecoveryOptions* options_;
    VolumeEvidence* volume_;
    const Seed* seed_;
    const std::vector<std::uint64_t>* moovAnchors_;
    std::uint64_t window_ = 0;
    std::uint64_t target_ = 0;
    std::vector<Hypothesis> pool_;
    std::set<Layout> seen_;
    SearchStats stats_;
    std::uint64_t nextOrder_ = 0;
    std::string_view cut_;
};

// Whether `a` ranks before `b`: the structure (Valid first; then a structure
// that does not end early, the bytes confirmed, Truncated before Invalid),
// then fewer clusters in use now, fewer claimed by other files, fewer
// fragments. Neither: they are equally supported.
[[nodiscard]] bool ranksBefore(Search& search, const Hypothesis& a, const Hypothesis& b);
[[nodiscard]] bool equallySupported(Search& search, const Hypothesis& a, const Hypothesis& b);
// Sorts the pool best first (stable: equally supported layouts keep their order).
void rank(Search& search, std::vector<Hypothesis>& pool);

// MP4 and M4A (fragment_mp4.cpp): layouts placed by the sample tables, with
// the moov read where the top-level boxes put it. The layouts validated before
// are confirmed only as far as their samples frame. False when no movie was
// found (nothing was placed).
[[nodiscard]] Result<bool> placeBySampleTables(Search& search);
[[nodiscard]] bool isIsoFormat(const carving::IFileFormat& format) noexcept;

}  // namespace recovery::fragments
