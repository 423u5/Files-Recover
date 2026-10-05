#include "recovery/mp4_recovery.hpp"

#include "carving/format_registry.hpp"
#include "diagnostics/logger.hpp"
#include "formats/mp4_box.hpp"
#include "recovery/candidate_content.hpp"
#include "recovery/checked_math.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <variant>

namespace recovery {

namespace {

namespace mp4 = formats::mp4;
using carving::FileCandidate;
using carving::ValidationStatus;
using diagnostics::field;
using diagnostics::LogLevel;

constexpr std::string_view kComponent = "mp4_recovery";
// Bytes read to tell whether a filesystem candidate's content is an MP4 file.
constexpr std::size_t kSniffLength = 4096;
// Active files named in a carved file's allocation evidence.
constexpr std::size_t kMaxActiveFiles = 8;

std::string plural(std::uint64_t count, std::string_view one, std::string_view many) {
    return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

bool isVideoExtension(std::string_view extension) noexcept {
    static constexpr std::array<std::string_view, 8> kExtensions = {"mp4", "m4v", "mov", "qt",
                                                                    "3gp", "3g2", "3gpp", "3gp2"};
    return std::find(kExtensions.begin(), kExtensions.end(), extension) != kExtensions.end();
}

// ---------------------------------------------------------------------------
// The structure of one file's data
// ---------------------------------------------------------------------------

struct Analysis {
    Mp4Structure structure;
    // The movie (from the top-level boxes, or found by searching), for the
    // sample-table analysis against the file's layout.
    std::optional<mp4::Movie> movie;
    // Mp4Format's verdict on the data.
    carving::ValidationResult verdict;
};

Result<Analysis> analyze(carving::IContentReader& content, const formats::Mp4FormatOptions& format) {
    Result<mp4::Mp4File> parsed = mp4::parseFile(content, format.limits);
    if (!parsed.ok()) {
        return parsed.error();
    }
    mp4::Mp4File& file = *parsed;
    Analysis analysis;
    Mp4Structure& structure = analysis.structure;
    structure.dataSize = content.size();
    structure.status = file.status;
    structure.detail = file.detail;
    structure.issues = file.issues.recorded();
    structure.issueCount = file.issues.count();
    if (file.fileType.has_value()) {
        structure.majorBrand = file.fileType->majorBrand;
    }
    structure.mediaDataBoxes = file.layout.all(mp4::box::kMdat).size() +
                               (file.layout.cutBox.has_value() && file.layout.cutBox->type == mp4::box::kMdat ? 1 : 0);
    structure.movieFragments = file.fragments.size();
    std::uint64_t boxesEnd = file.layout.boxes.empty() ? 0 : file.layout.boxes.back().end();
    if (file.layout.cutBox.has_value()) {
        boxesEnd = std::max(boxesEnd, file.layout.cutBox->end());
    }

    std::optional<mp4::FramingCheck> framing;
    if (file.movie.has_value() && format.checkSampleFraming) {
        Result<mp4::FramingCheck> checked = mp4::checkSampleFraming(content, *file.movie);
        if (!checked.ok()) {
            return checked.error();
        }
        framing = std::move(*checked);
    }
    analysis.verdict = formats::mp4Verdict(file, content.size(), framing);

    std::optional<mp4::Movie> movie = std::move(file.movie);
    if (!movie.has_value()) {
        // moov discovery: the top-level boxes do not lead to a movie.
        Result<std::optional<mp4::FoundMovie>> found = mp4::findMovie(content, 0, content.size(), format.limits);
        if (!found.ok()) {
            return found.error();
        }
        if (found->has_value()) {
            movie = std::move((*found)->movie);
            structure.moovFoundBySearch = true;
            structure.status = mp4::FileStatus::Invalid;
            structure.detail = (structure.detail.empty() ? std::string() : structure.detail + "; ") +
                               "moov found by searching the data at offset " + std::to_string(movie->box.offset);
            if (format.checkSampleFraming) {
                Result<mp4::FramingCheck> checked = mp4::checkSampleFraming(content, *movie);
                if (!checked.ok()) {
                    return checked.error();
                }
                framing = std::move(*checked);
            }
        }
    }
    const mp4::Classification kind = mp4::classify(file.fileType, movie.has_value() ? &*movie : nullptr);
    structure.kind = kind.kind;
    structure.kindReason = kind.reason;
    structure.structureEnd = boxesEnd;
    if (movie.has_value()) {
        structure.moovOffset = movie->box.offset;
        structure.moovSize = movie->box.size;
        structure.media = mp4::mediaExtent(*movie);
        structure.moovBeforeMediaData =
            structure.media.has_value() ? movie->box.offset < structure.media->begin : movie->box.offset < boxesEnd;
        structure.structureEnd = std::max({boxesEnd, movie->box.end(), structure.media ? structure.media->end : 0});
        for (std::size_t t = 0; t < movie->tracks.size(); ++t) {
            const mp4::Track& track = movie->tracks[t];
            Mp4TrackEvidence evidence;
            evidence.number = static_cast<std::uint32_t>(t + 1);
            evidence.kind = track.kind;
            if (!track.samples.descriptions.empty()) {
                const mp4::SampleDescription& description = track.samples.descriptions.front();
                evidence.codec = description.format;
                evidence.width = description.width;
                evidence.height = description.height;
                evidence.channels = description.channelCount;
                evidence.sampleRate = description.sampleRate;
            }
            evidence.duration = track.media.duration;
            evidence.timescale = track.media.timescale;
            for (const bool run : {false, true}) {
                for (const mp4::Chunk& chunk : run ? track.fragmentRuns : track.chunks) {
                    evidence.samples += chunk.sampleCount;
                    evidence.sampleBytes += chunk.size;
                }
            }
            if (framing.has_value()) {
                for (const mp4::TrackFraming& checked : framing->tracks) {
                    if (checked.track == evidence.number) {
                        evidence.samplesFramed = checked.checked;
                        evidence.samplesMisframed = checked.bad;
                    }
                }
            }
            structure.tracks.push_back(evidence);
        }
    }
    analysis.movie = std::move(movie);
    return analysis;
}

// ---------------------------------------------------------------------------
// Sample-table analysis against the file's layout
// ---------------------------------------------------------------------------

// How bad a file range is; a higher value is worse.
enum class Damage : std::uint8_t { Reallocated = 0, Unreadable = 1, Missing = 2 };
constexpr std::size_t kDamageKinds = 3;

struct DamagedRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    Damage damage = Damage::Missing;
};

// Disjoint, sorted segments with the worst damage of the ranges covering them.
std::vector<DamagedRange> flatten(std::vector<DamagedRange> ranges) {
    struct Event {
        std::uint64_t at;
        int delta;
        Damage damage;
    };
    std::vector<Event> events;
    events.reserve(ranges.size() * 2);
    for (const DamagedRange& range : ranges) {
        if (range.begin < range.end) {
            events.push_back(Event{range.begin, +1, range.damage});
            events.push_back(Event{range.end, -1, range.damage});
        }
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.at < b.at; });
    std::array<std::int64_t, kDamageKinds> active{};
    std::vector<DamagedRange> segments;
    std::size_t i = 0;
    while (i < events.size()) {
        const std::uint64_t at = events[i].at;
        for (; i < events.size() && events[i].at == at; ++i) {
            active[static_cast<std::size_t>(events[i].damage)] += events[i].delta;
        }
        if (i == events.size()) {
            break;
        }
        std::optional<Damage> worst;
        for (std::size_t k = kDamageKinds; k-- > 0;) {
            if (active[k] > 0) {
                worst = static_cast<Damage>(k);
                break;
            }
        }
        if (worst.has_value()) {
            const std::uint64_t next = events[i].at;
            if (!segments.empty() && segments.back().end == at && segments.back().damage == *worst) {
                segments.back().end = next;
            } else {
                segments.push_back(DamagedRange{at, next, *worst});
            }
        }
    }
    return segments;
}

// Maps source ranges inside the stored regions to file ranges.
void addUnreadable(const std::vector<SourceRegion>& regions, const storage::BadRegionMap& unreadable,
                   std::vector<DamagedRange>& ranges) {
    for (const SourceRegion& region : regions) {
        if (region.kind != RegionKind::Stored) {
            continue;
        }
        for (const storage::BadRegion& bad : unreadable.overlapping(region.sourceOffset, region.length)) {
            const std::uint64_t begin = std::max(bad.offset, region.sourceOffset);
            const std::uint64_t end = std::min(bad.end(), region.sourceOffset + region.length);
            if (begin < end) {
                ranges.push_back(DamagedRange{region.fileOffset + (begin - region.sourceOffset),
                                              region.fileOffset + (end - region.sourceOffset), Damage::Unreadable});
            }
        }
    }
}

// The damaged file ranges of a candidate's data read through `content`.
std::vector<DamagedRange> damageOf(const RecoveryCandidate& candidate, const CandidateContentReader& content) {
    std::vector<DamagedRange> ranges;
    for (const SourceRegion& region : candidate.sourceRegions) {
        const std::uint64_t end = region.fileOffset + region.length;
        if (region.kind == RegionKind::Missing || region.kind == RegionKind::Zeros) {
            ranges.push_back(DamagedRange{region.fileOffset, end, Damage::Missing});
        } else if (region.kind == RegionKind::Stored && region.reallocated) {
            ranges.push_back(DamagedRange{region.fileOffset, end, Damage::Reallocated});
        }
    }
    addUnreadable(candidate.sourceRegions, content.unreadable(), ranges);
    for (const auto& [begin, end] : content.outsideSource()) {
        ranges.push_back(DamagedRange{begin, end, Damage::Unreadable});
    }
    return flatten(std::move(ranges));
}

// Counts each track's samples by the worst damage of the bytes they cover.
void countSamples(Mp4Structure& structure, const mp4::Movie& movie, const std::vector<DamagedRange>& segments) {
    for (Mp4TrackEvidence& evidence : structure.tracks) {
        evidence.samplesBeyondData = 0;
        evidence.samplesMissing = 0;
        evidence.samplesUnreadable = 0;
        evidence.samplesReallocated = 0;
        evidence.samplesIntact = 0;
        if (evidence.number == 0 || evidence.number > movie.tracks.size()) {
            continue;
        }
        mp4::forEachSample(movie.tracks[evidence.number - 1], [&](std::uint32_t, std::uint64_t offset,
                                                                  std::uint32_t size) {
            const std::uint64_t end = offset + size;
            if (end > structure.dataSize) {
                ++evidence.samplesBeyondData;
                return;
            }
            // The first segment ending after the sample's start.
            auto segment = std::upper_bound(segments.begin(), segments.end(), offset,
                                            [](std::uint64_t value, const DamagedRange& r) { return value < r.end; });
            std::optional<Damage> worst;
            for (; segment != segments.end() && segment->begin < end; ++segment) {
                if (!worst.has_value() || segment->damage > *worst) {
                    worst = segment->damage;
                }
            }
            if (!worst.has_value()) {
                ++evidence.samplesIntact;
            } else if (*worst == Damage::Missing) {
                ++evidence.samplesMissing;
            } else if (*worst == Damage::Unreadable) {
                ++evidence.samplesUnreadable;
            } else {
                ++evidence.samplesReallocated;
            }
        });
    }
}

// ---------------------------------------------------------------------------
// Allocation of a byte range on a volume
// ---------------------------------------------------------------------------

struct AllocationScan {
    Mp4Allocation evidence;
    // Source ranges of the clusters allocated now, merged.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> allocated;
};

// Active files' stored data on one volume, sorted by source offset.
struct ActiveData {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::size_t candidate = 0;
};

std::vector<ActiveData> activeDataOf(const CandidateScan& scan) {
    std::vector<ActiveData> data;
    for (std::size_t i = 0; i < scan.candidates.size(); ++i) {
        const RecoveryCandidate& candidate = scan.candidates[i];
        if (candidate.isDeleted()) {
            continue;
        }
        for (const SourceRegion& region : candidate.sourceRegions) {
            if (region.kind == RegionKind::Stored && region.length > 0) {
                data.push_back(ActiveData{region.sourceOffset, region.sourceOffset + region.length, i});
            }
        }
    }
    std::sort(data.begin(), data.end(), [](const ActiveData& a, const ActiveData& b) { return a.begin < b.begin; });
    return data;
}

// The source offset of a volume's cluster, if it is one of its data clusters.
std::optional<std::uint64_t> clusterOffset(const FilesystemRecovery& volume, const filesystem::FilesystemInfo& info,
                                           std::uint64_t cluster) {
    if (cluster < info.firstCluster || cluster - info.firstCluster >= info.clusterCount || info.clusterSize == 0) {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> within =
        checkedMul(cluster - info.firstCluster, std::uint64_t{info.clusterSize});
    if (!within.has_value()) {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> inVolume = checkedAdd(info.dataOffset, *within);
    return inVolume.has_value() ? checkedAdd(volume.volumeOffset(), *inVolume) : std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

std::string_view toString(Mp4Warning warning) noexcept {
    switch (warning) {
    case Mp4Warning::StructureInvalid:
        return "StructureInvalid";
    case Mp4Warning::StructureTruncated:
        return "StructureTruncated";
    case Mp4Warning::NoMovie:
        return "NoMovie";
    case Mp4Warning::MovieFoundBySearch:
        return "MovieFoundBySearch";
    case Mp4Warning::SamplesDamaged:
        return "SamplesDamaged";
    case Mp4Warning::SamplesMisframed:
        return "SamplesMisframed";
    case Mp4Warning::SizeMismatch:
        return "SizeMismatch";
    case Mp4Warning::AllocatedClusters:
        return "AllocatedClusters";
    case Mp4Warning::InsideActiveFile:
        return "InsideActiveFile";
    }
    return "Unknown";
}

std::uint64_t Mp4Structure::samples() const noexcept {
    std::uint64_t total = 0;
    for (const Mp4TrackEvidence& track : tracks) {
        total += track.samples;
    }
    return total;
}

std::uint64_t Mp4Structure::samplesIntact() const noexcept {
    std::uint64_t total = 0;
    for (const Mp4TrackEvidence& track : tracks) {
        total += track.samplesIntact;
    }
    return total;
}

std::uint64_t Mp4Structure::samplesMisframed() const noexcept {
    std::uint64_t total = 0;
    for (const Mp4TrackEvidence& track : tracks) {
        total += track.samplesMisframed;
    }
    return total;
}

std::uint64_t Mp4Structure::samplesDamaged() const noexcept {
    std::uint64_t total = 0;
    for (const Mp4TrackEvidence& track : tracks) {
        total += track.samplesBeyondData + track.samplesMissing + track.samplesUnreadable + track.samplesReallocated;
    }
    return total;
}

bool Mp4Structure::intact() const noexcept {
    return status == formats::mp4::FileStatus::Valid && moovOffset.has_value() && samplesDamaged() == 0 &&
           samplesMisframed() == 0;
}

bool Mp4Candidate::hasWarning(Mp4Warning warning) const noexcept {
    return std::find(warnings.begin(), warnings.end(), warning) != warnings.end();
}

std::string_view mp4Extension(std::optional<formats::mp4::FourCc> majorBrand) noexcept {
    if (!majorBrand.has_value()) {
        return "mp4";
    }
    const std::uint32_t brand = majorBrand->value();
    const auto startsWith = [&](std::string_view prefix) {
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            if (((brand >> (24 - 8 * i)) & 0xFFU) != static_cast<unsigned char>(prefix[i])) {
                return false;
            }
        }
        return true;
    };
    if (*majorBrand == formats::mp4::FourCc("qt  ")) {
        return "mov";
    }
    if (startsWith("M4V")) {
        return "m4v";
    }
    if (startsWith("3g2")) {
        return "3g2";
    }
    if (startsWith("3gp") || startsWith("3gg") || startsWith("3gs") || startsWith("3gr")) {
        return "3gp";
    }
    return "mp4";
}

Result<Mp4Structure> analyzeMp4(carving::IContentReader& content, const formats::Mp4FormatOptions& format) {
    Result<Analysis> analysis = analyze(content, format);
    if (!analysis.ok()) {
        return analysis.error();
    }
    if (analysis->movie.has_value()) {
        countSamples(analysis->structure, *analysis->movie, {});
    }
    return std::move(analysis->structure);
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

namespace {

// The candidate's own data validated: its layout stands.
bool ownValid(const Mp4PendingCandidate& pending) noexcept {
    const Mp4Structure& structure = pending.candidate.structure;
    return pending.examined && structure.status == mp4::FileStatus::Valid && structure.samplesMisframed() == 0;
}

// Whether a carve from the candidate's start would give it its layout
// (HYBRID): when the candidate's own data did not validate and the carve's
// did, or when the metadata located no data at all.
bool takesCarveLayout(const Mp4PendingCandidate& pending, bool carveValid) noexcept {
    return !(pending.examined && (ownValid(pending) || !carveValid));
}

// A deleted entry whose first cluster is allocated to other data now: what
// starts there is the new owner's file, so its structure confirms nothing
// about the entry.
bool startReallocated(const RecoveryCandidate& data) noexcept {
    return data.isDeleted() && !data.sourceRegions.empty() && data.sourceRegions.front().kind == RegionKind::Stored &&
           data.sourceRegions.front().reallocated;
}

}  // namespace

struct Mp4HitWork::Impl {
    carving::SignatureHit hit;
    // The carve: rejected, or carved with the validation the analysis gave it
    // (or that of the filesystem candidate holding the same bytes).
    std::variant<carving::CarveRejection, FileCandidate> outcome;
    std::optional<Analysis> analysis;
    std::vector<DamagedRange> unreadable;
};

Mp4HitWork::Mp4HitWork() = default;
Mp4HitWork::~Mp4HitWork() = default;
Mp4HitWork::Mp4HitWork(Mp4HitWork&&) noexcept = default;
Mp4HitWork& Mp4HitWork::operator=(Mp4HitWork&&) noexcept = default;

std::uint64_t Mp4HitWork::fileOffset() const noexcept {
    return impl_ != nullptr ? impl_->hit.fileOffset : 0;
}

// The state the steps share; Mp4Recovery::run drives the same steps.
struct Mp4RecoverySteps::Impl {
    Impl(storage::IStorageSource& source, Mp4RecoveryOptions options)
        : source(source), options(std::move(options)), format(this->options.format) {}

    [[nodiscard]] const carving::SourceReadOptions& reads() const noexcept { return options.carving.scan.reads; }
    void log(LogLevel level, std::string_view message, std::initializer_list<diagnostics::LogField> fields) const {
        if (options.carving.scan.logger != nullptr) {
            options.carving.scan.logger->log(level, kComponent, message, fields);
        }
    }
    [[nodiscard]] std::optional<std::uint64_t> startOf(const RecoveryCandidate& candidate, std::size_t volume) const;
    [[nodiscard]] std::optional<std::size_t> volumeAt(std::uint64_t sourceOffset) const;
    Result<AllocationScan> allocationOf(std::size_t volume, std::uint64_t begin, std::uint64_t end);
    Result<Analysis> analyzeCarve(FileCandidate& carve, std::vector<DamagedRange>& unreadable) const;
    Status merge(Mp4PendingCandidate& pending, FileCandidate carve, std::optional<Analysis>& analysis,
                 std::vector<DamagedRange>& unreadable);
    [[nodiscard]] std::vector<SourceRegion> regionsFor(const FileCandidate& carve, const AllocationScan* scan) const;
    Status apply(Mp4HitWork::Impl& work);
    void finish(Mp4Candidate& candidate, std::uint64_t id) const;

    storage::IStorageSource& source;
    const Mp4RecoveryOptions options;
    // The format carves are made with.
    const formats::Mp4Format format;
    std::vector<std::pair<FilesystemRecovery*, const CandidateScan*>> volumes;
    std::vector<Mp4PendingCandidate> pending;
    // Where each pending candidate starts on the source -> its index.
    std::multimap<std::uint64_t, std::size_t> starts;
    std::vector<Mp4Candidate> carved;
    // The carve that validated reaching furthest; hits strictly inside it are its own.
    std::uint64_t trustedStart = 0;
    std::uint64_t trustedEnd = 0;
    // Id of the next carve that is not rejected, as one carver numbers them.
    std::uint64_t nextCarveId = 1;
    Mp4RecoveryReport counts;
    // Per volume, the active files' data (built on first use).
    std::map<std::size_t, std::vector<ActiveData>> active;
    // The last examination added, to keep them in scan order.
    std::optional<std::pair<std::size_t, std::size_t>> lastExamined;
    // For takeChanges(): pending candidates changed, and carving candidates reported.
    std::set<std::size_t> changed;
    std::size_t carvedReported = 0;
    bool delivered = false;
};

std::optional<std::uint64_t> Mp4RecoverySteps::Impl::startOf(const RecoveryCandidate& candidate,
                                                            std::size_t volume) const {
    if (!candidate.sourceRegions.empty() && candidate.sourceRegions.front().kind == RegionKind::Stored) {
        return candidate.sourceRegions.front().sourceOffset;
    }
    // Nothing stored at the start: where the metadata says the first cluster is.
    const std::uint64_t cluster = candidate.filesystemEvidence.allocation.firstCluster.value();
    if (cluster == 0) {
        return std::nullopt;
    }
    FilesystemRecovery& recovery = *volumes[volume].first;
    return clusterOffset(recovery, recovery.filesystem().info(), cluster);
}

std::optional<std::size_t> Mp4RecoverySteps::Impl::volumeAt(std::uint64_t sourceOffset) const {
    for (std::size_t v = 0; v < volumes.size(); ++v) {
        FilesystemRecovery& recovery = *volumes[v].first;
        const filesystem::FilesystemInfo& info = recovery.filesystem().info();
        const std::uint64_t begin = recovery.volumeOffset();
        if (sourceOffset >= begin && sourceOffset - begin < info.volumeSize) {
            return v;
        }
    }
    return std::nullopt;
}

Result<AllocationScan> Mp4RecoverySteps::Impl::allocationOf(std::size_t volume, std::uint64_t begin,
                                                            std::uint64_t end) {
    FilesystemRecovery& recovery = *volumes[volume].first;
    filesystem::IFilesystem& fs = recovery.filesystem();
    const filesystem::FilesystemInfo& info = fs.info();
    AllocationScan scan;
    Mp4Allocation& evidence = scan.evidence;
    evidence.filesystem = info.type;
    evidence.volumeOffset = recovery.volumeOffset();

    // The volume's clusters the range covers.
    const std::optional<std::uint64_t> dataStart = checkedAdd(recovery.volumeOffset(), info.dataOffset);
    const std::optional<std::uint64_t> areaSize = checkedMul(info.clusterCount, std::uint64_t{info.clusterSize});
    if (dataStart.has_value() && areaSize.has_value() && info.clusterSize != 0) {
        const std::uint64_t dataEnd = checkedAdd(*dataStart, *areaSize).value_or(UINT64_MAX);
        const std::uint64_t from = std::max(begin, *dataStart);
        const std::uint64_t to = std::min(end, dataEnd);
        if (from < to) {
            const std::uint64_t first = (from - *dataStart) / info.clusterSize;
            const std::uint64_t last = (to - 1 - *dataStart) / info.clusterSize;
            evidence.clusters = last - first + 1;
            for (std::uint64_t index = first; index <= last; ++index) {
                if (index - first >= options.maxClusterChecks) {
                    evidence.complete = false;
                    break;
                }
                if (reads().cancellation.isCancellationRequested()) {
                    return makeError(ErrorCode::Cancelled, "MP4 recovery cancelled");
                }
                Result<filesystem::ClusterState> state =
                    fs.clusterState(filesystem::ClusterNumber{info.firstCluster + index});
                if (!state.ok() || (*state != filesystem::ClusterState::Free &&
                                    *state != filesystem::ClusterState::Allocated)) {
                    ++evidence.otherClusters;
                    continue;
                }
                if (*state == filesystem::ClusterState::Free) {
                    ++evidence.freeClusters;
                    continue;
                }
                ++evidence.allocatedClusters;
                const std::uint64_t clusterBegin = std::max(from, *dataStart + index * info.clusterSize);
                const std::uint64_t clusterEnd = std::min(to, *dataStart + (index + 1) * info.clusterSize);
                if (!scan.allocated.empty() && scan.allocated.back().second == clusterBegin) {
                    scan.allocated.back().second = clusterEnd;
                } else {
                    scan.allocated.emplace_back(clusterBegin, clusterEnd);
                }
            }
        }
    }

    // Active files whose data overlaps the range.
    auto found = active.find(volume);
    if (found == active.end()) {
        found = active.emplace(volume, activeDataOf(*volumes[volume].second)).first;
    }
    const std::vector<ActiveData>& data = found->second;
    auto at = std::lower_bound(data.begin(), data.end(), begin,
                               [](const ActiveData& d, std::uint64_t value) { return d.begin < value; });
    if (at != data.begin()) {
        --at;
    }
    std::vector<std::size_t> named;
    for (; at != data.end() && at->begin < end; ++at) {
        if (at->end <= begin) {
            continue;
        }
        if (at->begin <= begin && at->end >= end) {
            evidence.insideActiveFile = true;
        }
        if (std::find(named.begin(), named.end(), at->candidate) == named.end()) {
            named.push_back(at->candidate);
            if (evidence.activeFiles.size() < kMaxActiveFiles) {
                const RecoveryCandidate& file = volumes[volume].second->candidates[at->candidate];
                evidence.activeFiles.push_back(file.filesystemEvidence.path);
            }
        }
    }
    return scan;
}

Result<Analysis> Mp4RecoverySteps::Impl::analyzeCarve(FileCandidate& carve,
                                                      std::vector<DamagedRange>& unreadable) const {
    Result<std::unique_ptr<carving::SourceContentReader>> content = carving::SourceContentReader::open(
        source, carve.sourceOffset, carve.length, reads(), options.carving.readCacheSize);
    if (!content.ok()) {
        return content.error();
    }
    Result<Analysis> analysis = analyze(**content, options.format);
    if (!analysis.ok()) {
        return analysis.error();
    }
    carve.validation = analysis->verdict;
    carve.validation.validBytes = std::min(carve.validation.validBytes, carve.length);
    std::vector<DamagedRange> ranges;
    for (const storage::BadRegion& bad : (*content)->unreadable().overlapping(carve.sourceOffset, carve.length)) {
        const std::uint64_t begin = std::max(bad.offset, carve.sourceOffset);
        const std::uint64_t end = std::min(bad.end(), carve.sourceEnd());
        if (begin < end) {
            ranges.push_back(DamagedRange{begin - carve.sourceOffset, end - carve.sourceOffset, Damage::Unreadable});
        }
    }
    unreadable = std::move(ranges);
    return analysis;
}

std::vector<SourceRegion> Mp4RecoverySteps::Impl::regionsFor(const FileCandidate& carve,
                                                             const AllocationScan* scan) const {
    std::vector<SourceRegion> regions;
    std::uint64_t position = carve.sourceOffset;
    const auto add = [&](std::uint64_t end, bool allocated) {
        if (end > position) {
            regions.push_back(SourceRegion{position - carve.sourceOffset, end - position, RegionKind::Stored, position,
                                           allocated});
            position = end;
        }
    };
    if (scan != nullptr) {
        for (const auto& [begin, end] : scan->allocated) {
            add(begin, false);
            add(end, true);
        }
    }
    add(carve.sourceEnd(), false);
    return regions;
}

Status Mp4RecoverySteps::Impl::merge(Mp4PendingCandidate& pendingCandidate, FileCandidate carve,
                                     std::optional<Analysis>& analysis, std::vector<DamagedRange>& unreadable) {
    Mp4Candidate& video = pendingCandidate.candidate;
    if (!takesCarveLayout(pendingCandidate, carve.validation.status == ValidationStatus::Valid)) {
        // The metadata's layout stands; the carve is evidence of the same file.
        video.carving = std::move(carve);
        return success();
    }
    // HYBRID: the metadata's name and start, the carve's layout and length.
    if (!analysis.has_value()) {
        return makeError(ErrorCode::InternalError, "a carve merged without its analysis");
    }
    // A deleted entry's clusters allocated now belong to other data, whatever it is.
    const bool deleted = video.data.isDeleted();
    std::optional<AllocationScan> allocation;
    if (deleted) {
        Result<AllocationScan> scanned = allocationOf(pendingCandidate.volume, carve.sourceOffset, carve.sourceEnd());
        if (!scanned.ok()) {
            return scanned.error();
        }
        allocation = std::move(*scanned);
    }
    std::vector<SourceRegion> regions = regionsFor(carve, allocation.has_value() ? &*allocation : nullptr);
    if (deleted && !regions.empty() && regions.front().reallocated) {
        // Another file took the entry's first cluster: the carve is that file's, not the entry's.
        video.carving = std::move(carve);
        return success();
    }
    RecoveryCandidate& data = video.data;
    data.method = RecoveryMethod::Hybrid;
    data.expectedSize = carve.length;
    data.embeddedData.clear();
    data.sourceRegions = std::move(regions);
    data.fragmentation = FragmentationInfo{1, false};
    // What the metadata said about the regions it located no longer applies;
    // LayoutGuessed stays (the carve assumes contiguity too).
    std::erase_if(data.warnings, [](CandidateWarning warning) {
        return warning == CandidateWarning::DataMissing || warning == CandidateWarning::ClustersReallocated ||
               warning == CandidateWarning::DataNotDecoded;
    });
    if (std::any_of(data.sourceRegions.begin(), data.sourceRegions.end(),
                    [](const SourceRegion& r) { return r.reallocated; })) {
        data.warnings.push_back(CandidateWarning::ClustersReallocated);
    }
    video.structure = std::move(analysis->structure);
    if (analysis->movie.has_value()) {
        std::vector<DamagedRange> ranges = std::move(unreadable);
        for (const SourceRegion& region : data.sourceRegions) {
            if (region.reallocated) {
                ranges.push_back(
                    DamagedRange{region.fileOffset, region.fileOffset + region.length, Damage::Reallocated});
            }
        }
        countSamples(video.structure, *analysis->movie, flatten(std::move(ranges)));
    }
    video.carving = std::move(carve);
    return success();
}

Status Mp4RecoverySteps::Impl::apply(Mp4HitWork::Impl& work) {
    if (std::holds_alternative<carving::CarveRejection>(work.outcome)) {
        ++counts.carvesRejected;
        return success();
    }
    FileCandidate carve = std::get<FileCandidate>(std::move(work.outcome));
    carve.id = carving::FileCandidateId{nextCarveId++};
    ++counts.carved;
    std::optional<Analysis>& analysis = work.analysis;
    std::vector<DamagedRange>& unreadable = work.unreadable;
    if (carve.end.status == carving::EndStatus::Found && carve.validation.status == ValidationStatus::Valid &&
        carve.sourceEnd() > trustedEnd) {
        trustedStart = carve.sourceOffset;
        trustedEnd = carve.sourceEnd();
    }

    const auto [first, last] = starts.equal_range(carve.sourceOffset);
    if (first != last) {
        for (auto it = first; it != last; ++it) {
            // The last candidate starting here takes the analysis; the others copy it.
            const bool lastOne = std::next(it) == last;
            std::optional<Analysis> own = lastOne ? std::move(analysis) : analysis;
            std::vector<DamagedRange> ranges = lastOne ? std::move(unreadable) : unreadable;
            if (Status merged = merge(pending[it->second], carve, own, ranges); !merged.ok()) {
                return merged;
            }
            changed.insert(it->second);
        }
        ++counts.carvesMerged;
        return success();
    }
    if (!analysis.has_value()) {
        return makeError(ErrorCode::InternalError, "a carve without its analysis");
    }

    // CARVING: no metadata names it.
    Mp4Candidate video;
    video.data.method = RecoveryMethod::Carving;
    video.data.extension = std::string(mp4Extension(analysis->structure.majorBrand));
    video.data.expectedSize = carve.length;
    std::optional<AllocationScan> allocation;
    if (const std::optional<std::size_t> volume = volumeAt(carve.sourceOffset); volume.has_value()) {
        Result<AllocationScan> scanned = allocationOf(*volume, carve.sourceOffset, carve.sourceEnd());
        if (!scanned.ok()) {
            return scanned.error();
        }
        allocation = std::move(*scanned);
    }
    // Clusters allocated now hold other data, unless the carve lies inside
    // an active file: then they are that file's, and so are the carve's bytes.
    const bool reused = allocation.has_value() && !allocation->evidence.insideActiveFile;
    video.data.sourceRegions = regionsFor(carve, reused ? &*allocation : nullptr);
    video.data.fragmentation = FragmentationInfo{1, false};
    if (video.data.reallocatedBytes() > 0) {
        video.data.warnings.push_back(CandidateWarning::ClustersReallocated);
    }
    video.structure = std::move(analysis->structure);
    if (analysis->movie.has_value()) {
        std::vector<DamagedRange> ranges = std::move(unreadable);
        for (const SourceRegion& region : video.data.sourceRegions) {
            if (region.reallocated) {
                ranges.push_back(
                    DamagedRange{region.fileOffset, region.fileOffset + region.length, Damage::Reallocated});
            }
        }
        countSamples(video.structure, *analysis->movie, flatten(std::move(ranges)));
    }
    if (allocation.has_value()) {
        video.allocation = std::move(allocation->evidence);
    }
    video.carving = std::move(carve);
    carved.push_back(std::move(video));
    return success();
}

void Mp4RecoverySteps::Impl::finish(Mp4Candidate& candidate, std::uint64_t id) const {
    candidate.data.id = CandidateId{id};
    if (candidate.data.method == RecoveryMethod::Carving) {
        std::string number = std::to_string(id);
        if (number.size() < 6) {
            number.insert(0, 6 - number.size(), '0');
        }
        candidate.data.filename = "recovered_" + number + "." + candidate.data.extension;
    }
    const Mp4Structure& structure = candidate.structure;
    std::vector<Mp4Warning>& warnings = candidate.warnings;
    warnings.clear();
    if (structure.status == mp4::FileStatus::Invalid) {
        warnings.push_back(Mp4Warning::StructureInvalid);
    } else if (structure.status == mp4::FileStatus::Truncated) {
        warnings.push_back(Mp4Warning::StructureTruncated);
    }
    if (!structure.moovOffset.has_value()) {
        warnings.push_back(Mp4Warning::NoMovie);
    }
    if (structure.moovFoundBySearch) {
        warnings.push_back(Mp4Warning::MovieFoundBySearch);
    }
    if (structure.samplesDamaged() > 0) {
        warnings.push_back(Mp4Warning::SamplesDamaged);
    }
    if (structure.samplesMisframed() > 0) {
        warnings.push_back(Mp4Warning::SamplesMisframed);
    }
    if (candidate.recordedSize.has_value() && structure.moovOffset.has_value()) {
        // The structure ends elsewhere than the recorded size: data after the
        // last box, or a structure the recorded size cuts short.
        const bool trailing = structure.status == mp4::FileStatus::Invalid && structure.issueCount == 1 &&
                              !structure.issues.empty() && structure.issues.front().offset == structure.structureEnd;
        const bool comparable = structure.status != mp4::FileStatus::Invalid || trailing;
        if (comparable && structure.structureEnd != *candidate.recordedSize) {
            warnings.push_back(Mp4Warning::SizeMismatch);
        }
    }
    if (candidate.allocation.has_value()) {
        if (candidate.allocation->insideActiveFile) {
            warnings.push_back(Mp4Warning::InsideActiveFile);
        } else if (candidate.allocation->allocatedClusters > 0) {
            warnings.push_back(Mp4Warning::AllocatedClusters);
        }
    }
}

// ---------------------------------------------------------------------------
// Mp4RecoverySteps
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Mp4RecoverySteps>> Mp4RecoverySteps::create(storage::IStorageSource& source,
                                                                   Mp4RecoveryOptions options) {
    if (!source.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "MP4 recovery source is not open");
    }
    if (options.carving.readCacheSize < carving::SourceContentReader::kMinCacheSize ||
        options.carving.readCacheSize > carving::IContentReader::kMaxReadLength) {
        return makeError(ErrorCode::InvalidInput,
                         "readCacheSize must lie between " +
                             std::to_string(carving::SourceContentReader::kMinCacheSize) + " and " +
                             std::to_string(carving::IContentReader::kMaxReadLength));
    }
    if (Status valid = carving::validate(options.carving.scan.reads); !valid.ok()) {
        return valid.error();
    }
    if (Status valid = formats::mp4::validate(options.format.limits); !valid.ok()) {
        return valid.error();
    }
    if (options.maxClusterChecks == 0) {
        return makeError(ErrorCode::InvalidInput, "maxClusterChecks must not be 0");
    }
    auto impl = std::make_unique<Impl>(source, std::move(options));
    impl->nextCarveId = impl->options.carving.firstId;
    return std::unique_ptr<Mp4RecoverySteps>(new Mp4RecoverySteps(std::move(impl)));
}

Mp4RecoverySteps::Mp4RecoverySteps(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Mp4RecoverySteps::~Mp4RecoverySteps() = default;

const Mp4RecoveryOptions& Mp4RecoverySteps::options() const noexcept {
    return impl_->options;
}

Status Mp4RecoverySteps::addVolume(FilesystemRecovery& volume, const CandidateScan& scan) {
    if (scan.volumeOffset != volume.volumeOffset()) {
        return makeError(ErrorCode::InvalidInput, "the candidate scan of the volume at " +
                                                      std::to_string(scan.volumeOffset) +
                                                      " is not from the volume at " +
                                                      std::to_string(volume.volumeOffset()));
    }
    if (!impl_->pending.empty() || impl_->lastExamined.has_value() || impl_->counts.carved > 0) {
        return makeError(ErrorCode::InvalidInput, "volumes are added before the first step");
    }
    impl_->volumes.emplace_back(&volume, &scan);
    return success();
}

std::size_t Mp4RecoverySteps::volumeCount() const noexcept {
    return impl_->volumes.size();
}

Result<Mp4Examination> Mp4RecoverySteps::examine(std::size_t volume, std::size_t index) const {
    const Impl& impl = *impl_;
    if (volume >= impl.volumes.size() || index >= impl.volumes[volume].second->candidates.size()) {
        return makeError(ErrorCode::InvalidInput, "no filesystem candidate " + std::to_string(index) +
                                                      " in volume " + std::to_string(volume));
    }
    if (impl.reads().cancellation.isCancellationRequested()) {
        return makeError(ErrorCode::Cancelled, "MP4 recovery cancelled");
    }
    const RecoveryCandidate& candidate = impl.volumes[volume].second->candidates[index];
    Mp4Examination examination;
    examination.volume = volume;
    examination.index = index;
    const bool named = isVideoExtension(candidate.extension);
    const std::optional<std::uint64_t> start = impl.startOf(candidate, volume);
    const bool located = candidate.bytes(RegionKind::Stored) + candidate.bytes(RegionKind::Embedded) > 0;
    if (!located) {
        if (named && start.has_value()) {
            // An MP4 name without data: a carve starting at its first cluster may be it.
            Mp4PendingCandidate pending;
            pending.candidate.data = candidate;
            pending.candidate.filesystemCandidate = candidate.id;
            pending.candidate.recordedSize = candidate.expectedSize;
            pending.volume = volume;
            pending.start = start;
            examination.pending = std::move(pending);
        }
        return examination;
    }
    Result<std::unique_ptr<CandidateContentReader>> content =
        CandidateContentReader::open(impl.source, candidate, impl.reads(), impl.options.carving.readCacheSize);
    if (!content.ok()) {
        return content.error();
    }
    CandidateContentReader& reader = **content;
    if (!named) {
        // Unnamed: only content that starts like an MP4 file.
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(reader.size(), kSniffLength));
        if (length < formats::Mp4Format::kMinimumSize) {
            return examination;
        }
        Result<std::span<const std::byte>> header = reader.read(0, length);
        if (!header.ok()) {
            return header.error();
        }
        static const formats::Mp4Format sniffer;
        if (!sniffer.checkHeader(*header).plausible) {
            return examination;
        }
    }
    examination.analysed = true;
    Result<Analysis> analysis = analyze(reader, impl.options.format);
    if (!analysis.ok()) {
        return analysis.error();
    }
    if (!named && analysis->structure.kind != mp4::MediaKind::Video) {
        return examination;  // an audio file (M4A's) or an image
    }
    examination.video = true;
    Mp4PendingCandidate pending;
    pending.volume = volume;
    pending.examined = true;
    pending.start = start;
    Mp4Candidate& video = pending.candidate;
    video.data = candidate;
    video.filesystemCandidate = candidate.id;
    video.recordedSize = candidate.expectedSize;
    video.structure = std::move(analysis->structure);
    if (analysis->movie.has_value()) {
        countSamples(video.structure, *analysis->movie, damageOf(candidate, reader));
    }
    // A guessed layout that the structure validates is the metadata's
    // start and the structure's confirmation together, unless another
    // file has taken the first cluster since.
    if (candidate.filesystemEvidence.allocation.layout == LayoutEvidence::Guessed &&
        video.structure.status == mp4::FileStatus::Valid && video.structure.samplesMisframed() == 0 &&
        !startReallocated(candidate)) {
        video.data.method = RecoveryMethod::Hybrid;
    }
    examination.pending = std::move(pending);
    return examination;
}

Status Mp4RecoverySteps::addExamination(Mp4Examination examination) {
    Impl& impl = *impl_;
    const std::pair<std::size_t, std::size_t> at{examination.volume, examination.index};
    if (examination.volume >= impl.volumes.size() ||
        examination.index >= impl.volumes[examination.volume].second->candidates.size() ||
        (impl.lastExamined.has_value() && at <= *impl.lastExamined)) {
        return makeError(ErrorCode::InvalidInput, "examinations are added once each, in scan order");
    }
    if (examination.pending.has_value() && examination.pending->volume != examination.volume) {
        return makeError(ErrorCode::InvalidInput, "an examination's candidate belongs to its volume");
    }
    impl.lastExamined = at;
    impl.counts.filesystemExamined += examination.analysed ? 1 : 0;
    impl.counts.filesystemMp4 += examination.video ? 1 : 0;
    if (examination.pending.has_value()) {
        if (examination.pending->start.has_value()) {
            impl.starts.emplace(*examination.pending->start, impl.pending.size());
        }
        impl.pending.push_back(std::move(*examination.pending));
    }
    return success();
}

const carving::IFileFormat& Mp4RecoverySteps::format() const noexcept {
    return impl_->format;
}

bool Mp4RecoverySteps::skips(std::uint64_t fileOffset) const noexcept {
    return fileOffset > impl_->trustedStart && fileOffset < impl_->trustedEnd;
}

Result<Mp4HitWork> Mp4RecoverySteps::prepare(const carving::SignatureHit& hit) const {
    const Impl& impl = *impl_;
    const carving::FormatDescriptor& descriptor = impl.format.descriptor();
    if (hit.signatureIndex >= descriptor.signatures.size()) {
        return makeError(ErrorCode::InvalidInput, "an MP4 hit names a signature MP4 does not have");
    }
    carving::SignatureHit own = hit;
    own.format = &impl.format;
    own.formatIndex = 0;
    carving::CarveOptions carveOptions = impl.options.carving;
    carveOptions.validate = false;  // each carve is analysed (and validated) here
    carving::FileCarver carver(impl.source, carveOptions);
    Result<carving::CarveOutcome> outcome = carver.carve(own);
    if (!outcome.ok()) {
        return outcome.error();
    }
    Mp4HitWork work;
    work.impl_ = std::make_unique<Mp4HitWork::Impl>();
    Mp4HitWork::Impl& result = *work.impl_;
    result.hit = own;
    if (carving::CarveRejection* rejection = std::get_if<carving::CarveRejection>(&*outcome); rejection != nullptr) {
        result.outcome = std::move(*rejection);
        return work;
    }
    FileCandidate carve = std::get<FileCandidate>(std::move(*outcome));
    const auto [first, last] = impl.starts.equal_range(carve.sourceOffset);

    // A filesystem candidate whose own data validated, stored as one run
    // from the same start and of the carve's length, holds the same bytes:
    // its analysis stands for the carve's, unless another candidate starting
    // here would take the carve's layout, which needs the carve's own
    // analysis. (Commits of other hits change only the candidates that start
    // where those hits do.)
    const Mp4PendingCandidate* same = nullptr;
    for (auto it = first; it != last; ++it) {
        const Mp4PendingCandidate& candidate = impl.pending[it->second];
        const std::vector<SourceRegion>& regions = candidate.candidate.data.sourceRegions;
        const bool oneRun =
            candidate.examined && candidate.candidate.structure.intact() && !regions.empty() &&
            std::all_of(regions.begin(), regions.end(),
                        [&](const SourceRegion& r) {
                            return r.kind == RegionKind::Stored && r.sourceOffset == carve.sourceOffset + r.fileOffset;
                        }) &&
            candidate.candidate.data.expectedSize == carve.length;
        if (oneRun) {
            same = &candidate;
            break;
        }
    }
    const bool othersTakeIt = std::any_of(first, last, [&](const auto& entry) {
        const Mp4PendingCandidate& other = impl.pending[entry.second];
        return &other != same && takesCarveLayout(other, true);
    });
    if (same != nullptr && !othersTakeIt) {
        carve.validation = carving::ValidationResult{ValidationStatus::Valid, carve.length,
                                                     same->candidate.structure.detail};
    } else {
        Result<Analysis> analyzed = impl.analyzeCarve(carve, result.unreadable);
        if (!analyzed.ok()) {
            return analyzed.error();
        }
        result.analysis = std::move(*analyzed);
    }
    result.outcome = std::move(carve);
    return work;
}

Status Mp4RecoverySteps::commit(const carving::SignatureHit& hit, Mp4HitWork* work) {
    Impl& impl = *impl_;
    if (impl.delivered) {
        return makeError(ErrorCode::InvalidInput, "the MP4 steps were delivered already");
    }
    if (skips(hit.fileOffset)) {
        ++impl.counts.hitsSkipped;
        return success();
    }
    Mp4HitWork prepared;
    if (work == nullptr || work->impl_ == nullptr) {
        Result<Mp4HitWork> made = prepare(hit);
        if (!made.ok()) {
            return made.error();
        }
        prepared = std::move(*made);
        work = &prepared;
    }
    if (work->impl_->hit.fileOffset != hit.fileOffset) {
        return makeError(ErrorCode::InvalidInput, "an MP4 hit committed with the work of another hit");
    }
    return impl.apply(*work->impl_);
}

Mp4StepsState Mp4RecoverySteps::state() const {
    const Impl& impl = *impl_;
    Mp4StepsState state;
    state.pending = impl.pending;
    state.carved = impl.carved;
    state.trustedStart = impl.trustedStart;
    state.trustedEnd = impl.trustedEnd;
    state.nextCarveId = impl.nextCarveId;
    state.report = impl.counts;
    return state;
}

Mp4StepsChanges Mp4RecoverySteps::takeChanges() {
    Impl& impl = *impl_;
    Mp4StepsChanges changes;
    for (const std::size_t index : impl.changed) {
        changes.pending.emplace_back(index, impl.pending[index]);
    }
    impl.changed.clear();
    for (std::size_t i = impl.carvedReported; i < impl.carved.size(); ++i) {
        changes.carved.push_back(impl.carved[i]);
    }
    impl.carvedReported = impl.carved.size();
    changes.trustedStart = impl.trustedStart;
    changes.trustedEnd = impl.trustedEnd;
    changes.nextCarveId = impl.nextCarveId;
    changes.report = impl.counts;
    return changes;
}

Status Mp4RecoverySteps::restore(Mp4StepsState state) {
    Impl& impl = *impl_;
    for (const Mp4PendingCandidate& pending : state.pending) {
        if (pending.volume >= impl.volumes.size()) {
            return makeError(ErrorCode::InvalidInput, "a saved MP4 candidate belongs to a volume that was not added");
        }
    }
    if (state.trustedStart > state.trustedEnd) {
        return makeError(ErrorCode::InvalidInput, "a saved MP4 state has an empty trusted range");
    }
    impl.pending = std::move(state.pending);
    impl.starts.clear();
    for (std::size_t i = 0; i < impl.pending.size(); ++i) {
        if (impl.pending[i].start.has_value()) {
            impl.starts.emplace(*impl.pending[i].start, i);
        }
    }
    impl.carved = std::move(state.carved);
    impl.trustedStart = state.trustedStart;
    impl.trustedEnd = state.trustedEnd;
    impl.nextCarveId = state.nextCarveId;
    impl.counts = state.report;
    impl.changed.clear();
    impl.carvedReported = impl.carved.size();
    impl.lastExamined = std::pair<std::size_t, std::size_t>{SIZE_MAX, SIZE_MAX};
    return success();
}

Status Mp4RecoverySteps::deliver(const Mp4CandidateSink& sink, Mp4RecoveryReport& report) {
    Impl& impl = *impl_;
    if (!sink) {
        return makeError(ErrorCode::InvalidInput, "MP4 recovery needs a candidate sink");
    }
    if (impl.delivered) {
        return makeError(ErrorCode::InvalidInput, "the MP4 steps were delivered already");
    }
    impl.delivered = true;
    report.filesystemExamined = impl.counts.filesystemExamined;
    report.filesystemMp4 = impl.counts.filesystemMp4;
    report.carved = impl.counts.carved;
    report.carvesRejected = impl.counts.carvesRejected;
    report.hitsSkipped = impl.counts.hitsSkipped;
    report.carvesMerged = impl.counts.carvesMerged;
    std::uint64_t id = impl.options.firstId;
    const auto send = [&](Mp4Candidate& candidate) -> Status {
        impl.finish(candidate, id++);
        switch (candidate.data.method) {
        case RecoveryMethod::Filesystem:
            ++report.filesystem;
            break;
        case RecoveryMethod::Carving:
            ++report.carving;
            break;
        case RecoveryMethod::Hybrid:
            ++report.hybrid;
            break;
        case RecoveryMethod::Fragmented:
            break;
        }
        switch (candidate.structure.status) {
        case mp4::FileStatus::Valid:
            ++report.valid;
            break;
        case mp4::FileStatus::Truncated:
            ++report.truncated;
            break;
        case mp4::FileStatus::Invalid:
            ++report.invalid;
            break;
        }
        impl.log(LogLevel::Debug, "MP4 candidate",
                 {field("id", candidate.data.id.value()), field("method", toString(candidate.data.method)),
                  field("offset", candidate.data.sourceOffset().value_or(0)),
                  field("size", candidate.data.expectedSize),
                  field("structure", candidate.structure.status == mp4::FileStatus::Valid       ? "valid"
                                     : candidate.structure.status == mp4::FileStatus::Truncated ? "truncated"
                                                                                               : "invalid"),
                  field("samples", candidate.structure.samples()),
                  field("samples_intact", candidate.structure.samplesIntact())});
        return sink(std::move(candidate));
    };
    for (Mp4PendingCandidate& pending : impl.pending) {
        if (!pending.examined && pending.candidate.data.method != RecoveryMethod::Hybrid) {
            continue;  // an MP4 name without data, and no carve starts where it did
        }
        if (Status sent = send(pending.candidate); !sent.ok()) {
            return sent;
        }
    }
    for (Mp4Candidate& candidate : impl.carved) {
        if (Status sent = send(candidate); !sent.ok()) {
            return sent;
        }
    }
    return success();
}

// ---------------------------------------------------------------------------
// Mp4Recovery: the steps, one after the other
// ---------------------------------------------------------------------------

Mp4Recovery::Mp4Recovery(storage::IStorageSource& source, Mp4RecoveryOptions options)
    : source_(&source), options_(std::move(options)) {}

Status Mp4Recovery::addVolume(FilesystemRecovery& volume, const CandidateScan& scan) {
    if (scan.volumeOffset != volume.volumeOffset()) {
        return makeError(ErrorCode::InvalidInput, "the candidate scan of the volume at " +
                                                      std::to_string(scan.volumeOffset) +
                                                      " is not from the volume at " +
                                                      std::to_string(volume.volumeOffset()));
    }
    volumes_.push_back(Volume{&volume, &scan});
    return success();
}

Result<Mp4RecoveryReport> Mp4Recovery::run(const Mp4CandidateSink& sink) {
    if (!sink) {
        return makeError(ErrorCode::InvalidInput, "MP4 recovery needs a candidate sink");
    }
    Result<std::unique_ptr<Mp4RecoverySteps>> created = Mp4RecoverySteps::create(*source_, options_);
    if (!created.ok()) {
        return created.error();
    }
    Mp4RecoverySteps& steps = **created;
    for (const Volume& volume : volumes_) {
        if (Status added = steps.addVolume(*volume.recovery, *volume.scan); !added.ok()) {
            return added.error();
        }
    }
    const auto started = std::chrono::steady_clock::now();
    diagnostics::Logger* logger = options_.carving.scan.logger;
    if (logger != nullptr) {
        logger->log(LogLevel::Info, kComponent, "MP4 recovery started",
                    {field("volumes", volumes_.size()), field("filesystem", options_.useFilesystem ? "yes" : "no"),
                     field("carve", options_.carve ? "yes" : "no")});
    }
    Mp4RecoveryReport report;
    if (options_.useFilesystem) {
        for (std::size_t v = 0; v < volumes_.size(); ++v) {
            for (std::size_t i = 0; i < volumes_[v].scan->candidates.size(); ++i) {
                Result<Mp4Examination> examined = steps.examine(v, i);
                if (!examined.ok()) {
                    return examined.error();
                }
                if (Status added = steps.addExamination(std::move(*examined)); !added.ok()) {
                    return added.error();
                }
            }
        }
    }
    if (options_.carve) {
        carving::FormatRegistry registry;
        if (Status added = registry.add(std::make_shared<formats::Mp4Format>(options_.format)); !added.ok()) {
            return added.error();
        }
        Result<carving::SignatureScanner> scanner = carving::SignatureScanner::create(registry);
        if (!scanner.ok()) {
            return scanner.error();
        }
        const carving::HitSink onHit = [&](const carving::SignatureHit& hit) -> Status {
            return steps.commit(hit, nullptr);
        };
        Result<carving::ScanReport> scan = scanner->scan(*source_, onHit, options_.carving.scan);
        if (!scan.ok()) {
            return scan.error();
        }
        report.scan = std::move(*scan);
        if (report.scan.outcome == carving::ScanOutcome::Cancelled) {
            return makeError(ErrorCode::Cancelled, "MP4 recovery cancelled");
        }
    }
    if (Status delivered = steps.deliver(sink, report); !delivered.ok()) {
        return delivered.error();
    }
    report.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (logger != nullptr) {
        logger->log(LogLevel::Info, kComponent, "MP4 recovery ended",
                    {field("examined", report.filesystemExamined), field("filesystem", report.filesystem),
                     field("hybrid", report.hybrid), field("carving", report.carving),
                     field("carved", report.carved), field("merged", report.carvesMerged),
                     field("valid", report.valid), field("truncated", report.truncated),
                     field("invalid", report.invalid), field("elapsed_ms", report.elapsed.count())});
    }
    return report;
}

}  // namespace recovery
