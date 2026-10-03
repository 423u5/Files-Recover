#include "fragment_search.hpp"

#include "recovery/checked_math.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace recovery::fragments {

namespace {

using carving::ValidationStatus;

int statusRank(ValidationStatus status) noexcept {
    switch (status) {
    case ValidationStatus::Valid:
        return 2;
    case ValidationStatus::Truncated:
        return 1;
    case ValidationStatus::Invalid:
    case ValidationStatus::NotValidated:
        break;
    }
    return 0;
}

LayoutSource sourceOf(Policy policy) noexcept {
    switch (policy) {
    case Policy::Contiguous:
        return LayoutSource::Contiguous;
    case Policy::SkipAllocated:
        return LayoutSource::SkipAllocated;
    case Policy::SkipClaimed:
        return LayoutSource::SkipClaimed;
    }
    return LayoutSource::Contiguous;
}

// 1 when `a` ranks before `b`, -1 after, 0 when they are equally supported.
int compare(Search& search, const Hypothesis& a, const Hypothesis& b) {
    const bool validA = a.assessment.status == ValidationStatus::Valid;
    const bool validB = b.assessment.status == ValidationStatus::Valid;
    if (validA != validB) {
        return validA ? 1 : -1;
    }
    if (!validA) {
        // A structure that ends before the recorded size lost clusters on the way.
        if (a.assessment.endedEarly != b.assessment.endedEarly) {
            return b.assessment.endedEarly ? 1 : -1;
        }
        if (a.confirmed != b.confirmed) {
            return a.confirmed > b.confirmed ? 1 : -1;
        }
        const int ra = statusRank(a.assessment.status);
        const int rb = statusRank(b.assessment.status);
        if (ra != rb) {
            return ra > rb ? 1 : -1;
        }
    }
    const HypothesisEvidence& ea = search.evidenceOf(a);
    const HypothesisEvidence& eb = search.evidenceOf(b);
    if (ea.allocatedClusters != eb.allocatedClusters) {
        return ea.allocatedClusters < eb.allocatedClusters ? 1 : -1;
    }
    if (ea.claimedClusters != eb.claimedClusters) {
        return ea.claimedClusters < eb.claimedClusters ? 1 : -1;
    }
    if (ea.fragments != eb.fragments) {
        return ea.fragments < eb.fragments ? 1 : -1;
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// PrefixReader
// ---------------------------------------------------------------------------

PrefixReader::PrefixReader(carving::IContentReader& inner, std::uint64_t size) noexcept
    : inner_(&inner), size_(std::min(size, inner.size())) {}

Result<std::span<const std::byte>> PrefixReader::read(std::uint64_t offset, std::size_t length) {
    if (!rangeWithin<std::uint64_t>(offset, length, size_)) {
        return makeError(ErrorCode::InvalidInput, "content read [" + std::to_string(offset) + ", +" +
                                                      std::to_string(length) + ") lies beyond the " +
                                                      std::to_string(size_) + " bytes of the file");
    }
    return inner_->read(offset, length);
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

Search::Search(storage::IStorageSource& source, const FragmentRecoveryOptions& options, VolumeEvidence& volume,
               const Seed& seed, const std::vector<std::uint64_t>& moovAnchors)
    : source_(&source), options_(&options), volume_(&volume), seed_(&seed), moovAnchors_(&moovAnchors) {
    const std::uint64_t cs = volume.geometry().clusterSize;
    if (seed.size.has_value()) {
        window_ = *seed.size;
    } else {
        window_ = std::min(seed.format->descriptor().maximumSize, options.limits.maxUnknownLength);
    }
    window_ = std::max<std::uint64_t>(window_, 1);
    target_ = window_ / cs + (window_ % cs != 0 ? 1 : 0);
}

std::uint64_t Search::contentLength() const noexcept {
    return window_;
}

void Search::limitReached(std::string_view limit) {
    if (stats_.complete) {
        stats_.complete = false;
        stats_.limit = std::string(limit);
    }
}

Status Search::checkCancelled() const {
    if (options_->carving.scan.reads.cancellation.isCancellationRequested()) {
        return makeError(ErrorCode::Cancelled, "fragment reconstruction cancelled");
    }
    return success();
}

Result<Layout> Search::extend(Layout layout, std::uint64_t from, Policy policy) {
    const std::uint64_t count = volume_->geometry().clusterCount;
    std::uint64_t have = clustersIn(layout);
    if (have >= target_ || from >= count) {
        return layout;
    }
    // The clusters the layout holds so far (disjoint runs), sorted, for the
    // new clusters to avoid.
    std::vector<Run> held;
    for (const Run& run : layout) {
        if (run.placed()) {
            held.push_back(run);
        }
    }
    std::sort(held.begin(), held.end());
    // The first held run that ends after `index`, or held.end().
    const auto heldAfter = [&](std::uint64_t index) {
        return std::upper_bound(held.begin(), held.end(), index,
                                [](std::uint64_t value, const Run& run) { return value < run.first + run.count; });
    };

    if (policy == Policy::Contiguous) {
        const auto blocking = heldAfter(from);
        const std::uint64_t stop = blocking == held.end() ? count : std::max(blocking->first, from);
        const std::uint64_t take = std::min(target_ - have, stop - from);
        appendRun(layout, Run{from, take});
        return layout;
    }
    std::uint64_t index = from;
    std::uint64_t steps = 0;
    while (have < target_ && index < count) {
        if ((++steps & 0xFFFF) == 0) {
            if (Status cancelled = checkCancelled(); !cancelled.ok()) {
                return cancelled.error();
            }
        }
        if (const auto blocking = heldAfter(index); blocking != held.end() && blocking->first <= index) {
            index = blocking->first + blocking->count;  // skip the held run
            continue;
        }
        bool skip = false;
        if (index != from) {
            skip = volume_->inUse(index);
            if (!skip && policy == Policy::SkipClaimed) {
                skip = volume_->strong().claimedByOthers(index, seed_->owner);
            }
        }
        if (!skip) {
            appendRun(layout, Run{index, 1});
            ++have;
        }
        ++index;
    }
    return layout;
}

Result<std::unique_ptr<CandidateContentReader>> Search::open(const Layout& layout, std::uint64_t size,
                                                             std::uint64_t placedLimit) {
    RecoveryCandidate candidate;
    candidate.expectedSize = size;
    candidate.sourceRegions = regionsOf(*volume_, layout, size, placedLimit, false);
    return CandidateContentReader::open(*source_, candidate, options_->carving.scan.reads,
                                        options_->carving.readCacheSize);
}

Result<std::optional<Assessment>> Search::assess(const Layout& layout) {
    const FragmentSearchLimits& limits = options_->limits;
    if (stats_.layoutsValidated >= limits.maxValidations) {
        limitReached("maxValidations");
        return std::optional<Assessment>{};
    }
    if (stats_.bytesRead >= limits.maxReadBytes) {
        limitReached("maxReadBytes");
        return std::optional<Assessment>{};
    }
    if (Status cancelled = checkCancelled(); !cancelled.ok()) {
        return cancelled.error();
    }
    Result<std::unique_ptr<CandidateContentReader>> opened = open(layout, window_, window_);
    if (!opened.ok()) {
        return opened.error();
    }
    CandidateContentReader& reader = **opened;
    const carving::IFileFormat& format = *seed_->format;
    ++stats_.layoutsValidated;

    // A format that fails with an error of its own (not cancellation, not the
    // source) did not find the layout consistent.
    const auto failed = [&](const Error& error) -> Result<std::optional<Assessment>> {
        stats_.bytesRead += reader.bytesRead();
        if (error.code == ErrorCode::Cancelled) {
            return error;
        }
        if (reader.sourceFailure().has_value()) {
            return *reader.sourceFailure();
        }
        return std::optional<Assessment>(
            Assessment{ValidationStatus::Invalid, 0, window_, "the format failed: " + error.message});
    };

    // Where the structure ends first (the format's end detection), then the
    // format's validation of the file it delimits. A recorded size fixes the
    // file's length: a structure that ends before it skipped part of the file.
    const bool sized = seed_->size.has_value();
    Assessment assessment;
    if (reader.size() < format.descriptor().minimumSize) {
        assessment = Assessment{ValidationStatus::Truncated, reader.size(), sized ? window_ : reader.size(),
                                "the layout holds fewer bytes than the format's smallest file"};
    } else {
        Result<carving::EndDetection> end = format.findEnd(reader);
        if (!end.ok()) {
            return failed(end.error());
        }
        const std::uint64_t length = sized ? window_ : std::min(end->length, reader.size());
        switch (end->status) {
        case carving::EndStatus::Found: {
            if (end->length == 0 || end->length > reader.size()) {
                assessment = Assessment{ValidationStatus::Invalid, 0, sized ? window_ : reader.size(),
                                        "the format ended the file outside its data"};
                break;
            }
            PrefixReader file(reader, end->length);
            Result<carving::ValidationResult> verdict = format.validator().validate(file);
            if (!verdict.ok()) {
                return failed(verdict.error());
            }
            assessment = Assessment{verdict->status, std::min(verdict->validBytes, end->length), length,
                                    std::move(verdict->detail)};
            if (sized && end->length < window_) {
                assessment.endedEarly = true;
                if (assessment.status == ValidationStatus::Valid) {
                    assessment.status = ValidationStatus::Invalid;
                }
                assessment.detail += "; the structure ends at " + std::to_string(end->length) +
                                     ", before the recorded size of " + std::to_string(window_);
            }
            break;
        }
        case carving::EndStatus::Broken:
            assessment = Assessment{ValidationStatus::Invalid, std::min(end->length, reader.size()), length,
                                    std::move(end->detail)};
            break;
        case carving::EndStatus::Truncated:
        case carving::EndStatus::Unknown:
            assessment = Assessment{ValidationStatus::Truncated, reader.size(), length, std::move(end->detail)};
            if (sized && reader.size() == window_) {
                // Every byte of the recorded size is there and the structure wants more: it ran past
                // the file where the layout stops being it (a garbage length), which the format's
                // validation locates.
                Result<carving::ValidationResult> verdict = format.validator().validate(reader);
                if (!verdict.ok()) {
                    return failed(verdict.error());
                }
                assessment.status = ValidationStatus::Invalid;
                assessment.progress = std::min(verdict->validBytes, reader.size());
                assessment.detail += "; the structure runs past the recorded size of " + std::to_string(window_);
            }
            break;
        }
    }
    stats_.bytesRead += reader.bytesRead();
    return std::optional<Assessment>(std::move(assessment));
}

std::optional<std::size_t> Search::admit(Hypothesis hypothesis) {
    if (!seen_.insert(hypothesis.layout).second) {
        return std::nullopt;
    }
    hypothesis.order = nextOrder_++;
    pool_.push_back(std::move(hypothesis));
    return pool_.size() - 1;
}

const HypothesisEvidence& Search::evidenceOf(const Hypothesis& hypothesis) {
    if (hypothesis.evidence.has_value()) {
        return *hypothesis.evidence;
    }
    HypothesisEvidence evidence;
    const std::uint64_t cs = clusterSize();
    const std::uint64_t delivered = hypothesis.deliveredLength();
    const std::uint64_t clusters = delivered / cs + (delivered % cs != 0 ? 1 : 0);
    const Layout part = prefixOf(hypothesis.layout, clusters);
    evidence.fragments = fragmentsOf(part);
    for (const Run& run : part) {
        if (!run.placed()) {
            continue;
        }
        for (std::uint64_t i = 0; i < run.count; ++i) {
            const std::uint64_t index = run.first + i;
            ++evidence.clusters;
            if (volume_->inUse(index)) {
                ++evidence.allocatedClusters;
            }
            if (volume_->strong().claimedByOthers(index, seed_->owner)) {
                ++evidence.claimedClusters;
            }
        }
    }
    const std::uint64_t length = std::max(hypothesis.assessment.length, delivered);
    for (const SourceRegion& region : regionsOf(*volume_, hypothesis.layout, length, delivered, false)) {
        if (region.kind == RegionKind::Stored) {
            evidence.placedBytes += region.length;
        } else {
            evidence.missingBytes += region.length;
        }
    }
    hypothesis.evidence = evidence;
    return *hypothesis.evidence;
}

Status Search::baseLayouts() {
    for (const Policy policy : {Policy::Contiguous, Policy::SkipAllocated, Policy::SkipClaimed}) {
        Result<Layout> layout = extend({}, seed_->start, policy);
        if (!layout.ok()) {
            return layout.error();
        }
        if (validated(*layout)) {
            continue;
        }
        Result<std::optional<Assessment>> assessment = assess(*layout);
        if (!assessment.ok()) {
            return assessment.error();
        }
        if (!assessment->has_value()) {
            return success();
        }
        Hypothesis hypothesis;
        hypothesis.layout = std::move(*layout);
        hypothesis.source = sourceOf(policy);
        hypothesis.policy = policy;
        hypothesis.fixed = 1;
        hypothesis.assessment = std::move(**assessment);
        const std::uint64_t progress = hypothesis.assessment.progress;
        const std::uint64_t cs = clusterSize();
        hypothesis.confirmed = progress >= cs ? progress / cs * cs : progress;
        (void)admit(std::move(hypothesis));
    }
    return success();
}

Status Search::gapSearch() {
    const FragmentSearchLimits& limits = options_->limits;
    const std::uint64_t cs = clusterSize();
    // The layouts to continue from: those that broke after some consistent bytes.
    std::vector<std::size_t> frontier;
    for (std::size_t i = 0; i < pool_.size(); ++i) {
        const Assessment& assessment = pool_[i].assessment;
        if (assessment.status != ValidationStatus::Valid && !assessment.endedEarly && assessment.progress > 0) {
            frontier.push_back(i);
        }
    }
    const auto keepBest = [&](std::vector<std::size_t>& indices) {
        std::stable_sort(indices.begin(), indices.end(),
                         [&](std::size_t a, std::size_t b) { return compare(*this, pool_[a], pool_[b]) > 0; });
        if (indices.size() > limits.beamWidth) {
            indices.resize(limits.beamWidth);
        }
    };
    keepBest(frontier);

    while (!frontier.empty() && !limited()) {
        std::vector<std::size_t> next;
        for (const std::size_t index : frontier) {
            // pool_ grows below: work on a copy of the parent.
            const Hypothesis parent = pool_[index];
            bool solved = false;
            if (fragmentsOf(parent.layout) >= limits.maxFragments) {
                limitReached("maxFragments");
                continue;
            }
            const std::uint64_t broken = parent.assessment.progress;
            const std::uint64_t clusters = clustersIn(parent.layout);
            // Where the fragment may end: the cluster boundaries nearest the break,
            // the one at the start of the cluster holding it first.
            std::vector<std::uint64_t> boundaries;
            const std::uint64_t at = broken / cs;
            const auto consider = [&](std::uint64_t k) {
                if (k >= parent.fixed && k < clusters && boundaries.size() < limits.maxBoundaries) {
                    boundaries.push_back(k);
                }
            };
            consider(at);
            for (std::uint64_t distance = 1; boundaries.size() < limits.maxBoundaries; ++distance) {
                const bool before = distance <= at && at - distance >= parent.fixed;
                const bool after = at + distance < clusters;
                if (!before && !after) {
                    break;
                }
                if (before) {
                    consider(at - distance);
                }
                if (after) {
                    consider(at + distance);
                }
            }
            for (const std::uint64_t k : boundaries) {
                const Layout prefix = prefixOf(parent.layout, k);
                const std::optional<std::uint64_t> last = clusterAt(prefix, k - 1);
                if (!last.has_value()) {
                    continue;
                }
                const std::optional<std::uint64_t> own = clusterAt(parent.layout, k);
                ContinuationQuery query;
                query.after = *last;
                query.held = &prefix;
                query.self = seed_->owner;
                query.maxCandidates = limits.maxContinuations;
                query.maxScan = limits.maxSearchClusters;
                Result<Continuations> continuations =
                    continuationsAfter(*volume_, query, options_->carving.scan.reads.cancellation);
                if (!continuations.ok()) {
                    return continuations.error();
                }
                if (!continuations->limit.empty()) {
                    candidatesCut(continuations->limit);
                }
                for (const std::uint64_t start : continuations->clusters) {
                    if (own == start) {
                        continue;  // the parent itself
                    }
                    Result<Layout> child = extend(prefix, start, parent.policy);
                    if (!child.ok()) {
                        return child.error();
                    }
                    if (validated(*child)) {
                        continue;
                    }
                    Result<std::optional<Assessment>> assessment = assess(*child);
                    if (!assessment.ok()) {
                        return assessment.error();
                    }
                    if (!assessment->has_value()) {
                        return success();
                    }
                    Hypothesis hypothesis;
                    hypothesis.layout = std::move(*child);
                    hypothesis.source = LayoutSource::GapSearch;
                    hypothesis.policy = parent.policy;
                    hypothesis.fixed = k + 1;
                    hypothesis.assessment = std::move(**assessment);
                    // The continuation confirms the file up to its boundary once it held long enough.
                    const std::uint64_t boundary = k * cs;
                    const bool held = hypothesis.assessment.progress >= boundary &&
                                      hypothesis.assessment.progress - boundary >= limits.minimumContinuation;
                    hypothesis.confirmed = held ? boundary : std::min(boundary, parent.confirmed);
                    const bool valid = hypothesis.assessment.status == ValidationStatus::Valid;
                    // Worth continuing: it held past its boundary and got past the parent's break, and
                    // its structure did not end early.
                    const bool progressed = !valid && held && !hypothesis.assessment.endedEarly &&
                                            hypothesis.assessment.progress > broken;
                    const std::optional<std::size_t> admitted = admit(std::move(hypothesis));
                    if (admitted.has_value() && progressed) {
                        next.push_back(*admitted);
                    }
                    solved = solved || (admitted.has_value() && valid);
                }
            }
            if (solved) {
                // The file validates: its alternatives at this layout's boundaries were all tried.
                return success();
            }
        }
        keepBest(next);
        frontier = std::move(next);
    }
    return success();
}

Status Search::run() {
    if (seed_->unrecoverable.has_value()) {
        return success();
    }
    if (Status base = baseLayouts(); !base.ok()) {
        return base;
    }
    const auto anyValid = [&] {
        return std::any_of(pool_.begin(), pool_.end(), [](const Hypothesis& h) {
            return h.assessment.status == ValidationStatus::Valid;
        });
    };
    // MP4 and M4A: once a movie is known, its samples place the file (the
    // generic search, which validates whole layouts, is left out).
    bool movie = false;
    if (!anyValid() && isIsoFormat(*seed_->format) && !limited()) {
        Result<bool> placed = placeBySampleTables(*this);
        if (!placed.ok()) {
            return placed.error();
        }
        movie = *placed;
    }
    if (!anyValid() && !movie && !limited()) {
        if (Status searched = gapSearch(); !searched.ok()) {
            return searched;
        }
    }
    if (!anyValid() && !cut_.empty()) {
        limitReached(cut_);
    }
    return success();
}

// ---------------------------------------------------------------------------
// Ranking
// ---------------------------------------------------------------------------

bool ranksBefore(Search& search, const Hypothesis& a, const Hypothesis& b) {
    return compare(search, a, b) > 0;
}

bool equallySupported(Search& search, const Hypothesis& a, const Hypothesis& b) {
    return compare(search, a, b) == 0;
}

void rank(Search& search, std::vector<Hypothesis>& pool) {
    std::stable_sort(pool.begin(), pool.end(),
                     [&](const Hypothesis& a, const Hypothesis& b) { return compare(search, a, b) > 0; });
}

}  // namespace recovery::fragments
