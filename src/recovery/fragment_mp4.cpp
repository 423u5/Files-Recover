// MP4 and M4A layouts placed by the sample tables (P13).
//
// The movie comes from the file's first fragment (moov before the media
// data), or, when the top-level boxes put it after the media data, from a
// moov box found where they put it: at the offset where the first mdat ends,
// in a cluster whose offset within it is that offset's, in free space (a moov
// "anchor"). The video samples of the movie are then walked in file order,
// a window at a time, through the layout: each one's NAL units must fill it
// (the rule of mp4::checkSampleFraming). Where a sample does not, the fragment
// ended before it: the cluster boundaries between the last sample that framed
// and this one are tried as the end of the fragment, and for each the
// clusters that could start the next one (the cluster that joins the moov's
// fragment first); a continuation is taken when the samples after the
// boundary frame through it. When none does, the layout is kept as it is if
// later samples frame through it again (the part between was damaged where
// it lies), or the samples are looked for in the moov's fragment further on
// (the part between is missing). Each finished walk is one layout, validated
// in full by the format like any other.

#include "fragment_search.hpp"

#include "formats/m4a_format.hpp"
#include "formats/mp4_analysis.hpp"
#include "formats/mp4_box.hpp"
#include "formats/mp4_format.hpp"
#include "formats/mp4_parser.hpp"
#include "recovery/checked_math.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <string>
#include <utility>

namespace recovery::fragments {

namespace {

namespace mp4 = formats::mp4;
using carving::ValidationStatus;

// Cache of the readers that probe samples: they read NAL unit headers.
constexpr std::size_t kProbeCacheSize = 64 * kKiB;
// Cluster boundaries tried where a sample breaks, at most (probes are cheap).
constexpr std::size_t kMinProbeBoundaries = 64;

// A video sample whose NAL units can be checked.
struct Checkpoint {
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
    std::uint8_t lengthSize = 0;

    [[nodiscard]] std::uint64_t end() const noexcept { return offset + size; }
};

// Where a movie comes from.
struct MovieSource {
    mp4::Movie movie;
    // The file's length: the recorded size, or where its structure ends.
    std::uint64_t length = 0;
    // Moov after the media data, found where the top-level boxes put it: its
    // file offset, the file cluster holding it and the cluster it was found in.
    bool anchored = false;
    std::uint64_t moovOffset = 0;
    std::uint64_t moovFileCluster = 0;
    std::uint64_t moovCluster = 0;
};

std::vector<Checkpoint> checkpointsOf(const mp4::Movie& movie) {
    std::vector<Checkpoint> points;
    for (const mp4::Track& track : movie.tracks) {
        if (track.kind != mp4::TrackKind::Video) {
            continue;
        }
        const std::vector<mp4::SampleDescription>& descriptions = track.samples.descriptions;
        const auto add = [&](const mp4::Chunk& chunk, bool run) {
            if (chunk.sampleDescriptionIndex == 0 || chunk.sampleDescriptionIndex > descriptions.size()) {
                return;
            }
            const std::uint8_t lengthSize = descriptions[chunk.sampleDescriptionIndex - 1].nalLengthSize;
            if (lengthSize == 0) {
                return;
            }
            std::uint64_t offset = chunk.offset;
            for (std::uint32_t i = 0; i < chunk.sampleCount; ++i) {
                const std::uint32_t index = chunk.firstSample + i;
                const std::uint32_t size = run ? track.fragmentSampleSizes[index] : track.samples.sampleSize(index);
                points.push_back(Checkpoint{offset, size, lengthSize});
                offset += size;
            }
        };
        for (const mp4::Chunk& chunk : track.chunks) {
            add(chunk, false);
        }
        for (const mp4::Chunk& run : track.fragmentRuns) {
            add(run, true);
        }
    }
    std::sort(points.begin(), points.end(),
              [](const Checkpoint& a, const Checkpoint& b) { return a.offset < b.offset; });
    return points;
}

// A window of checkpoints as a movie mp4::checkSampleFraming can walk: one
// video track, one chunk per sample.
mp4::Movie windowMovie(const std::vector<Checkpoint>& points, std::size_t first, std::size_t last) {
    mp4::Movie movie;
    mp4::Track track;
    track.kind = mp4::TrackKind::Video;
    track.sampleTableValid = true;
    for (const std::uint8_t size : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{4}}) {
        mp4::SampleDescription description;
        description.format = mp4::FourCc("avc1");
        description.nalLengthSize = size;
        track.samples.descriptions.push_back(description);
    }
    for (std::size_t i = first; i < last; ++i) {
        const Checkpoint& point = points[i];
        mp4::Chunk chunk;
        chunk.offset = point.offset;
        chunk.size = point.size;
        chunk.firstSample = static_cast<std::uint32_t>(i - first);
        chunk.sampleCount = 1;
        chunk.sampleDescriptionIndex = point.lengthSize == 1 ? 1U : point.lengthSize == 2 ? 2U : 3U;
        track.chunks.push_back(chunk);
        track.samples.sampleSizes.push_back(point.size);
    }
    track.samples.sampleCount = static_cast<std::uint32_t>(last - first);
    movie.tracks.push_back(std::move(track));
    return movie;
}

struct ProbeResult {
    // The first checkpoint of the window that does not frame (the window's end when all do).
    std::size_t firstBad = 0;
    std::uint64_t checked = 0;
};

// A walk through the samples: the layout so far and where it stands.
struct Walk {
    Layout layout;
    // The next checkpoint to probe.
    std::size_t next = 0;
    // File clusters before the last boundary chosen: later boundaries come after them.
    std::uint64_t fixed = 1;
    // File bytes placed over damage (samples that do not frame between samples that do).
    std::uint64_t damagedBytes = 0;
    // The walk joined the moov's fragment (anchored movies).
    bool joined = false;
};

class Placement {
public:
    Placement(Search& search, MovieSource source)
        : search_(search), source_(std::move(source)), points_(checkpointsOf(source_.movie)) {
        const std::uint64_t cs = search_.clusterSize();
        clusters_ = source_.length / cs + (source_.length % cs != 0 ? 1 : 0);
    }

    [[nodiscard]] Status run();
    // Where the first video sample that does not frame through `layout` starts (the file's length when
    // all do). Nothing when a limit stopped the probes.
    [[nodiscard]] Result<std::optional<std::uint64_t>> firstBadOffset(const Layout& layout);

private:
    [[nodiscard]] Result<std::optional<ProbeResult>> probe(const Layout& layout, std::size_t first, std::size_t last);
    [[nodiscard]] Result<Layout> continueFrom(const Layout& prefix, std::uint64_t start);
    [[nodiscard]] std::optional<std::uint64_t> joinCluster(std::uint64_t fileCluster) const noexcept;
    // The first checkpoint whose sample ends after file offset `offset`, or starts at or after it.
    [[nodiscard]] std::size_t firstEndingAfter(std::uint64_t offset) const noexcept;
    [[nodiscard]] std::size_t firstStartingAt(std::uint64_t offset) const noexcept;
    // The file clusters the samples before checkpoint `bad` confirm.
    [[nodiscard]] std::uint64_t confirmedClusters(std::size_t bad) const noexcept;
    [[nodiscard]] Result<std::vector<Walk>> bridge(const Walk& walk, std::size_t bad);
    [[nodiscard]] Result<std::vector<Walk>> finish(Walk walk);
    [[nodiscard]] Status submit(const Walk& walk);

    Search& search_;
    MovieSource source_;
    std::vector<Checkpoint> points_;
    std::uint64_t clusters_ = 0;
    // The reader of the last layout probed, kept while the walk probes it.
    Layout readerLayout_;
    std::unique_ptr<CandidateContentReader> reader_;
};

Result<std::optional<ProbeResult>> Placement::probe(const Layout& layout, std::size_t first, std::size_t last) {
    const FragmentSearchLimits& limits = search_.options().limits;
    if (search_.stats().sampleProbes >= limits.maxSampleProbes) {
        search_.limitReached("maxSampleProbes");
        return std::optional<ProbeResult>{};
    }
    if (search_.stats().bytesRead >= limits.maxReadBytes) {
        search_.limitReached("maxReadBytes");
        return std::optional<ProbeResult>{};
    }
    if (Status cancelled = search_.checkCancelled(); !cancelled.ok()) {
        return cancelled.error();
    }
    ++search_.stats().sampleProbes;
    if (reader_ == nullptr || readerLayout_ != layout) {
        RecoveryCandidate candidate;
        candidate.expectedSize = source_.length;
        candidate.sourceRegions = regionsOf(search_.volume(), layout, source_.length, source_.length, false);
        Result<std::unique_ptr<CandidateContentReader>> opened = CandidateContentReader::open(
            search_.source(), candidate, search_.options().carving.scan.reads, kProbeCacheSize);
        if (!opened.ok()) {
            return opened.error();
        }
        reader_ = std::move(*opened);
        readerLayout_ = layout;
    }
    CandidateContentReader& reader = *reader_;
    const std::uint64_t before = reader.bytesRead();
    const mp4::Movie movie = windowMovie(points_, first, last);
    Result<mp4::FramingCheck> framing = mp4::checkSampleFraming(reader, movie);
    search_.addBytesRead(reader.bytesRead() - before);
    if (!framing.ok()) {
        if (reader.sourceFailure().has_value()) {
            return *reader.sourceFailure();
        }
        return framing.error();
    }
    ProbeResult result;
    result.firstBad = last;
    if (!framing->tracks.empty()) {
        const mp4::TrackFraming& track = framing->tracks.front();
        result.checked = track.checked;
        if (track.firstBadSample.has_value()) {
            result.firstBad = first + static_cast<std::size_t>(*track.firstBadSample);
        }
    }
    return std::optional<ProbeResult>(result);
}

Result<std::optional<std::uint64_t>> Placement::firstBadOffset(const Layout& layout) {
    const std::size_t window = search_.options().limits.probeSamples;
    for (std::size_t from = 0; from < points_.size();) {
        const std::size_t to = std::min(points_.size(), from + window);
        Result<std::optional<ProbeResult>> result = probe(layout, from, to);
        if (!result.ok()) {
            return result.error();
        }
        if (!result->has_value()) {
            return std::optional<std::uint64_t>{};
        }
        if ((*result)->firstBad < to) {
            return std::optional<std::uint64_t>(points_[(*result)->firstBad].offset);
        }
        from = to;
    }
    return std::optional<std::uint64_t>(source_.length);
}

std::optional<std::uint64_t> Placement::joinCluster(std::uint64_t fileCluster) const noexcept {
    if (!source_.anchored || fileCluster > source_.moovFileCluster) {
        return std::nullopt;
    }
    const std::uint64_t back = source_.moovFileCluster - fileCluster;
    if (back > source_.moovCluster) {
        return std::nullopt;
    }
    return source_.moovCluster - back;
}

Result<Layout> Placement::continueFrom(const Layout& prefix, std::uint64_t start) {
    Result<Layout> layout = search_.extend(prefix, start, Policy::Contiguous);
    if (!layout.ok()) {
        return layout;
    }
    // Only as long as the file.
    return prefixOf(*layout, clusters_);
}

std::size_t Placement::firstEndingAfter(std::uint64_t offset) const noexcept {
    const auto found = std::partition_point(points_.begin(), points_.end(),
                                            [&](const Checkpoint& point) { return point.end() <= offset; });
    return static_cast<std::size_t>(found - points_.begin());
}

std::size_t Placement::firstStartingAt(std::uint64_t offset) const noexcept {
    const auto found = std::partition_point(points_.begin(), points_.end(),
                                            [&](const Checkpoint& point) { return point.offset < offset; });
    return static_cast<std::size_t>(found - points_.begin());
}

std::uint64_t Placement::confirmedClusters(std::size_t bad) const noexcept {
    // Up to the cluster holding the last byte of the last sample that framed.
    std::uint64_t end = 0;
    for (std::size_t i = 0; i < bad && i < points_.size(); ++i) {
        end = std::max(end, points_[i].end());
    }
    const std::uint64_t cs = search_.clusterSize();
    return end / cs + (end % cs != 0 ? 1 : 0);
}

Result<std::vector<Walk>> Placement::bridge(const Walk& walk, std::size_t bad) {
    const FragmentSearchLimits& limits = search_.options().limits;
    const std::uint64_t cs = search_.clusterSize();
    const Checkpoint& broken = points_[bad];
    // The fragment ends after the start of the last sample that framed and
    // before the end of the one that did not: every cluster boundary between
    // them, the one at the failing sample first.
    const std::uint64_t lastGood = bad > 0 ? points_[bad - 1].offset : 0;
    const std::uint64_t low = std::max<std::uint64_t>({walk.fixed, lastGood / cs + 1, 1});
    const std::uint64_t high = std::min((broken.end() - 1) / cs, clusters_ - 1);
    const std::size_t maxBoundaries = std::max(limits.maxBoundaries, kMinProbeBoundaries);
    std::vector<std::uint64_t> boundaries;
    if (low <= high) {
        const std::uint64_t at = std::clamp(broken.offset / cs, low, high);
        boundaries.push_back(at);
        for (std::uint64_t distance = 1; boundaries.size() < maxBoundaries; ++distance) {
            const bool before = distance <= at - low;
            const bool after = distance <= high - at;
            if (!before && !after) {
                break;
            }
            if (before) {
                boundaries.push_back(at - distance);
            }
            if (after && boundaries.size() < maxBoundaries) {
                boundaries.push_back(at + distance);
            }
        }
        if (high - low + 1 > boundaries.size()) {
            search_.candidatesCut("maxBoundaries");
        }
    }

    // Every continuation whose samples frame, at every boundary: the samples
    // may not tell some of them apart (clusters of audio, or inside a NAL unit).
    std::vector<Walk> accepted;
    std::vector<std::uint64_t> checkedUpTo;
    for (const std::uint64_t k : boundaries) {
        const Layout prefix = prefixOf(walk.layout, k);
        const std::optional<std::uint64_t> last = clusterAt(prefix, k - 1);
        if (!last.has_value()) {
            continue;
        }
        const std::optional<std::uint64_t> own = clusterAt(walk.layout, k);
        std::vector<std::uint64_t> starts;
        if (const std::optional<std::uint64_t> join = joinCluster(k); join.has_value() && !holds(prefix, *join)) {
            starts.push_back(*join);
        }
        ContinuationQuery query;
        query.after = *last;
        query.held = &prefix;
        query.self = search_.seed().owner;
        query.maxCandidates = limits.maxContinuations;
        query.maxScan = limits.maxSearchClusters;
        Result<Continuations> continuations =
            continuationsAfter(search_.volume(), query, search_.options().carving.scan.reads.cancellation);
        if (!continuations.ok()) {
            return continuations.error();
        }
        if (!continuations->limit.empty()) {
            search_.candidatesCut(continuations->limit);
        }
        for (const std::uint64_t start : continuations->clusters) {
            if (std::find(starts.begin(), starts.end(), start) == starts.end()) {
                starts.push_back(start);
            }
        }
        // The samples from the first one the boundary cuts or follows.
        const std::size_t from = firstEndingAfter(k * cs);
        const std::size_t to = std::min(points_.size(), std::max(from + limits.probeSamples, bad + 1));
        for (const std::uint64_t start : starts) {
            if (own == start) {
                continue;
            }
            Result<Layout> child = continueFrom(prefix, start);
            if (!child.ok()) {
                return child.error();
            }
            Result<std::optional<ProbeResult>> result = probe(*child, from, to);
            if (!result.ok()) {
                return result.error();
            }
            if (!result->has_value()) {
                return std::vector<Walk>{};
            }
            if ((*result)->firstBad != to || (*result)->checked == 0) {
                continue;
            }
            Walk next;
            next.layout = std::move(*child);
            next.next = to;
            next.fixed = k + 1;
            next.damagedBytes = walk.damagedBytes;
            next.joined = joinCluster(k) == std::optional<std::uint64_t>(start);
            accepted.push_back(std::move(next));
            checkedUpTo.push_back(std::min(source_.length, points_[to - 1].end()));
        }
    }
    if (!accepted.empty()) {
        // The best supported by the evidence over what was checked first;
        // equals are kept, up to the beam, as alternatives.
        std::vector<Hypothesis> ranked;
        for (std::size_t i = 0; i < accepted.size(); ++i) {
            Hypothesis h;
            h.layout = accepted[i].layout;
            h.assessment.status = ValidationStatus::Valid;
            h.assessment.length = checkedUpTo[i];
            h.assessment.progress = checkedUpTo[i];
            h.order = i;
            ranked.push_back(std::move(h));
        }
        rank(search_, ranked);
        std::vector<Walk> chosen;
        for (Hypothesis& h : ranked) {
            if (chosen.size() >= limits.beamWidth ||
                (!chosen.empty() && !equallySupported(search_, ranked.front(), h))) {
                break;
            }
            chosen.push_back(accepted[static_cast<std::size_t>(h.order)]);
        }
        return chosen;
    }

    // No continuation frames. The layout as it is, if later samples frame
    // through it again: the part between is damaged where it lies.
    for (std::size_t from = bad + 1; from < points_.size();) {
        const std::size_t to = std::min(points_.size(), from + limits.probeSamples);
        Result<std::optional<ProbeResult>> result = probe(walk.layout, from, to);
        if (!result.ok()) {
            return result.error();
        }
        if (!result->has_value()) {
            return std::vector<Walk>{};
        }
        if ((*result)->firstBad == to && (*result)->checked > 0) {
            Walk next = walk;
            next.next = to;
            next.damagedBytes += points_[from].offset - broken.offset;
            return std::vector<Walk>{std::move(next)};
        }
        from = std::max((*result)->firstBad + 1, from + 1);
    }

    // Or in the moov's fragment further on: the part between is missing. The
    // fragment starts at the cluster of the first sample that frames there.
    if (source_.anchored) {
        const std::uint64_t cut = std::max<std::uint64_t>({walk.fixed, confirmedClusters(bad), 1});
        for (std::uint64_t k = cut + 1; k <= source_.moovFileCluster && k < clusters_; ++k) {
            const std::optional<std::uint64_t> join = joinCluster(k);
            const Layout head = prefixOf(walk.layout, cut);
            if (!join.has_value() || holds(head, *join)) {
                continue;
            }
            const std::size_t from = firstStartingAt(k * cs);
            if (from >= points_.size()) {
                break;
            }
            Layout prefix = head;
            appendRun(prefix, Run{kUnplaced, k - cut});
            Result<Layout> child = continueFrom(prefix, *join);
            if (!child.ok()) {
                return child.error();
            }
            const std::size_t to = std::min(points_.size(), from + limits.probeSamples);
            Result<std::optional<ProbeResult>> result = probe(*child, from, to);
            if (!result.ok()) {
                return result.error();
            }
            if (!result->has_value()) {
                return std::vector<Walk>{};
            }
            if ((*result)->firstBad != to || (*result)->checked == 0) {
                continue;
            }
            const std::uint64_t start = std::max(k, points_[from].offset / cs);
            Layout confirmed = head;
            appendRun(confirmed, Run{kUnplaced, start - cut});
            Result<Layout> tail = continueFrom(confirmed, *joinCluster(start));
            if (!tail.ok()) {
                return tail.error();
            }
            Walk next;
            next.layout = std::move(*tail);
            next.next = to;
            next.fixed = start + 1;
            next.damagedBytes = walk.damagedBytes;
            next.joined = true;
            return std::vector<Walk>{std::move(next)};
        }
    }
    return std::vector<Walk>{};
}

Result<std::vector<Walk>> Placement::finish(Walk walk) {
    // Without an anchor, or once joined, the layout is complete as it is.
    if (!source_.anchored || walk.joined ||
        clusterAt(walk.layout, source_.moovFileCluster) == std::optional<std::uint64_t>(source_.moovCluster)) {
        return std::vector<Walk>{std::move(walk)};
    }
    // The moov's fragment starts somewhere after the last sample checked: the
    // samples cannot tell where, the evidence may.
    const FragmentSearchLimits& limits = search_.options().limits;
    const std::uint64_t low = std::max(walk.fixed, confirmedClusters(std::min(walk.next, points_.size())));
    std::vector<Walk> options;
    const std::size_t maxBoundaries = std::max(limits.maxBoundaries, kMinProbeBoundaries);
    for (std::uint64_t k = source_.moovFileCluster + 1; k-- > low;) {
        if (options.size() >= maxBoundaries) {
            search_.candidatesCut("maxBoundaries");
            break;
        }
        const std::optional<std::uint64_t> join = joinCluster(k);
        if (!join.has_value()) {
            continue;
        }
        const Layout prefix = prefixOf(walk.layout, k);
        if (holds(prefix, *join)) {
            continue;
        }
        Result<Layout> child = continueFrom(prefix, *join);
        if (!child.ok()) {
            return child.error();
        }
        Walk option = walk;
        option.layout = std::move(*child);
        option.joined = true;
        options.push_back(std::move(option));
    }
    return options;
}

Status Placement::submit(const Walk& walk) {
    // The bytes the layout gets right as far as the samples tell.
    std::uint64_t missing = 0;
    for (const SourceRegion& region :
         regionsOf(search_.volume(), walk.layout, source_.length, source_.length, false)) {
        if (region.kind == RegionKind::Missing) {
            missing += region.length;
        }
    }
    const std::uint64_t right = source_.length - std::min(source_.length, missing + walk.damagedBytes);
    if (search_.validated(walk.layout)) {
        // A layout validated before (the file where it lay, say): the samples place all of it.
        for (Hypothesis& hypothesis : search_.pool()) {
            if (hypothesis.layout == walk.layout && !hypothesis.wholeFile) {
                hypothesis.wholeFile = true;
                hypothesis.source = LayoutSource::SampleTables;
                hypothesis.confirmed = right;
                hypothesis.evidence.reset();
            }
        }
        return success();
    }
    Result<std::optional<Assessment>> assessment = search_.assess(walk.layout);
    if (!assessment.ok()) {
        return assessment.error();
    }
    if (!assessment->has_value()) {
        return success();
    }
    Hypothesis hypothesis;
    hypothesis.layout = walk.layout;
    hypothesis.source = LayoutSource::SampleTables;
    hypothesis.policy = Policy::Contiguous;
    hypothesis.assessment = std::move(**assessment);
    hypothesis.wholeFile = true;
    hypothesis.confirmed = right;
    (void)search_.admit(std::move(hypothesis));
    return success();
}

Status Placement::run() {
    Result<Layout> start = search_.extend({}, search_.seed().start, Policy::Contiguous);
    if (!start.ok()) {
        return start.error();
    }
    Walk first;
    first.layout = prefixOf(*start, clusters_);
    std::deque<Walk> walks;
    walks.push_back(std::move(first));
    std::size_t started = 1;
    const FragmentSearchLimits& limits = search_.options().limits;
    const std::uint64_t cs = search_.clusterSize();
    while (!walks.empty() && !search_.limited()) {
        Walk walk = std::move(walks.front());
        walks.pop_front();
        while (walk.next < points_.size()) {
            if (fragmentsOf(walk.layout) > limits.maxFragments) {
                search_.limitReached("maxFragments");
                break;
            }
            const std::size_t to = std::min(points_.size(), walk.next + limits.probeSamples);
            Result<std::optional<ProbeResult>> result = probe(walk.layout, walk.next, to);
            if (!result.ok()) {
                return result.error();
            }
            if (!result->has_value()) {
                return success();
            }
            const std::size_t bad = (*result)->firstBad;
            if (bad == to) {
                walk.next = to;
                continue;
            }
            Result<std::vector<Walk>> bridged = bridge(walk, bad);
            if (!bridged.ok()) {
                return bridged.error();
            }
            if (search_.limited()) {
                return success();
            }
            if (bridged->empty()) {
                // Nothing continues the file: what the samples confirm is
                // placed, the rest is not (the moov stays where it was found).
                const std::uint64_t cut = std::max<std::uint64_t>({walk.fixed, confirmedClusters(bad), 1});
                Layout placed = prefixOf(walk.layout, cut);
                if (source_.anchored && source_.moovFileCluster >= cut && source_.moovFileCluster < clusters_) {
                    appendRun(placed, Run{kUnplaced, source_.moovFileCluster - cut});
                    appendRun(placed, Run{source_.moovCluster, clusters_ - source_.moovFileCluster});
                } else if (clusters_ > cut) {
                    appendRun(placed, Run{kUnplaced, clusters_ - cut});
                }
                walk.layout = std::move(placed);
                walk.joined = true;
                break;
            }
            walk = std::move(bridged->front());
            for (std::size_t i = 1; i < bridged->size() && started < limits.beamWidth; ++i, ++started) {
                walks.push_back(std::move((*bridged)[i]));
            }
        }
        (void)cs;
        Result<std::vector<Walk>> finals = finish(std::move(walk));
        if (!finals.ok()) {
            return finals.error();
        }
        for (const Walk& done : *finals) {
            if (Status submitted = submit(done); !submitted.ok()) {
                return submitted;
            }
        }
    }
    return success();
}

// The movie in the file's first fragment, or found where the top-level boxes
// put it.
Result<std::vector<MovieSource>> moviesOf(Search& search) {
    std::vector<MovieSource> sources;
    const Hypothesis* base = nullptr;
    for (const Hypothesis& hypothesis : search.pool()) {
        if (hypothesis.source == LayoutSource::Contiguous) {
            base = &hypothesis;
            break;
        }
    }
    if (base == nullptr) {
        return sources;
    }
    const FragmentRecoveryOptions& options = search.options();
    const mp4::ParseLimits& limits = options.mp4.limits;
    const std::uint64_t window = search.contentLength();
    Result<std::unique_ptr<CandidateContentReader>> reader = search.open(base->layout, window, window);
    if (!reader.ok()) {
        return reader.error();
    }
    const std::uint64_t consistent = base->assessment.progress;
    Result<mp4::Mp4File> parsed = mp4::parseFile(**reader, limits);
    search.addBytesRead((*reader)->bytesRead());
    if (!parsed.ok()) {
        return parsed.error();
    }
    const auto usable = [](const mp4::Movie& movie) {
        return std::any_of(movie.tracks.begin(), movie.tracks.end(),
                           [](const mp4::Track& track) { return track.sampleTableValid; });
    };
    const bool knownSize = search.seed().size.has_value();
    // The movie of the first fragment: moov lies in the part that is consistent.
    if (parsed->movie.has_value() && usable(*parsed->movie) && parsed->movie->issues.empty() &&
        parsed->movie->box.end() <= consistent) {
        MovieSource source;
        source.movie = std::move(*parsed->movie);
        std::uint64_t length = window;
        if (!knownSize) {
            // The structure's end: the media data or the last box, whichever is further.
            length = source.movie.box.end();
            if (const std::optional<mp4::MediaExtent> extent = mp4::mediaExtent(source.movie)) {
                length = std::max(length, extent->end);
            }
            for (const mp4::BoxHeader& box : parsed->layout.boxes) {
                if (box.offset < consistent && box.sizeKind != mp4::BoxSize::ToEnd) {
                    length = std::max(length, box.end());
                }
            }
            length = std::min(length, window);
        }
        source.length = length;
        sources.push_back(std::move(source));
        return sources;
    }

    // moov after the media data: at the end of the first mdat, whose header the first fragment holds.
    const mp4::BoxHeader* mdat = nullptr;
    for (const mp4::BoxHeader& box : parsed->layout.boxes) {
        if (box.type == mp4::box::kMdat) {
            mdat = &box;
            break;
        }
    }
    if (mdat == nullptr && parsed->layout.cutBox.has_value() && parsed->layout.cutBox->type == mp4::box::kMdat) {
        mdat = &*parsed->layout.cutBox;
    }
    if (mdat == nullptr || mdat->sizeKind == mp4::BoxSize::ToEnd || mdat->payloadOffset() > consistent ||
        mdat->end() >= window) {
        return sources;
    }
    const std::uint64_t moovOffset = mdat->end();
    const VolumeEvidence& volume = search.volume();
    const Geometry& geometry = volume.geometry();
    const std::uint64_t cs = geometry.clusterSize;
    const std::uint64_t within = moovOffset % cs;
    const std::uint64_t moovFileCluster = moovOffset / cs;
    // Anchors in the order the file's writer would have met them from its start.
    std::vector<std::uint64_t> anchors;
    for (const std::uint64_t offset : search.moovAnchors()) {
        const std::optional<std::uint64_t> index = geometry.indexAt(offset);
        if (index.has_value() && (offset - geometry.dataStart) % cs == within) {
            anchors.push_back(offset);
        }
    }
    const std::uint64_t startOffset = geometry.offsetOf(search.seed().start);
    std::stable_partition(anchors.begin(), anchors.end(), [&](std::uint64_t offset) { return offset >= startOffset; });
    for (const std::uint64_t offset : anchors) {
        if (sources.size() >= options.limits.maxMovieAnchors) {
            break;
        }
        if (Status cancelled = search.checkCancelled(); !cancelled.ok()) {
            return cancelled.error();
        }
        const std::uint64_t cluster = *geometry.indexAt(offset);
        // The moov's cluster placed where the moov belongs, the rest after it contiguous.
        const std::uint64_t tailClusters = std::min(geometry.clusterCount - cluster,
                                                    search.targetClusters() - std::min(search.targetClusters(),
                                                                                       moovFileCluster));
        if (tailClusters == 0) {
            continue;
        }
        Layout layout;
        appendRun(layout, Run{kUnplaced, moovFileCluster});
        appendRun(layout, Run{cluster, tailClusters});
        Result<std::unique_ptr<CandidateContentReader>> tail = search.open(layout, window, window);
        if (!tail.ok()) {
            return tail.error();
        }
        Result<mp4::BoxRead> header = mp4::readBoxHeader(**tail, moovOffset, (*tail)->size());
        if (!header.ok()) {
            return header.error();
        }
        if (header->status != mp4::BoxStatus::Valid || header->header.type != mp4::box::kMoov ||
            header->header.sizeKind == mp4::BoxSize::ToEnd) {
            search.addBytesRead((*tail)->bytesRead());
            continue;
        }
        const std::uint64_t moovEnd = header->header.end();
        if (moovEnd > window) {
            search.addBytesRead((*tail)->bytesRead());
            continue;
        }
        Result<mp4::Movie> movie = mp4::parseMovie(**tail, header->header, limits);
        search.addBytesRead((*tail)->bytesRead());
        if (!movie.ok()) {
            return movie.error();
        }
        if (!usable(*movie)) {
            continue;
        }
        // Its media data must lie in the mdat before it.
        if (const std::optional<mp4::MediaExtent> extent = mp4::mediaExtent(*movie);
            !extent.has_value() || extent->begin < mdat->payloadOffset() || extent->end > moovOffset) {
            continue;
        }
        MovieSource source;
        source.movie = std::move(*movie);
        source.length = knownSize ? window : moovEnd;
        source.anchored = true;
        source.moovOffset = moovOffset;
        source.moovFileCluster = moovFileCluster;
        source.moovCluster = cluster;
        sources.push_back(std::move(source));
    }
    return sources;
}

}  // namespace

bool isIsoFormat(const carving::IFileFormat& format) noexcept {
    return dynamic_cast<const formats::Mp4Format*>(&format) != nullptr ||
           dynamic_cast<const formats::M4aFormat*>(&format) != nullptr;
}

Result<bool> placeBySampleTables(Search& search) {
    Result<std::vector<MovieSource>> movies = moviesOf(search);
    if (!movies.ok()) {
        return movies.error();
    }
    if (movies->empty()) {
        return false;
    }
    // The layouts validated so far are confirmed only as far as their samples frame: the
    // top-level boxes pass over the media data unchecked.
    {
        Placement scorer(search, movies->front());
        const std::uint64_t cs = search.clusterSize();
        for (std::size_t i = 0; i < search.pool().size(); ++i) {
            if (search.pool()[i].wholeFile ||
                search.pool()[i].assessment.status == carving::ValidationStatus::Valid) {
                continue;
            }
            const Layout layout = search.pool()[i].layout;
            Result<std::optional<std::uint64_t>> bad = scorer.firstBadOffset(layout);
            if (!bad.ok()) {
                return bad.error();
            }
            if (!bad->has_value()) {
                return true;
            }
            Hypothesis& hypothesis = search.pool()[i];
            const std::uint64_t framed = **bad >= cs ? **bad / cs * cs : **bad;
            hypothesis.confirmed = std::min(hypothesis.confirmed, framed);
            hypothesis.evidence.reset();
        }
    }
    for (MovieSource& movie : *movies) {
        if (search.limited()) {
            break;
        }
        Placement placement(search, std::move(movie));
        if (Status placed = placement.run(); !placed.ok()) {
            return placed.error();
        }
    }
    return true;
}

}  // namespace recovery::fragments
