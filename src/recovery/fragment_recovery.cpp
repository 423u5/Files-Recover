#include "recovery/fragment_recovery.hpp"

#include "carving/content_reader.hpp"
#include "diagnostics/logger.hpp"
#include "formats/m4a_format.hpp"
#include "formats/mp4_analysis.hpp"
#include "formats/mp4_parser.hpp"
#include "fragment_evidence.hpp"
#include "fragment_search.hpp"
#include "recovery/candidate_content.hpp"
#include "recovery/checked_math.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>

namespace recovery {

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

std::string_view toString(ReconstructionStatus status) noexcept {
    switch (status) {
    case ReconstructionStatus::Complete:
        return "COMPLETE";
    case ReconstructionStatus::Partial:
        return "PARTIAL";
    case ReconstructionStatus::Corrupted:
        return "CORRUPTED";
    case ReconstructionStatus::Ambiguous:
        return "AMBIGUOUS";
    case ReconstructionStatus::Unrecoverable:
        return "UNRECOVERABLE";
    }
    return "Unknown";
}

std::string_view toString(SeedOrigin origin) noexcept {
    switch (origin) {
    case SeedOrigin::Filesystem:
        return "filesystem";
    case SeedOrigin::Carving:
        return "carving";
    }
    return "Unknown";
}

std::string_view toString(LayoutSource source) noexcept {
    switch (source) {
    case LayoutSource::Contiguous:
        return "contiguous";
    case LayoutSource::SkipAllocated:
        return "skip-allocated";
    case LayoutSource::SkipClaimed:
        return "skip-claimed";
    case LayoutSource::GapSearch:
        return "gap-search";
    case LayoutSource::SampleTables:
        return "sample-tables";
    }
    return "Unknown";
}

const ReconstructionHypothesis* FragmentCandidate::reconstruction() const noexcept {
    if (status == ReconstructionStatus::Ambiguous || status == ReconstructionStatus::Unrecoverable ||
        hypotheses.empty()) {
        return nullptr;
    }
    return &hypotheses.front();
}

Status validate(const FragmentSearchLimits& limits) {
    const std::array<std::pair<std::string_view, std::uint64_t>, 13> values = {{
        {"maxFragments", limits.maxFragments},
        {"minimumContinuation", limits.minimumContinuation},
        {"maxBoundaries", limits.maxBoundaries},
        {"maxContinuations", limits.maxContinuations},
        {"maxSearchClusters", limits.maxSearchClusters},
        {"beamWidth", limits.beamWidth},
        {"maxValidations", limits.maxValidations},
        {"maxReadBytes", limits.maxReadBytes},
        {"maxUnknownLength", limits.maxUnknownLength},
        {"maxMovieAnchors", limits.maxMovieAnchors},
        {"probeSamples", limits.probeSamples},
        {"maxSampleProbes", limits.maxSampleProbes},
        {"maxAlternatives", limits.maxAlternatives},
    }};
    for (const auto& [name, value] : values) {
        if (value == 0) {
            return makeError(ErrorCode::InvalidInput, "fragment search limit " + std::string(name) + " must not be 0");
        }
    }
    return success();
}

namespace {

namespace mp4 = formats::mp4;
using carving::FileCandidate;
using carving::ValidationStatus;
using diagnostics::field;
using diagnostics::LogLevel;
using fragments::Assessment;
using fragments::Geometry;
using fragments::Hypothesis;
using fragments::Layout;
using fragments::OwnerId;
using fragments::Run;
using fragments::Search;
using fragments::Seed;
using fragments::VolumeEvidence;

constexpr std::string_view kComponent = "fragments";
// Largest header read to tell a file's format by its content.
constexpr std::size_t kMaxHeader = 64 * kKiB;
// Chunk of a content comparison.
constexpr std::size_t kCompareChunk = 256 * kKiB;

std::string plural(std::uint64_t count, std::string_view one, std::string_view many) {
    return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

// "recovered_000012.jpg"
std::string carvedName(CandidateId id, std::string_view extension) {
    std::string number = std::to_string(id.value());
    if (number.size() < 6) {
        number.insert(0, 6 - number.size(), '0');
    }
    return "recovered_" + number + "." + std::string(extension);
}

// ---------------------------------------------------------------------------
// The moov probe: finds moov boxes (for MP4 and M4A files whose moov follows
// their media data) during the scan. It is never carved.
// ---------------------------------------------------------------------------

class MoovProbe final : public carving::IFileFormat, public carving::FormatValidator {
public:
    MoovProbe() {
        descriptor_.id = "fragments-moov-probe";
        descriptor_.name = "moov box (fragment reconstruction)";
        descriptor_.extension = "moov";
        descriptor_.signatures = {carving::textSignature("moov box", "moov", 4)};
        descriptor_.minimumSize = 16;
        descriptor_.maximumSize = 1ULL << 32;
        descriptor_.headerSize = 16;
        descriptor_.endDetection = carving::EndDetectionMethod::StructureWalk;
    }

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte>) const override {
        return carving::HeaderCheck::accept();
    }
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader&) const override {
        return carving::EndDetection{carving::EndStatus::Broken, 0, "never carved"};
    }
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader&) const override {
        return carving::ValidationResult{ValidationStatus::Invalid, 0, "never carved"};
    }

private:
    carving::FormatDescriptor descriptor_;
};

// Extensions that name the formats' files besides their descriptor's own.
constexpr std::array<std::pair<std::string_view, std::string_view>, 13> kExtensionAliases = {{
    {"jpeg", "jpg"},
    {"jpe", "jpg"},
    {"jfif", "jpg"},
    {"mov", "mp4"},
    {"qt", "mp4"},
    {"m4v", "mp4"},
    {"3gp", "mp4"},
    {"3g2", "mp4"},
    {"3gpp", "mp4"},
    {"3gp2", "mp4"},
    {"m4b", "m4a"},
    {"m4p", "m4a"},
    {"adts", "aac"},
}};

// ---------------------------------------------------------------------------
// The damage of a delivered layout, in file ranges (for MP4 sample counts).
// ---------------------------------------------------------------------------

enum class Damage : std::uint8_t { Reallocated = 0, Unreadable = 1, Missing = 2 };

struct DamagedRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    Damage damage = Damage::Missing;
};

// The file ranges of `data` that are missing, unreadable (as read through
// `content`) or in clusters in use now.
std::vector<DamagedRange> damageOf(const RecoveryCandidate& data, const CandidateContentReader& content) {
    std::vector<DamagedRange> ranges;
    for (const SourceRegion& region : data.sourceRegions) {
        const std::uint64_t end = region.fileOffset + region.length;
        if (region.kind == RegionKind::Missing || region.kind == RegionKind::Zeros) {
            ranges.push_back(DamagedRange{region.fileOffset, end, Damage::Missing});
        } else if (region.kind == RegionKind::Stored) {
            if (region.reallocated) {
                ranges.push_back(DamagedRange{region.fileOffset, end, Damage::Reallocated});
            }
            for (const storage::BadRegion& bad : content.unreadable().overlapping(region.sourceOffset, region.length)) {
                const std::uint64_t from = std::max(bad.offset, region.sourceOffset);
                const std::uint64_t to = std::min(bad.end(), region.sourceOffset + region.length);
                if (from < to) {
                    ranges.push_back(DamagedRange{region.fileOffset + (from - region.sourceOffset),
                                                  region.fileOffset + (to - region.sourceOffset), Damage::Unreadable});
                }
            }
        }
    }
    for (const auto& [begin, end] : content.outsideSource()) {
        ranges.push_back(DamagedRange{begin, end, Damage::Unreadable});
    }
    return ranges;
}

// Stored bytes of `data` that `content` could not read.
std::uint64_t unreadableBytesOf(const RecoveryCandidate& data, const CandidateContentReader& content) {
    std::uint64_t total = 0;
    for (const DamagedRange& range : damageOf(data, content)) {
        if (range.damage == Damage::Unreadable) {
            total += range.end - range.begin;
        }
    }
    return total;
}

// P12's structure evidence for a layout, with each sample counted once under
// the worst damage of the bytes it covers.
Result<Mp4Structure> mp4StructureOf(CandidateContentReader& content, const RecoveryCandidate& data,
                                    const formats::Mp4FormatOptions& options) {
    Result<Mp4Structure> analyzed = analyzeMp4(content, options);
    if (!analyzed.ok()) {
        return analyzed.error();
    }
    Mp4Structure structure = std::move(*analyzed);
    Result<mp4::Mp4File> parsed = mp4::parseFile(content, options.limits);
    if (!parsed.ok()) {
        return parsed.error();
    }
    std::optional<mp4::Movie> movie = std::move(parsed->movie);
    if (!movie.has_value() && structure.moovFoundBySearch) {
        Result<std::optional<mp4::FoundMovie>> found = mp4::findMovie(content, 0, content.size(), options.limits);
        if (!found.ok()) {
            return found.error();
        }
        if (found->has_value()) {
            movie = std::move((*found)->movie);
        }
    }
    if (!movie.has_value()) {
        return structure;
    }
    const std::vector<DamagedRange> ranges = damageOf(data, content);
    for (Mp4TrackEvidence& track : structure.tracks) {
        track.samplesBeyondData = 0;
        track.samplesMissing = 0;
        track.samplesUnreadable = 0;
        track.samplesReallocated = 0;
        track.samplesIntact = 0;
        if (track.number == 0 || track.number > movie->tracks.size()) {
            continue;
        }
        mp4::forEachSample(movie->tracks[track.number - 1], [&](std::uint32_t, std::uint64_t offset,
                                                                std::uint32_t size) {
            const std::uint64_t end = offset + size;
            if (end > structure.dataSize) {
                ++track.samplesBeyondData;
                return;
            }
            std::optional<Damage> worst;
            for (const DamagedRange& range : ranges) {
                if (range.begin < end && offset < range.end && (!worst.has_value() || range.damage > *worst)) {
                    worst = range.damage;
                }
            }
            if (!worst.has_value()) {
                ++track.samplesIntact;
            } else if (*worst == Damage::Missing) {
                ++track.samplesMissing;
            } else if (*worst == Damage::Unreadable) {
                ++track.samplesUnreadable;
            } else {
                ++track.samplesReallocated;
            }
        });
    }
    return structure;
}

// A file's content with the bytes [begin, end) replaced by others: what the
// validator makes of a layout whose cluster there holds other data. The
// replacement is a fixed pseudo-random pattern with the bytes FF 01 every 64
// bytes, as compressed data of a few kilobytes almost always holds (a
// cluster of 512 bytes of random data holds no FF about one time in eight).
class SubstitutedReader final : public carving::IContentReader {
public:
    SubstitutedReader(carving::IContentReader& inner, std::uint64_t begin, std::uint64_t end) noexcept
        : inner_(&inner), begin_(begin), end_(std::max(begin, end)) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return inner_->size(); }
    [[nodiscard]] Result<std::span<const std::byte>> read(std::uint64_t offset, std::size_t length) override {
        Result<std::span<const std::byte>> bytes = inner_->read(offset, length);
        if (!bytes.ok() || offset >= end_ || offset + length <= begin_) {
            return bytes;
        }
        buffer_.assign(bytes->begin(), bytes->end());
        const std::uint64_t from = std::max(offset, begin_);
        const std::uint64_t to = std::min<std::uint64_t>(offset + length, end_);
        for (std::uint64_t at = from; at < to; ++at) {
            buffer_[static_cast<std::size_t>(at - offset)] = patternAt(at - begin_);
        }
        return std::span<const std::byte>(buffer_);
    }

private:
    // SplitMix64 of the position's 8-byte word, one byte of it; FF 01 every 64 bytes.
    static std::byte patternAt(std::uint64_t position) noexcept {
        if (position % 64 == 0) {
            return std::byte{0xFF};
        }
        if (position % 64 == 1) {
            return std::byte{0x01};
        }
        std::uint64_t z = (position / 8 + 1) * 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        return static_cast<std::byte>((z >> (8 * (position % 8))) & 0xFF);
    }

    carving::IContentReader* inner_;
    std::uint64_t begin_;
    std::uint64_t end_;
    std::vector<std::byte> buffer_;
};

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

class RunState {
public:
    RunState(storage::IStorageSource& source, const carving::FormatRegistry& formats,
             const FragmentRecoveryOptions& options)
        : source_(source), formats_(formats), options_(options), probe_(std::make_shared<MoovProbe>()) {
        carving::CarveOptions carveOptions = options_.carving;
        carveOptions.validate = true;
        carver_ = std::make_unique<carving::FileCarver>(source_, std::move(carveOptions));
        nextCarveId_ = options_.carving.firstId;
        nextId_ = options_.firstId;
    }

    [[nodiscard]] Status addVolume(FilesystemRecovery& recovery, const CandidateScan& scan);
    [[nodiscard]] std::size_t volumeCount() const noexcept { return volumes_.size(); }
    [[nodiscard]] Status begin();

    [[nodiscard]] Result<FragmentSeedExamination> examineSeed(std::size_t volume, std::size_t index) const;
    [[nodiscard]] Status addSeedExamination(FragmentSeedExamination examination);

    [[nodiscard]] std::vector<std::shared_ptr<const carving::IFileFormat>> extraFormats() const;
    [[nodiscard]] bool wants(const carving::SignatureHit& hit);
    [[nodiscard]] Status commit(const carving::SignatureHit& hit, const carving::CarveOutcome* outcome) {
        return commitHit(hit, outcome, false);
    }
    void recordEvents(bool on) noexcept { recording_ = on; }
    [[nodiscard]] std::vector<FragmentPassEvent> takeEvents() { return std::exchange(events_, {}); }
    [[nodiscard]] Status replay(const std::vector<FragmentPassEvent>& events);

    [[nodiscard]] std::size_t seedCount() const noexcept { return filesystemSeeds_.size() + carveSeeds_.size(); }
    [[nodiscard]] std::size_t nextSeed() const noexcept { return cursor_; }
    [[nodiscard]] Result<std::optional<FragmentCandidate>> reconstructNext();
    [[nodiscard]] Status replayReconstruction(const FragmentCandidate& candidate);

    [[nodiscard]] const FragmentRecoveryReport& report() const noexcept { return report_; }

private:
    // Where a hit of the pass lands: its volume and cluster.
    struct HitPlace {
        std::size_t volume = 0;
        std::uint64_t index = 0;
    };

    [[nodiscard]] const carving::SourceReadOptions& reads() const noexcept { return options_.carving.scan.reads; }
    void log(LogLevel level, std::string_view message, std::initializer_list<diagnostics::LogField> fields) const {
        if (options_.carving.scan.logger != nullptr) {
            options_.carving.scan.logger->log(level, kComponent, message, fields);
        }
    }
    [[nodiscard]] Status checkCancelled() const {
        if (reads().cancellation.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "fragment reconstruction cancelled");
        }
        return success();
    }
    [[nodiscard]] std::optional<std::size_t> volumeAt(std::uint64_t sourceOffset) const;
    [[nodiscard]] const carving::IFileFormat* formatForExtension(std::string_view extension) const;
    [[nodiscard]] Result<const carving::IFileFormat*> formatByContent(const RecoveryCandidate& candidate) const;
    // A format of the registry that the pass carves.
    [[nodiscard]] bool carves(const carving::IFileFormat* format) const;
    // Where a carve of the hit would go: a free cluster's start of a volume,
    // not inside a carve that validated, nor inside its own format's stream.
    [[nodiscard]] std::optional<HitPlace> carvePlace(const carving::SignatureHit& hit);
    [[nodiscard]] Status commitHit(const carving::SignatureHit& hit, const carving::CarveOutcome* outcome,
                                   bool replaying);
    void applyCarve(const carving::SignatureHit& hit, const HitPlace& place, FileCandidate carve, bool replaying);
    void record(const carving::SignatureHit& hit, const FileCandidate* carve);
    // The delivery counts of one candidate.
    void count(const FragmentCandidate& candidate);
    [[nodiscard]] Seed& seedAt(std::size_t position) {
        return position < filesystemSeeds_.size() ? filesystemSeeds_[position]
                                                  : carveSeeds_[position - filesystemSeeds_.size()];
    }

    void claimVolumes();
    [[nodiscard]] Result<FragmentCandidate> reconstruct(Seed& seed);
    [[nodiscard]] Result<ReconstructionHypothesis> deliver(Search& search, Hypothesis& hypothesis,
                                                           const FragmentCandidate& candidate, bool full);
    [[nodiscard]] Result<bool> sameContent(Search& search, Hypothesis& a, Hypothesis& b);
    [[nodiscard]] Result<bool> dataChecked(Search& search, const Hypothesis& hypothesis, std::uint64_t length);

    storage::IStorageSource& source_;
    const carving::FormatRegistry& formats_;
    const FragmentRecoveryOptions& options_;
    std::vector<std::unique_ptr<VolumeEvidence>> volumes_;
    // Owner ids: each volume's candidates get consecutive ids from their base.
    std::vector<OwnerId> ownerBase_;
    OwnerId nextOwner_ = 1;
    std::vector<std::vector<std::uint64_t>> moovAnchors_;
    std::vector<Seed> filesystemSeeds_;
    // (volume, first cluster) -> filesystem seed.
    std::map<std::pair<std::size_t, std::uint64_t>, std::size_t> seedStarts_;
    std::vector<Seed> carveSeeds_;
    // (volume, first cluster) -> the owner of the carves that start there.
    std::map<std::pair<std::size_t, std::uint64_t>, OwnerId> carveOwners_;
    std::uint64_t nextId_ = 1;

    bool begun_ = false;
    // The last seed examination added, to keep them in scan order.
    std::optional<std::pair<std::size_t, std::size_t>> lastExamined_;
    // The pass: the moov probe, the carver, the carve that validated reaching
    // furthest (hits strictly inside it are its own) and, per
    // self-synchronizing format, the carve reaching furthest (its own frames
    // are not files).
    std::shared_ptr<MoovProbe> probe_;
    std::unique_ptr<carving::FileCarver> carver_;
    std::uint64_t trustedStart_ = 0;
    std::uint64_t trustedEnd_ = 0;
    std::map<const carving::IFileFormat*, std::pair<std::uint64_t, std::uint64_t>> streams_;
    // Id of the next carve that is not rejected, as one carver numbers them.
    std::uint64_t nextCarveId_ = 1;
    bool recording_ = false;
    std::vector<FragmentPassEvent> events_;
    // The next seed to reconstruct.
    std::size_t cursor_ = 0;
    FragmentRecoveryReport report_;
};

Status RunState::addVolume(FilesystemRecovery& recovery, const CandidateScan& scan) {
    if (begun_) {
        return makeError(ErrorCode::InvalidInput, "volumes are added before fragment reconstruction begins");
    }
    const std::optional<Geometry> geometry = Geometry::of(recovery);
    if (!geometry.has_value()) {
        return makeError(ErrorCode::InvalidInput, "the volume at " + std::to_string(recovery.volumeOffset()) +
                                                      " has no usable cluster area");
    }
    volumes_.push_back(std::make_unique<VolumeEvidence>(recovery, scan, *geometry));
    ownerBase_.push_back(nextOwner_);
    nextOwner_ += scan.candidates.size();
    moovAnchors_.emplace_back();
    return success();
}

std::optional<std::size_t> RunState::volumeAt(std::uint64_t sourceOffset) const {
    for (std::size_t v = 0; v < volumes_.size(); ++v) {
        if (volumes_[v]->geometry().indexAt(sourceOffset).has_value()) {
            return v;
        }
    }
    return std::nullopt;
}

const carving::IFileFormat* RunState::formatForExtension(std::string_view extension) const {
    if (extension.empty()) {
        return nullptr;
    }
    std::string_view wanted = extension;
    for (const auto& [alias, target] : kExtensionAliases) {
        if (alias == extension) {
            wanted = target;
        }
    }
    for (const std::shared_ptr<const carving::IFileFormat>& format : formats_.formats()) {
        if (format->descriptor().extension == wanted) {
            return format.get();
        }
    }
    return nullptr;
}

Result<const carving::IFileFormat*> RunState::formatByContent(const RecoveryCandidate& candidate) const {
    Result<std::unique_ptr<CandidateContentReader>> reader =
        CandidateContentReader::open(source_, candidate, reads(), options_.carving.readCacheSize);
    if (!reader.ok()) {
        return reader.error();
    }
    const auto length = static_cast<std::size_t>(std::min<std::uint64_t>((*reader)->size(), kMaxHeader));
    if (length == 0) {
        return static_cast<const carving::IFileFormat*>(nullptr);
    }
    Result<std::span<const std::byte>> header = (*reader)->read(0, length);
    if (!header.ok()) {
        return header.error();
    }
    const carving::IFileFormat* byExtension = formatForExtension(candidate.extension);
    const carving::IFileFormat* first = nullptr;
    for (const std::shared_ptr<const carving::IFileFormat>& format : formats_.formats()) {
        const carving::FormatDescriptor& descriptor = format->descriptor();
        const bool matches = std::any_of(descriptor.signatures.begin(), descriptor.signatures.end(),
                                         [&](const carving::FileSignature& signature) {
                                             return signature.reach() <= header->size() &&
                                                    signature.matches(header->subspan(signature.offset));
                                         });
        if (!matches) {
            continue;
        }
        const std::size_t headerLength = std::min<std::size_t>(descriptor.headerSize, header->size());
        if (!format->checkHeader(header->first(headerLength)).plausible) {
            continue;
        }
        if (format.get() == byExtension) {
            return format.get();
        }
        if (first == nullptr) {
            first = format.get();
        }
    }
    return first;
}

void RunState::claimVolumes() {
    for (std::size_t v = 0; v < volumes_.size(); ++v) {
        VolumeEvidence& volume = *volumes_[v];
        const std::vector<RecoveryCandidate>& candidates = volume.scan().candidates;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const RecoveryCandidate& candidate = candidates[i];
            const OwnerId owner = ownerBase_[v] + i;
            const LayoutEvidence layout = candidate.filesystemEvidence.allocation.layout;
            if (!candidate.isDeleted() || layout == LayoutEvidence::Recorded) {
                // An active file's data, or a deleted file's recorded layout.
                volume.claimRegions(volume.strong(), candidate.sourceRegions, owner);
            } else if (layout == LayoutEvidence::Guessed) {
                // A deleted file's first cluster; the rest of its guess only softly.
                const std::optional<std::uint64_t> start = candidate.sourceOffset();
                if (start.has_value()) {
                    if (const std::optional<std::uint64_t> index = volume.geometry().indexAt(*start)) {
                        volume.strong().add(*index, *index + 1, owner);
                    }
                }
                volume.claimRegions(volume.soft(), candidate.sourceRegions, owner);
            }
        }
    }
}

Status RunState::begin() {
    if (begun_) {
        return makeError(ErrorCode::InvalidInput, "fragment reconstruction has begun already");
    }
    begun_ = true;
    claimVolumes();
    return success();
}

Result<FragmentSeedExamination> RunState::examineSeed(std::size_t v, std::size_t i) const {
    if (!begun_) {
        return makeError(ErrorCode::InvalidInput, "fragment reconstruction has not begun");
    }
    if (v >= volumes_.size() || i >= volumes_[v]->scan().candidates.size()) {
        return makeError(ErrorCode::InvalidInput,
                         "no filesystem candidate " + std::to_string(i) + " in volume " + std::to_string(v));
    }
    if (Status cancelled = checkCancelled(); !cancelled.ok()) {
        return cancelled.error();
    }
    VolumeEvidence& volume = *volumes_[v];
    const RecoveryCandidate& candidate = volume.scan().candidates[i];
    FragmentSeedExamination examination;
    examination.volume = v;
    examination.index = i;
    if (candidate.filesystemEvidence.allocation.layout != LayoutEvidence::Guessed) {
        return examination;
    }
    const std::optional<std::uint64_t> startOffset = candidate.sourceOffset();
    const std::optional<std::uint64_t> start =
        startOffset.has_value() ? volume.geometry().indexAt(*startOffset) : std::nullopt;
    if (!start.has_value()) {
        examination.skipped = true;
        return examination;
    }
    const carving::IFileFormat* format = nullptr;
    const carving::IFileFormat* byName = formatForExtension(candidate.extension);
    if (volume.inUse(*start)) {
        // What starts there is another file's now: only a name says what this one was.
        if (byName == nullptr) {
            examination.skipped = true;
            return examination;
        }
        format = byName;
        examination.unrecoverable = "its first cluster is allocated to other data now";
    } else {
        Result<const carving::IFileFormat*> byContent = formatByContent(candidate);
        if (!byContent.ok()) {
            return byContent.error();
        }
        if (*byContent != nullptr) {
            format = *byContent;
        } else if (byName != nullptr) {
            format = byName;
            examination.unrecoverable = "its first cluster does not start a file of the format its name says (" +
                                        byName->descriptor().name + ")";
        } else {
            examination.skipped = true;
            return examination;
        }
    }
    if (candidate.filesystemEvidence.allocation.hasIssue(filesystem::AllocationIssue::SizeExceedsVolume)) {
        examination.unrecoverable = "its recorded size is larger than the volume";
    }
    examination.seed = true;
    examination.formatId = format->descriptor().id;
    return examination;
}

Status RunState::addSeedExamination(FragmentSeedExamination examination) {
    const std::pair<std::size_t, std::size_t> at{examination.volume, examination.index};
    if (!begun_ || examination.volume >= volumes_.size() ||
        examination.index >= volumes_[examination.volume]->scan().candidates.size() ||
        (lastExamined_.has_value() && at <= *lastExamined_) || (examination.seed && examination.skipped)) {
        return makeError(ErrorCode::InvalidInput, "seed examinations are added once each, in scan order");
    }
    lastExamined_ = at;
    if (examination.skipped) {
        ++report_.filesystemSkipped;
        return success();
    }
    if (!examination.seed) {
        return success();
    }
    const carving::IFileFormat* format = formats_.find(examination.formatId);
    VolumeEvidence& volume = *volumes_[examination.volume];
    const RecoveryCandidate& candidate = volume.scan().candidates[examination.index];
    const std::optional<std::uint64_t> startOffset = candidate.sourceOffset();
    const std::optional<std::uint64_t> start =
        startOffset.has_value() ? volume.geometry().indexAt(*startOffset) : std::nullopt;
    if (format == nullptr || !start.has_value()) {
        return makeError(ErrorCode::InvalidInput, "a seed examination does not fit its candidate");
    }
    Seed seed;
    seed.origin = SeedOrigin::Filesystem;
    seed.volume = examination.volume;
    seed.start = *start;
    seed.size = candidate.expectedSize;
    seed.owner = ownerBase_[examination.volume] + examination.index;
    seed.metadata = &candidate;
    seed.format = format;
    seed.unrecoverable = std::move(examination.unrecoverable);
    seedStarts_.emplace(std::make_pair(examination.volume, *start), filesystemSeeds_.size());
    filesystemSeeds_.push_back(std::move(seed));
    ++report_.filesystemSeeds;
    return success();
}

std::vector<std::shared_ptr<const carving::IFileFormat>> RunState::extraFormats() const {
    const bool isoSeeds = std::any_of(filesystemSeeds_.begin(), filesystemSeeds_.end(),
                                      [](const Seed& seed) { return fragments::isIsoFormat(*seed.format); });
    const bool isoFormats = std::any_of(formats_.formats().begin(), formats_.formats().end(),
                                        [](const auto& format) { return fragments::isIsoFormat(*format); });
    if ((options_.carve && isoFormats) || isoSeeds) {
        return {probe_};
    }
    return {};
}

bool RunState::carves(const carving::IFileFormat* format) const {
    if (!options_.carve || format == nullptr) {
        return false;
    }
    return std::any_of(formats_.formats().begin(), formats_.formats().end(),
                       [&](const auto& registered) { return registered.get() == format; });
}

std::optional<RunState::HitPlace> RunState::carvePlace(const carving::SignatureHit& hit) {
    if (!carves(hit.format)) {
        return std::nullopt;
    }
    const std::optional<std::size_t> v = volumeAt(hit.fileOffset);
    if (!v.has_value()) {
        return std::nullopt;
    }
    VolumeEvidence& volume = *volumes_[*v];
    const Geometry& geometry = volume.geometry();
    const std::uint64_t index = *geometry.indexAt(hit.fileOffset);
    // Files start at a cluster; clusters in use hold other data now.
    if (!geometry.startsCluster(hit.fileOffset) || volume.inUse(index)) {
        return std::nullopt;
    }
    if (hit.fileOffset > trustedStart_ && hit.fileOffset < trustedEnd_) {
        return std::nullopt;
    }
    if (hit.format->descriptor().selfSynchronizing) {
        if (const auto stream = streams_.find(hit.format); stream != streams_.end() &&
                                                           hit.fileOffset > stream->second.first &&
                                                           hit.fileOffset < stream->second.second) {
            return std::nullopt;
        }
    }
    return HitPlace{*v, index};
}

bool RunState::wants(const carving::SignatureHit& hit) {
    return begun_ && carvePlace(hit).has_value();
}

void RunState::record(const carving::SignatureHit& hit, const FileCandidate* carve) {
    if (!recording_) {
        return;
    }
    FragmentPassEvent event;
    event.fileOffset = hit.fileOffset;
    event.formatId = hit.format->descriptor().id;
    event.signatureIndex = hit.signatureIndex;
    if (carve != nullptr) {
        event.carve = *carve;
    }
    events_.push_back(std::move(event));
}

Status RunState::commitHit(const carving::SignatureHit& hit, const carving::CarveOutcome* outcome, bool replaying) {
    const auto mismatch = [&]() -> Status {
        if (!replaying) {
            return success();
        }
        return makeError(ErrorCode::InvalidInput, "a saved hit at " + std::to_string(hit.fileOffset) +
                                                      " does not fit the volumes as they are now");
    };
    if (!begun_) {
        return makeError(ErrorCode::InvalidInput, "fragment reconstruction has not begun");
    }
    if (hit.format == probe_.get()) {
        const std::optional<std::size_t> v = volumeAt(hit.fileOffset);
        if (!v.has_value()) {
            return mismatch();
        }
        VolumeEvidence& volume = *volumes_[*v];
        // A moov of a file whose clusters are free.
        if (volume.inUse(*volume.geometry().indexAt(hit.fileOffset))) {
            return mismatch();
        }
        moovAnchors_[*v].push_back(hit.fileOffset);
        if (!replaying) {
            record(hit, nullptr);
        }
        return success();
    }
    const std::optional<HitPlace> place = carvePlace(hit);
    if (!place.has_value()) {
        return mismatch();
    }
    Result<carving::CarveOutcome> made = carving::CarveOutcome(carving::CarveRejection{});
    if (outcome == nullptr) {
        if (replaying) {
            return mismatch();
        }
        made = carver_->carve(hit);
        if (!made.ok()) {
            return made.error();
        }
        outcome = &*made;
    }
    const FileCandidate* carve = std::get_if<FileCandidate>(outcome);
    if (carve == nullptr) {
        return mismatch();  // rejected: nothing changes
    }
    if (carve->sourceOffset != hit.fileOffset) {
        return makeError(ErrorCode::InvalidInput, "a carve committed at another hit's offset");
    }
    applyCarve(hit, *place, *carve, replaying);
    return success();
}

void RunState::applyCarve(const carving::SignatureHit& hit, const HitPlace& place, FileCandidate carve,
                          bool replaying) {
    VolumeEvidence& volume = *volumes_[place.volume];
    const Geometry& geometry = volume.geometry();
    const std::uint64_t index = place.index;
    carve.id = carving::FileCandidateId{nextCarveId_++};
    ++report_.carved;
    if (!replaying) {
        record(hit, &carve);
    }
    if (hit.format->descriptor().selfSynchronizing) {
        std::pair<std::uint64_t, std::uint64_t>& stream = streams_[hit.format];
        if (carve.sourceEnd() > stream.second) {
            stream = {carve.sourceOffset, carve.sourceEnd()};
        }
    }
    const bool valid =
        carve.end.status == carving::EndStatus::Found && carve.validation.status == ValidationStatus::Valid;
    const auto seeded = seedStarts_.find(std::make_pair(place.volume, index));
    OwnerId owner = 0;
    if (seeded != seedStarts_.end()) {
        owner = filesystemSeeds_[seeded->second].owner;
    } else if (const auto known = carveOwners_.find(std::make_pair(place.volume, index));
               known != carveOwners_.end()) {
        owner = known->second;
    } else {
        owner = nextOwner_++;
        carveOwners_.emplace(std::make_pair(place.volume, index), owner);
    }
    volume.strong().add(index, index + 1, owner);
    const auto clustersOf = [&](std::uint64_t length) {
        return geometry.clustersOf(carve.sourceOffset, carve.sourceOffset + length);
    };
    if (valid) {
        ++report_.carvesValid;
        if (const auto clusters = clustersOf(carve.length)) {
            volume.strong().add(clusters->first, clusters->second, owner);
        }
        if (carve.sourceEnd() > trustedEnd_) {
            trustedStart_ = carve.sourceOffset;
            trustedEnd_ = carve.sourceEnd();
        }
    } else if (const auto clusters = clustersOf(carve.validation.validBytes)) {
        volume.soft().add(clusters->first, clusters->second, owner);
    }
    if (seeded != seedStarts_.end()) {
        // The same file as a filesystem seed: evidence of it, not a seed of its own.
        filesystemSeeds_[seeded->second].carve = std::move(carve);
        return;
    }
    const bool broken = carve.end.status == carving::EndStatus::Broken ||
                        (carve.end.status == carving::EndStatus::Found &&
                         carve.validation.status == ValidationStatus::Invalid);
    if (!valid && broken && options_.carve) {
        Seed seed;
        seed.origin = SeedOrigin::Carving;
        seed.volume = place.volume;
        seed.format = hit.format;
        seed.start = index;
        seed.owner = owner;
        seed.carve = std::move(carve);
        carveSeeds_.push_back(std::move(seed));
    }
}

Status RunState::replay(const std::vector<FragmentPassEvent>& events) {
    for (const FragmentPassEvent& event : events) {
        carving::SignatureHit hit;
        hit.fileOffset = event.fileOffset;
        hit.signatureIndex = event.signatureIndex;
        hit.format = event.formatId == probe_->descriptor().id ? probe_.get() : formats_.find(event.formatId);
        if (hit.format == nullptr || hit.signatureIndex >= hit.format->descriptor().signatures.size()) {
            return makeError(ErrorCode::InvalidInput, "a saved hit names an unknown format or signature");
        }
        std::optional<carving::CarveOutcome> outcome;
        if (event.carve.has_value()) {
            if (Status valid = carving::validateFileCandidate(*event.carve); !valid.ok()) {
                return valid;
            }
            outcome = carving::CarveOutcome(*event.carve);
        } else if (hit.format != probe_.get()) {
            return makeError(ErrorCode::InvalidInput, "a saved carving hit has no carve");
        }
        if (Status applied = commitHit(hit, outcome.has_value() ? &*outcome : nullptr, true); !applied.ok()) {
            return applied;
        }
    }
    return success();
}

Result<bool> RunState::sameContent(Search& search, Hypothesis& a, Hypothesis& b) {
    const std::uint64_t lengthA = a.deliveredLength();
    const std::uint64_t lengthB = b.deliveredLength();
    if (lengthA != lengthB || a.assessment.length != b.assessment.length) {
        return false;
    }
    Result<std::unique_ptr<CandidateContentReader>> first = search.open(a.layout, a.assessment.length, lengthA);
    if (!first.ok()) {
        return first.error();
    }
    Result<std::unique_ptr<CandidateContentReader>> second = search.open(b.layout, b.assessment.length, lengthB);
    if (!second.ok()) {
        return second.error();
    }
    if ((*first)->size() != (*second)->size()) {
        return false;
    }
    std::vector<std::byte> copy;
    for (std::uint64_t offset = 0; offset < (*first)->size(); offset += kCompareChunk) {
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(kCompareChunk, (*first)->size() - offset));
        Result<std::span<const std::byte>> left = (*first)->read(offset, length);
        if (!left.ok()) {
            return left.error();
        }
        copy.assign(left->begin(), left->end());
        Result<std::span<const std::byte>> right = (*second)->read(offset, length);
        if (!right.ok()) {
            return right.error();
        }
        if (!std::equal(copy.begin(), copy.end(), right->begin(), right->end())) {
            search.addBytesRead((*first)->bytesRead() + (*second)->bytesRead());
            return false;
        }
    }
    search.addBytesRead((*first)->bytesRead() + (*second)->bytesRead());
    return true;
}

Result<bool> RunState::dataChecked(Search& search, const Hypothesis& hypothesis, std::uint64_t length) {
    const std::uint64_t cs = search.clusterSize();
    const std::uint64_t clusters = length / cs + (length % cs != 0 ? 1 : 0);
    if (clusters <= 2) {
        return true;
    }
    // The joints of the layout: the first cluster of every fragment after the
    // first; in one piece, the middle cluster.
    std::vector<std::uint64_t> joints;
    std::uint64_t position = 0;
    std::optional<std::uint64_t> previousEnd;
    for (const Run& run : fragments::prefixOf(hypothesis.layout, clusters)) {
        if (!run.placed()) {
            return false;
        }
        if (previousEnd.has_value() && *previousEnd != run.first) {
            joints.push_back(position);
        }
        previousEnd = run.first + run.count;
        position += run.count;
    }
    if (joints.empty()) {
        joints.push_back(clusters / 2);
    }
    Result<std::unique_ptr<CandidateContentReader>> opened = search.open(hypothesis.layout, length, length);
    if (!opened.ok()) {
        return opened.error();
    }
    const carving::IFileFormat& format = *search.seed().format;
    for (const std::uint64_t joint : joints) {
        // The layout with other bytes in that cluster, every offset unchanged.
        const std::uint64_t begin = joint * cs;
        SubstitutedReader control(**opened, begin, std::min(length, begin + cs));
        Result<carving::ValidationResult> verdict = format.validator().validate(control);
        if (!verdict.ok()) {
            search.addBytesRead((*opened)->bytesRead());
            if (verdict.error().code == ErrorCode::Cancelled) {
                return verdict.error();
            }
            if ((*opened)->sourceFailure().has_value()) {
                return *(*opened)->sourceFailure();
            }
            continue;  // the format failed on the control: it rejected it
        }
        if (verdict->status == ValidationStatus::Valid) {
            search.addBytesRead((*opened)->bytesRead());
            return false;
        }
    }
    search.addBytesRead((*opened)->bytesRead());
    return true;
}

Result<ReconstructionHypothesis> RunState::deliver(Search& search, Hypothesis& hypothesis,
                                                   const FragmentCandidate& candidate, bool full) {
    const Seed& seed = search.seed();
    VolumeEvidence& volume = search.volume();
    const Geometry& geometry = volume.geometry();
    const std::uint64_t length = hypothesis.assessment.length;
    const std::uint64_t delivered = full ? length : std::min(length, hypothesis.deliveredLength());
    const std::uint64_t cs = geometry.clusterSize;

    ReconstructionHypothesis result;
    result.source = hypothesis.source;
    RecoveryCandidate& data = result.data;
    if (seed.metadata != nullptr) {
        data = *seed.metadata;
    } else {
        data.method = RecoveryMethod::Carving;
        data.extension = seed.format->descriptor().extension;
    }
    data.id = candidate.id;
    data.expectedSize = length;
    data.embeddedData.clear();
    data.sourceRegions = fragments::regionsOf(volume, hypothesis.layout, length, delivered, true);
    const Layout part = fragments::prefixOf(hypothesis.layout, delivered / cs + (delivered % cs != 0 ? 1 : 0));
    const std::size_t pieces = fragments::fragmentsOf(part);
    data.fragmentation = FragmentationInfo{pieces, false};
    if (pieces > 1) {
        data.method = RecoveryMethod::Fragmented;
    } else {
        data.method = seed.metadata != nullptr ? RecoveryMethod::Hybrid : RecoveryMethod::Carving;
    }
    std::erase_if(data.warnings, [](CandidateWarning warning) {
        return warning == CandidateWarning::DataMissing || warning == CandidateWarning::ClustersReallocated ||
               warning == CandidateWarning::DataNotDecoded;
    });
    if (data.bytes(RegionKind::Missing) > 0) {
        data.warnings.push_back(CandidateWarning::DataMissing);
    }
    if (data.reallocatedBytes() > 0) {
        data.warnings.push_back(CandidateWarning::ClustersReallocated);
    }
    for (const Run& run : part) {
        if (run.placed()) {
            result.clusters.push_back(ClusterRun{geometry.clusterNumber(run.first), run.count});
        }
    }

    // The format's validator on exactly the bytes the reconstruction delivers.
    Result<std::unique_ptr<CandidateContentReader>> reader =
        CandidateContentReader::open(source_, data, reads(), options_.carving.readCacheSize);
    if (!reader.ok()) {
        return reader.error();
    }
    const carving::IFileFormat& format = *seed.format;
    Result<carving::ValidationResult> verdict = format.validator().validate(**reader);
    if (!verdict.ok()) {
        if (verdict.error().code == ErrorCode::Cancelled) {
            return verdict.error();
        }
        if ((*reader)->sourceFailure().has_value()) {
            return *(*reader)->sourceFailure();
        }
        verdict =
            carving::ValidationResult{ValidationStatus::Invalid, 0, "the format failed: " + verdict.error().message};
    }
    result.validation = std::move(*verdict);

    HypothesisEvidence evidence;
    if (full) {
        // What was tried, whole.
        Hypothesis whole = hypothesis;
        whole.evidence.reset();
        whole.wholeFile = true;
        evidence = search.evidenceOf(whole);
    } else {
        evidence = search.evidenceOf(hypothesis);
    }
    evidence.unreadableBytes = unreadableBytesOf(data, **reader);
    if (fragments::isIsoFormat(format)) {
        Result<Mp4Structure> structure = mp4StructureOf(**reader, data, options_.mp4);
        if (!structure.ok()) {
            return structure.error();
        }
        if (seed.metadata == nullptr && dynamic_cast<const formats::M4aFormat*>(&format) == nullptr) {
            data.extension = std::string(mp4Extension(structure->majorBrand));
        }
        result.mp4 = std::move(*structure);
    }
    search.addBytesRead((*reader)->bytesRead());
    if (seed.metadata == nullptr) {
        data.filename = carvedName(candidate.id, data.extension);
    }
    if (result.validation.status == ValidationStatus::Valid) {
        Result<bool> checked = dataChecked(search, hypothesis, length);
        if (!checked.ok()) {
            return checked.error();
        }
        evidence.dataChecked = *checked;
    }
    result.evidence = evidence;
    return result;
}

Result<FragmentCandidate> RunState::reconstruct(Seed& seed) {
    VolumeEvidence& volume = *volumes_[seed.volume];
    FragmentCandidate candidate;
    candidate.id = CandidateId{nextId_++};
    candidate.origin = seed.origin;
    candidate.formatId = seed.format->descriptor().id;
    if (seed.metadata != nullptr) {
        candidate.filesystemCandidate = seed.metadata->id;
        candidate.recordedSize = seed.metadata->expectedSize;
        candidate.name = seed.metadata->filename;
        candidate.path = seed.metadata->filesystemEvidence.path;
    } else {
        candidate.name = carvedName(candidate.id, seed.format->descriptor().extension);
    }
    candidate.carve = seed.carve;
    candidate.filesystem = volume.recovery().filesystem().info().type;
    candidate.volumeOffset = volume.geometry().volumeOffset;
    candidate.clusterSize = static_cast<std::uint32_t>(volume.geometry().clusterSize);

    Search search(source_, options_, volume, seed, moovAnchors_[seed.volume]);
    if (Status searched = search.run(); !searched.ok()) {
        return searched.error();
    }
    std::vector<Hypothesis>& pool = search.pool();
    const std::size_t maxDelivered = options_.limits.maxAlternatives;

    if (seed.unrecoverable.has_value() || pool.empty()) {
        candidate.status = ReconstructionStatus::Unrecoverable;
        candidate.reason = seed.unrecoverable.value_or("no layout of the file could be validated");
        candidate.search = search.stats();
        return candidate;
    }
    fragments::rank(search, pool);

    // The layouts as supported as the best, whose bytes differ from each other.
    std::vector<std::size_t> tied{0};
    for (std::size_t i = 1; i < pool.size() && fragments::equallySupported(search, pool.front(), pool[i]); ++i) {
        bool duplicate = false;
        for (const std::size_t kept : tied) {
            Result<bool> same = sameContent(search, pool[kept], pool[i]);
            if (!same.ok()) {
                return same.error();
            }
            if (*same) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            tied.push_back(i);
        }
    }

    Hypothesis& best = pool.front();
    const Assessment& verdict = best.assessment;
    bool full = false;
    if (tied.size() >= 2) {
        candidate.status = ReconstructionStatus::Ambiguous;
        candidate.tied = std::min(tied.size(), maxDelivered);
        candidate.reason = plural(tied.size(), "layout", "layouts") +
                           " with different bytes are equally supported by the evidence: " + verdict.detail;
    } else if (best.deliveredLength() == 0 ||
               (!best.wholeFile && verdict.status != ValidationStatus::Valid && verdict.progress == 0)) {
        candidate.status = ReconstructionStatus::Unrecoverable;
        candidate.reason = "nothing of the file validates: " + verdict.detail;
        full = true;
    } else {
        const HypothesisEvidence& evidence = search.evidenceOf(best);
        if (!best.wholeFile && verdict.status != ValidationStatus::Valid) {
            candidate.status = ReconstructionStatus::Partial;
            candidate.reason = "the file is confirmed up to byte " + std::to_string(best.deliveredLength()) +
                               " (its structure breaks at " + std::to_string(verdict.progress) +
                               "); no layout continues it: " + verdict.detail;
        } else if (evidence.missingBytes > 0) {
            candidate.status = ReconstructionStatus::Partial;
            candidate.reason = std::to_string(evidence.missingBytes) +
                               " bytes of the file could not be placed; " + verdict.detail;
        } else if (verdict.status != ValidationStatus::Valid || evidence.allocatedClusters > 0) {
            candidate.status = ReconstructionStatus::Corrupted;
            candidate.reason = "the whole file is placed, but " +
                               (evidence.allocatedClusters > 0
                                    ? plural(evidence.allocatedClusters, "cluster is", "clusters are") +
                                          " allocated to other data now"
                                    : std::string("its content is damaged")) +
                               ": " + verdict.detail;
        } else {
            candidate.status = ReconstructionStatus::Complete;
            candidate.reason = "the layout (" + plural(evidence.fragments, "fragment", "fragments") +
                               ") validates: " + verdict.detail;
        }
    }

    // Delivery order: the tied first, then the others by rank, each with a
    // part of the file that no earlier one delivers as it does.
    const std::uint64_t cs = volume.geometry().clusterSize;
    const auto deliveredPart = [&](const Hypothesis& hypothesis) {
        const std::uint64_t length = full ? hypothesis.assessment.length : hypothesis.deliveredLength();
        return std::make_pair(length, fragments::prefixOf(hypothesis.layout, length / cs + (length % cs != 0 ? 1 : 0)));
    };
    std::vector<std::size_t> order = tied;
    std::vector<std::pair<std::uint64_t, Layout>> parts;
    for (const std::size_t index : tied) {
        parts.push_back(deliveredPart(pool[index]));
    }
    for (std::size_t i = 0; i < pool.size() && order.size() < maxDelivered; ++i) {
        if (std::find(tied.begin(), tied.end(), i) != tied.end()) {
            continue;
        }
        std::pair<std::uint64_t, Layout> part = deliveredPart(pool[i]);
        if (std::find(parts.begin(), parts.end(), part) == parts.end()) {
            parts.push_back(std::move(part));
            order.push_back(i);
        }
    }
    if (order.size() > maxDelivered) {
        order.resize(maxDelivered);
    }
    for (const std::size_t index : order) {
        Result<ReconstructionHypothesis> delivered = deliver(search, pool[index], candidate, full);
        if (!delivered.ok()) {
            return delivered.error();
        }
        candidate.hypotheses.push_back(std::move(*delivered));
    }
    if (seed.metadata == nullptr && !candidate.hypotheses.empty()) {
        // A carve's extension can depend on its content (an MP4's brand).
        candidate.name = candidate.hypotheses.front().data.filename;
    }
    if (!candidate.hypotheses.empty() && candidate.status != ReconstructionStatus::Ambiguous &&
        candidate.status != ReconstructionStatus::Unrecoverable) {
        const HypothesisEvidence& chosen = candidate.hypotheses.front().evidence;
        if (candidate.status == ReconstructionStatus::Complete && chosen.unreadableBytes > 0) {
            candidate.status = ReconstructionStatus::Corrupted;
            candidate.reason = "the whole file is placed, but " + std::to_string(chosen.unreadableBytes) +
                               " of its bytes could not be read: " + verdict.detail;
        }
        // Its clusters are this file's from now on.
        const std::uint64_t delivered = best.deliveredLength();
        const std::uint64_t clusters = delivered / cs + (delivered % cs != 0 ? 1 : 0);
        volume.claimLayout(volume.strong(), fragments::prefixOf(best.layout, clusters), seed.owner);
    }
    candidate.search = search.stats();
    return candidate;
}

void RunState::count(const FragmentCandidate& candidate) {
    switch (candidate.status) {
    case ReconstructionStatus::Complete:
        ++report_.complete;
        break;
    case ReconstructionStatus::Partial:
        ++report_.partial;
        break;
    case ReconstructionStatus::Corrupted:
        ++report_.corrupted;
        break;
    case ReconstructionStatus::Ambiguous:
        ++report_.ambiguous;
        break;
    case ReconstructionStatus::Unrecoverable:
        ++report_.unrecoverable;
        break;
    }
    if (!candidate.search.complete) {
        ++report_.searchesLimited;
    }
    report_.layoutsValidated += candidate.search.layoutsValidated;
}

Result<std::optional<FragmentCandidate>> RunState::reconstructNext() {
    if (Status cancelled = checkCancelled(); !cancelled.ok()) {
        return cancelled.error();
    }
    if (!begun_ || cursor_ >= seedCount()) {
        return makeError(ErrorCode::InvalidInput, "every seed is reconstructed already");
    }
    const bool fromCarve = cursor_ >= filesystemSeeds_.size();
    Seed& seed = seedAt(cursor_);
    ++cursor_;
    if (fromCarve) {
        // A reconstruction settled since holds its first cluster: it is part of that file.
        if (volumes_[seed.volume]->strong().claimedByOthers(seed.start, seed.owner)) {
            return std::optional<FragmentCandidate>{};
        }
        ++report_.carvingSeeds;
    }
    Result<FragmentCandidate> candidate = reconstruct(seed);
    if (!candidate.ok()) {
        return candidate.error();
    }
    count(*candidate);
    const ReconstructionHypothesis* chosen = candidate->reconstruction();
    log(LogLevel::Debug, "fragment candidate",
        {field("id", candidate->id.value()), field("origin", toString(candidate->origin)),
         field("format", candidate->formatId), field("status", toString(candidate->status)),
         field("hypotheses", candidate->hypotheses.size()),
         field("fragments", chosen != nullptr ? chosen->evidence.fragments : 0),
         field("layouts_validated", candidate->search.layoutsValidated),
         field("search_complete", candidate->search.complete ? "yes" : "no")});
    return std::optional<FragmentCandidate>(std::move(*candidate));
}

Status RunState::replayReconstruction(const FragmentCandidate& candidate) {
    if (!begun_) {
        return makeError(ErrorCode::InvalidInput, "fragment reconstruction has not begun");
    }
    // The seeds reconstructNext() would pass over first.
    for (;;) {
        if (cursor_ >= seedCount()) {
            return makeError(ErrorCode::InvalidInput, "a saved reconstruction has no seed left");
        }
        const Seed& next = seedAt(cursor_);
        if (cursor_ >= filesystemSeeds_.size() &&
            volumes_[next.volume]->strong().claimedByOthers(next.start, next.owner)) {
            ++cursor_;
            continue;
        }
        break;
    }
    const bool fromCarve = cursor_ >= filesystemSeeds_.size();
    const Seed& seed = seedAt(cursor_);
    bool fits = candidate.origin == seed.origin && candidate.id.value() == nextId_ &&
                candidate.formatId == seed.format->descriptor().id;
    if (seed.metadata != nullptr) {
        fits = fits && candidate.filesystemCandidate == seed.metadata->id;
    } else {
        fits = fits && candidate.carve.has_value() && seed.carve.has_value() &&
               candidate.carve->sourceOffset == seed.carve->sourceOffset;
    }
    if (!fits) {
        return makeError(ErrorCode::InvalidInput, "a saved reconstruction is not the next seed's");
    }
    // Its clusters are this file's from now on, as reconstruct() settled them.
    VolumeEvidence& volume = *volumes_[seed.volume];
    const Geometry& geometry = volume.geometry();
    Layout settled;
    if (!candidate.hypotheses.empty() && candidate.status != ReconstructionStatus::Ambiguous &&
        candidate.status != ReconstructionStatus::Unrecoverable) {
        for (const ClusterRun& run : candidate.hypotheses.front().clusters) {
            const bool inside = run.count > 0 && run.firstCluster >= geometry.firstCluster &&
                                run.firstCluster - geometry.firstCluster < geometry.clusterCount &&
                                run.count <= geometry.clusterCount - (run.firstCluster - geometry.firstCluster);
            if (!inside) {
                return makeError(ErrorCode::InvalidInput, "a saved reconstruction places clusters outside its volume");
            }
            settled.push_back(Run{run.firstCluster - geometry.firstCluster, run.count});
        }
    }
    ++cursor_;
    ++nextId_;
    if (fromCarve) {
        ++report_.carvingSeeds;
    }
    volume.claimLayout(volume.strong(), settled, seed.owner);
    count(candidate);
    return success();
}

}  // namespace

// ---------------------------------------------------------------------------
// FragmentRecoverySteps
// ---------------------------------------------------------------------------

struct FragmentRecoverySteps::Impl {
    Impl(storage::IStorageSource& source, const carving::FormatRegistry& formats, FragmentRecoveryOptions options)
        : options(std::move(options)), state(source, formats, this->options) {}

    const FragmentRecoveryOptions options;
    RunState state;
};

Result<std::unique_ptr<FragmentRecoverySteps>> FragmentRecoverySteps::create(storage::IStorageSource& source,
                                                                             const carving::FormatRegistry& formats,
                                                                             FragmentRecoveryOptions options) {
    if (!source.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "fragment reconstruction source is not open");
    }
    if (formats.empty()) {
        return makeError(ErrorCode::InvalidInput, "fragment reconstruction needs at least one format");
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
    if (Status valid = formats::mp4::validate(options.mp4.limits); !valid.ok()) {
        return valid.error();
    }
    if (Status valid = validate(options.limits); !valid.ok()) {
        return valid.error();
    }
    auto impl = std::make_unique<Impl>(source, formats, std::move(options));
    return std::unique_ptr<FragmentRecoverySteps>(new FragmentRecoverySteps(std::move(impl)));
}

FragmentRecoverySteps::FragmentRecoverySteps(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FragmentRecoverySteps::~FragmentRecoverySteps() = default;

Status FragmentRecoverySteps::addVolume(FilesystemRecovery& volume, const CandidateScan& scan) {
    if (scan.volumeOffset != volume.volumeOffset()) {
        return makeError(ErrorCode::InvalidInput, "the candidate scan of the volume at " +
                                                      std::to_string(scan.volumeOffset) +
                                                      " is not from the volume at " +
                                                      std::to_string(volume.volumeOffset()));
    }
    return impl_->state.addVolume(volume, scan);
}

std::size_t FragmentRecoverySteps::volumeCount() const noexcept {
    return impl_->state.volumeCount();
}

Status FragmentRecoverySteps::begin() {
    return impl_->state.begin();
}

Result<FragmentSeedExamination> FragmentRecoverySteps::examineSeed(std::size_t volume, std::size_t index) const {
    return impl_->state.examineSeed(volume, index);
}

Status FragmentRecoverySteps::addSeedExamination(FragmentSeedExamination examination) {
    return impl_->state.addSeedExamination(std::move(examination));
}

std::vector<std::shared_ptr<const carving::IFileFormat>> FragmentRecoverySteps::extraFormats() const {
    return impl_->state.extraFormats();
}

bool FragmentRecoverySteps::wants(const carving::SignatureHit& hit) {
    return impl_->state.wants(hit);
}

Status FragmentRecoverySteps::commit(const carving::SignatureHit& hit, const carving::CarveOutcome* outcome) {
    return impl_->state.commit(hit, outcome);
}

void FragmentRecoverySteps::recordEvents(bool on) noexcept {
    impl_->state.recordEvents(on);
}

std::vector<FragmentPassEvent> FragmentRecoverySteps::takeEvents() {
    return impl_->state.takeEvents();
}

Status FragmentRecoverySteps::replay(const std::vector<FragmentPassEvent>& events) {
    return impl_->state.replay(events);
}

std::size_t FragmentRecoverySteps::seedCount() const noexcept {
    return impl_->state.seedCount();
}

std::size_t FragmentRecoverySteps::nextSeed() const noexcept {
    return impl_->state.nextSeed();
}

Result<std::optional<FragmentCandidate>> FragmentRecoverySteps::reconstructNext() {
    return impl_->state.reconstructNext();
}

Status FragmentRecoverySteps::replayReconstruction(const FragmentCandidate& candidate) {
    return impl_->state.replayReconstruction(candidate);
}

FragmentRecoveryReport FragmentRecoverySteps::report() const {
    return impl_->state.report();
}

const FragmentRecoveryOptions& FragmentRecoverySteps::options() const noexcept {
    return impl_->options;
}

// ---------------------------------------------------------------------------
// FragmentRecovery: the steps, one after the other
// ---------------------------------------------------------------------------

FragmentRecovery::FragmentRecovery(storage::IStorageSource& source, const carving::FormatRegistry& formats,
                                   FragmentRecoveryOptions options)
    : source_(&source), formats_(&formats), options_(std::move(options)) {}

Status FragmentRecovery::addVolume(FilesystemRecovery& volume, const CandidateScan& scan) {
    if (scan.volumeOffset != volume.volumeOffset()) {
        return makeError(ErrorCode::InvalidInput, "the candidate scan of the volume at " +
                                                      std::to_string(scan.volumeOffset) +
                                                      " is not from the volume at " +
                                                      std::to_string(volume.volumeOffset()));
    }
    if (!Geometry::of(volume).has_value()) {
        return makeError(ErrorCode::InvalidInput, "the volume at " + std::to_string(volume.volumeOffset()) +
                                                      " has no usable cluster area");
    }
    volumes_.push_back(Volume{&volume, &scan});
    return success();
}

Result<FragmentRecoveryReport> FragmentRecovery::run(const FragmentCandidateSink& sink) {
    if (!sink) {
        return makeError(ErrorCode::InvalidInput, "fragment reconstruction needs a candidate sink");
    }
    Result<std::unique_ptr<FragmentRecoverySteps>> created =
        FragmentRecoverySteps::create(*source_, *formats_, options_);
    if (!created.ok()) {
        return created.error();
    }
    FragmentRecoverySteps& steps = **created;
    for (const Volume& volume : volumes_) {
        if (Status added = steps.addVolume(*volume.recovery, *volume.scan); !added.ok()) {
            return added.error();
        }
    }
    const auto started = std::chrono::steady_clock::now();
    diagnostics::Logger* logger = options_.carving.scan.logger;
    if (logger != nullptr) {
        logger->log(LogLevel::Info, kComponent, "fragment reconstruction started",
                    {field("volumes", volumes_.size()), field("formats", formats_->size()),
                     field("filesystem", options_.useFilesystem ? "yes" : "no"),
                     field("carve", options_.carve ? "yes" : "no")});
    }
    if (Status begun = steps.begin(); !begun.ok()) {
        return begun.error();
    }
    if (options_.useFilesystem) {
        for (std::size_t v = 0; v < volumes_.size(); ++v) {
            for (std::size_t i = 0; i < volumes_[v].scan->candidates.size(); ++i) {
                Result<FragmentSeedExamination> examined = steps.examineSeed(v, i);
                if (!examined.ok()) {
                    return examined.error();
                }
                if (Status added = steps.addSeedExamination(std::move(*examined)); !added.ok()) {
                    return added.error();
                }
            }
        }
    }

    // The pass: the registry's formats (when carving) and the moov probe.
    carving::ScanReport scanReport;
    carving::FormatRegistry registry;
    if (options_.carve) {
        for (const std::shared_ptr<const carving::IFileFormat>& format : formats_->formats()) {
            if (Status added = registry.add(format); !added.ok()) {
                return added.error();
            }
        }
    }
    const std::vector<std::shared_ptr<const carving::IFileFormat>> extras = steps.extraFormats();
    for (const std::shared_ptr<const carving::IFileFormat>& format : extras) {
        if (Status added = registry.add(format); !added.ok()) {
            return added.error();
        }
    }
    if (!registry.empty() && steps.volumeCount() > 0) {
        Result<carving::SignatureScanner> scanner = carving::SignatureScanner::create(registry);
        if (!scanner.ok()) {
            return scanner.error();
        }
        const carving::HitSink onHit = [&](const carving::SignatureHit& hit) -> Status {
            return steps.commit(hit, nullptr);
        };
        Result<carving::ScanReport> scanned = scanner->scan(*source_, onHit, options_.carving.scan);
        if (!scanned.ok()) {
            return scanned.error();
        }
        scanReport = std::move(*scanned);
        if (scanReport.outcome == carving::ScanOutcome::Cancelled) {
            return makeError(ErrorCode::Cancelled, "fragment reconstruction cancelled");
        }
        // The probe is not one of the caller's formats.
        if (!extras.empty() && !scanReport.hitsPerFormat.empty()) {
            scanReport.hits -= scanReport.hitsPerFormat.back();
            scanReport.hitsPerFormat.pop_back();
        }
    }

    while (steps.nextSeed() < steps.seedCount()) {
        Result<std::optional<FragmentCandidate>> candidate = steps.reconstructNext();
        if (!candidate.ok()) {
            return candidate.error();
        }
        if (candidate->has_value()) {
            if (Status sent = sink(std::move(**candidate)); !sent.ok()) {
                return sent.error();
            }
        }
    }
    FragmentRecoveryReport report = steps.report();
    report.scan = std::move(scanReport);
    report.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (logger != nullptr) {
        logger->log(LogLevel::Info, kComponent, "fragment reconstruction ended",
                    {field("filesystem_seeds", report.filesystemSeeds), field("carving_seeds", report.carvingSeeds),
                     field("complete", report.complete), field("partial", report.partial),
                     field("corrupted", report.corrupted), field("ambiguous", report.ambiguous),
                     field("unrecoverable", report.unrecoverable), field("limited", report.searchesLimited),
                     field("layouts_validated", report.layoutsValidated),
                     field("elapsed_ms", report.elapsed.count())});
    }
    return report;
}

}  // namespace recovery
