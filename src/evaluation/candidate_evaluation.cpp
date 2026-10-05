#include "evaluation/candidate_evaluation.hpp"

#include "recovery/candidate_content.hpp"
#include "recovery/checked_math.hpp"
#include "recovery/ordered_work.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace recovery::evaluation {

namespace {

using carving::FileCandidate;
using carving::ValidationStatus;

constexpr std::size_t kMaxActiveFiles = 8;

// A filesystem candidate, by its volume and its id in that volume's scan.
struct FsKey {
    std::uint64_t volumeOffset = 0;
    std::uint64_t id = 0;
    friend auto operator<=>(const FsKey&, const FsKey&) = default;
};

struct SlotRef {
    std::size_t volume = 0;
    std::size_t index = 0;
    friend auto operator<=>(const SlotRef&, const SlotRef&) = default;
};

// A file to evaluate: its layout and the evidence it was found with.
struct Proposal {
    RecoveryCandidate data;
    std::string formatId;
    std::optional<CandidateId> filesystemCandidate;
    std::optional<CandidateId> mp4Candidate;
    std::optional<FileCandidate> carve;
    std::vector<FileCandidate> otherCarves;
    std::optional<Mp4Structure> mp4;
    std::vector<Mp4Warning> mp4Warnings;
    std::optional<FragmentEvidence> fragments;
    std::optional<AllocationEvidence> allocation;
    // The format's verdict on exactly these bytes, when the evidence has it.
    std::optional<carving::ValidationResult> knownStructure;
    // A filesystem candidate's own layout (no other stage replaced it).
    bool filesystemLayout = false;
    // A filesystem candidate with a carve that starts where it starts: the
    // layout is decided when it is evaluated (P12's rule).
    bool mergeCarve = false;
    // A carve of its own: its place in the volume's allocation is examined.
    bool examineAllocation = false;
};

// A filesystem candidate, and what replaces it or joins it.
struct Slot {
    std::optional<std::uint64_t> start;
    // Proposals that replace it (fragment reconstructions, an MP4 candidate).
    std::vector<Proposal> replacements;
    std::optional<FileCandidate> carve;
    std::vector<FileCandidate> otherCarves;
    // Fragment reconstruction found nothing for it.
    std::optional<FragmentEvidence> failedReconstruction;
};

struct ActiveData {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::size_t candidate = 0;
};

struct AllocationScan {
    AllocationEvidence evidence;
    // Source ranges in clusters allocated now, merged.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> allocated;
    // The active file the range lies inside, if any.
    std::optional<std::size_t> container;
};

int rank(ValidationStatus status) noexcept {
    switch (status) {
    case ValidationStatus::Valid:
        return 0;
    case ValidationStatus::Truncated:
        return 1;
    case ValidationStatus::NotValidated:
        return 2;
    case ValidationStatus::Invalid:
        return 3;
    }
    return 4;
}

bool sameCarve(const FileCandidate& a, const FileCandidate& b) noexcept {
    return a.formatId == b.formatId && a.sourceOffset == b.sourceOffset && a.length == b.length &&
           a.extents == b.extents;
}

// A deleted entry whose first cluster is allocated to other data now: what
// starts there is the new owner's file (P12's rule).
bool startReallocated(const RecoveryCandidate& data) noexcept {
    return data.isDeleted() && !data.sourceRegions.empty() && data.sourceRegions.front().kind == RegionKind::Stored &&
           data.sourceRegions.front().reallocated;
}

std::optional<std::uint64_t> clusterOffset(FilesystemRecovery& volume, std::uint64_t cluster) {
    const filesystem::FilesystemInfo& info = volume.filesystem().info();
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

// Where a filesystem candidate starts on the source: its first stored byte,
// or where its metadata puts its first cluster.
std::optional<std::uint64_t> startOf(const RecoveryCandidate& candidate, FilesystemRecovery& volume) {
    if (!candidate.sourceRegions.empty() && candidate.sourceRegions.front().kind == RegionKind::Stored) {
        return candidate.sourceRegions.front().sourceOffset;
    }
    const std::uint64_t cluster = candidate.filesystemEvidence.allocation.firstCluster.value();
    if (cluster == 0) {
        return std::nullopt;
    }
    return clusterOffset(volume, cluster);
}

// A carve's layout as a recovery candidate: stored runs from its extents.
RecoveryCandidate fromCarve(const FileCandidate& carve) {
    RecoveryCandidate data;
    data.id = CandidateId{carve.id.value()};
    data.method = RecoveryMethod::Carving;
    data.extension = carve.extension;
    data.expectedSize = carve.length;
    std::size_t fragments = 0;
    for (const carving::CarvedExtent& extent : carve.extents) {
        if (!data.sourceRegions.empty()) {
            SourceRegion& last = data.sourceRegions.back();
            if (last.sourceOffset + last.length == extent.sourceOffset) {
                last.length += extent.length;
                continue;
            }
        }
        data.sourceRegions.push_back(
            SourceRegion{extent.fileOffset, extent.length, RegionKind::Stored, extent.sourceOffset, false});
        ++fragments;
    }
    // The carver assumes how the file continues: its layout is not recorded anywhere.
    data.fragmentation = FragmentationInfo{fragments, false};
    return data;
}

// The stored regions of `regions` split where they enter or leave the
// allocated source ranges, those inside marked reallocated.
std::vector<SourceRegion> markAllocated(const std::vector<SourceRegion>& regions,
                                        const std::vector<std::pair<std::uint64_t, std::uint64_t>>& allocated) {
    std::vector<SourceRegion> out;
    const auto push = [&out](SourceRegion region) {
        if (region.length == 0) {
            return;
        }
        if (!out.empty()) {
            SourceRegion& last = out.back();
            if (last.kind == RegionKind::Stored && region.kind == RegionKind::Stored &&
                last.reallocated == region.reallocated && last.sourceOffset + last.length == region.sourceOffset) {
                last.length += region.length;
                return;
            }
        }
        out.push_back(region);
    };
    for (const SourceRegion& region : regions) {
        if (region.kind != RegionKind::Stored) {
            push(region);
            continue;
        }
        std::uint64_t position = region.sourceOffset;
        const std::uint64_t end = region.sourceOffset + region.length;
        for (const auto& [begin, stop] : allocated) {
            if (stop <= position || begin >= end) {
                continue;
            }
            if (begin > position) {
                push(SourceRegion{region.fileOffset + (position - region.sourceOffset), begin - position,
                                  RegionKind::Stored, position, region.reallocated});
                position = begin;
            }
            const std::uint64_t until = std::min(stop, end);
            push(SourceRegion{region.fileOffset + (position - region.sourceOffset), until - position,
                              RegionKind::Stored, position, true});
            position = until;
        }
        push(SourceRegion{region.fileOffset + (position - region.sourceOffset), end - position, RegionKind::Stored,
                          position, region.reallocated});
    }
    return out;
}

std::string carvedName(std::uint64_t id, std::string_view extension) {
    std::string number = std::to_string(id);
    if (number.size() < 6) {
        number.insert(0, 6 - number.size(), '0');
    }
    return "recovered_" + number + (extension.empty() ? std::string() : "." + std::string(extension));
}

FragmentEvidence evidenceOf(const FragmentCandidate& candidate) {
    FragmentEvidence evidence;
    evidence.fragmentCandidate = candidate.id;
    evidence.status = candidate.status;
    evidence.reason = candidate.reason;
    evidence.origin = candidate.origin;
    evidence.hypotheses = candidate.hypotheses.size();
    evidence.tied = candidate.status == ReconstructionStatus::Ambiguous ? candidate.tied : 0;
    evidence.search = candidate.search;
    return evidence;
}

bool extensionMatches(std::string_view extension, const carving::FormatDescriptor& descriptor) {
    if (extension.empty()) {
        return false;
    }
    if (extension == descriptor.extension || extension == descriptor.id) {
        return true;
    }
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kAliases = {{
        {"jpeg", "jpeg"},
        {"jpe", "jpeg"},
        {"jfif", "jpeg"},
        {"mov", "mp4"},
        {"m4v", "mp4"},
        {"3gp", "mp4"},
        {"3g2", "mp4"},
        {"m4b", "m4a"},
        {"m4p", "m4a"},
    }};
    return std::any_of(kAliases.begin(), kAliases.end(), [&](const auto& alias) {
        return alias.first == extension && alias.second == descriptor.id;
    });
}

// Everything one run needs, and its steps.
class Run {
public:
    Run(storage::IStorageSource& source, const carving::FormatRegistry& formats,
        const validation::MediaValidatorRegistry& media, const EvaluationOptions& options,
        const std::vector<std::pair<FilesystemRecovery*, const CandidateScan*>>& volumes)
        : source_(source), formats_(formats), media_(media), options_(options), volumes_(volumes) {}

    Result<EvaluationReport> run(std::vector<FileCandidate> carves, std::vector<Mp4Candidate> mp4,
                                 std::vector<FragmentCandidate> fragments, const EvaluatedCandidateSink& sink);

private:
    // One candidate to deliver: what it is made of, and the filesystem
    // candidate it comes from.
    struct Item {
        Proposal proposal;
        std::optional<SlotRef> slot;
    };

    // Merges the inputs into the candidates to deliver, in delivery order.
    void plan(std::vector<FileCandidate>& carves, std::vector<Mp4Candidate>& mp4,
              std::vector<FragmentCandidate>& fragments);
    void buildSlots();
    void takeFragments(std::vector<FragmentCandidate>& fragments);
    void takeMp4(std::vector<Mp4Candidate>& mp4);
    void takeCarves(std::vector<FileCandidate>& carves);
    [[nodiscard]] Status cancelled() const;
    [[nodiscard]] std::optional<std::size_t> volumeAt(std::uint64_t sourceOffset) const;
    [[nodiscard]] const std::vector<ActiveData>& activeData(std::size_t volume);
    // Thread-safe: a volume's filesystem is used under its lock.
    Result<AllocationScan> allocationOf(std::size_t volume, std::uint64_t begin, std::uint64_t end);
    Result<const carving::IFileFormat*> detectFormat(carving::IContentReader& content, std::string_view extension);
    // Thread-safe once planned: validates and hashes one candidate.
    Result<EvaluatedCandidate> evaluate(Proposal proposal, std::optional<SlotRef> slot);
    // In delivery order: the id, duplicates, warnings and counts.
    Status deliver(EvaluatedCandidate&& candidate, const EvaluatedCandidateSink& sink);
    // Counts a candidate an earlier run delivered as deliver() counts it.
    Status replay(const EvaluationRecord& record);
    void countMethod(RecoveryMethod method);
    void countStatus(ValidationStatus status);

    storage::IStorageSource& source_;
    const carving::FormatRegistry& formats_;
    const validation::MediaValidatorRegistry& media_;
    const EvaluationOptions& options_;
    const std::vector<std::pair<FilesystemRecovery*, const CandidateScan*>>& volumes_;

    std::vector<std::vector<Slot>> slots_;
    std::map<FsKey, SlotRef> byId_;
    std::multimap<std::uint64_t, SlotRef> byStart_;
    // Starts taken by a replacement (an MP4 candidate or a reconstruction) -> where it is.
    std::map<std::uint64_t, std::pair<std::optional<SlotRef>, std::size_t>> claimed_;
    // Proposals that belong to no added volume's filesystem candidate.
    std::vector<Proposal> loose_;
    // Carve seeds that fragment reconstruction could not reconstruct: (start, format) -> evidence.
    std::map<std::pair<std::uint64_t, std::string>, FragmentEvidence> failedCarves_;
    // Carves that an MP4 candidate or a reconstruction already holds: (start, format).
    std::set<std::pair<std::uint64_t, std::string>> consumed_;
    std::map<std::size_t, std::vector<ActiveData>> active_;
    // The candidates to deliver, in delivery order.
    std::vector<Item> order_;
    // The evaluated id of each filesystem candidate's first candidate.
    std::map<SlotRef, EvaluatedCandidateId> slotFirstId_;
    // Per volume, held while its filesystem is used; and the lock of active_.
    std::vector<std::unique_ptr<std::mutex>> volumeLocks_;
    std::mutex activeLock_;
    DuplicateIndex duplicates_;
    EvaluationReport report_;
    std::uint64_t nextId_ = 0;
};

Status Run::cancelled() const {
    if (options_.reads.cancellation.isCancellationRequested()) {
        return makeError(ErrorCode::Cancelled, "candidate evaluation cancelled");
    }
    return success();
}

void Run::buildSlots() {
    slots_.resize(volumes_.size());
    for (std::size_t v = 0; v < volumes_.size(); ++v) {
        FilesystemRecovery& volume = *volumes_[v].first;
        const CandidateScan& scan = *volumes_[v].second;
        slots_[v].resize(scan.candidates.size());
        for (std::size_t i = 0; i < scan.candidates.size(); ++i) {
            const RecoveryCandidate& candidate = scan.candidates[i];
            const SlotRef ref{v, i};
            byId_.emplace(FsKey{scan.volumeOffset, candidate.id.value()}, ref);
            slots_[v][i].start = startOf(candidate, volume);
            if (slots_[v][i].start.has_value()) {
                byStart_.emplace(*slots_[v][i].start, ref);
            }
        }
    }
}

void Run::takeFragments(std::vector<FragmentCandidate>& fragments) {
    for (FragmentCandidate& candidate : fragments) {
        std::optional<SlotRef> seed;
        if (candidate.origin == SeedOrigin::Filesystem && candidate.filesystemCandidate.has_value()) {
            const auto found = byId_.find(FsKey{candidate.volumeOffset, candidate.filesystemCandidate->value()});
            if (found != byId_.end()) {
                seed = found->second;
            }
        }
        const FragmentEvidence base = evidenceOf(candidate);
        if (candidate.carve.has_value()) {
            consumed_.emplace(candidate.carve->sourceOffset, candidate.carve->formatId);
        }
        if (candidate.status == ReconstructionStatus::Unrecoverable || candidate.hypotheses.empty()) {
            // Nothing validates: the seed keeps its own layout, with this evidence.
            if (seed.has_value()) {
                slots_[seed->volume][seed->index].failedReconstruction = base;
            } else if (candidate.carve.has_value()) {
                consumed_.erase({candidate.carve->sourceOffset, candidate.carve->formatId});
                failedCarves_.emplace(std::pair{candidate.carve->sourceOffset, candidate.carve->formatId}, base);
            }
            continue;
        }
        const std::size_t tied = std::min(candidate.tied, candidate.hypotheses.size());
        const std::size_t layouts =
            candidate.status == ReconstructionStatus::Ambiguous ? std::max<std::size_t>(1, tied) : 1;
        for (std::size_t k = 0; k < layouts; ++k) {
            ReconstructionHypothesis& hypothesis = candidate.hypotheses[k];
            Proposal proposal;
            proposal.data = std::move(hypothesis.data);
            proposal.formatId = candidate.formatId;
            proposal.knownStructure = hypothesis.validation;
            proposal.mp4 = std::move(hypothesis.mp4);
            proposal.carve = candidate.carve;
            if (candidate.origin == SeedOrigin::Filesystem) {
                proposal.filesystemCandidate = candidate.filesystemCandidate;
            }
            FragmentEvidence evidence = base;
            evidence.source = hypothesis.source;
            evidence.clusters = std::move(hypothesis.clusters);
            evidence.evidence = hypothesis.evidence;
            evidence.alternative = candidate.status == ReconstructionStatus::Ambiguous ? k + 1 : 0;
            proposal.fragments = std::move(evidence);
            const std::optional<std::uint64_t> start = proposal.data.sourceOffset();
            if (seed.has_value()) {
                Slot& slot = slots_[seed->volume][seed->index];
                if (start.has_value()) {
                    claimed_.emplace(*start, std::pair{seed, slot.replacements.size()});
                }
                slot.replacements.push_back(std::move(proposal));
            } else {
                if (start.has_value()) {
                    claimed_.emplace(*start, std::pair{std::optional<SlotRef>{}, loose_.size()});
                }
                loose_.push_back(std::move(proposal));
            }
        }
        if (seed.has_value()) {
            ++report_.superseded;
        }
    }
}

void Run::takeMp4(std::vector<Mp4Candidate>& mp4) {
    for (Mp4Candidate& candidate : mp4) {
        std::optional<SlotRef> seed;
        if (candidate.filesystemCandidate.has_value()) {
            const FsKey key{candidate.data.filesystemEvidence.volumeOffset, candidate.filesystemCandidate->value()};
            const auto found = byId_.find(key);
            if (found != byId_.end()) {
                seed = found->second;
            }
        }
        if (candidate.carving.has_value()) {
            consumed_.emplace(candidate.carving->sourceOffset, candidate.carving->formatId);
        }
        if (seed.has_value() && !slots_[seed->volume][seed->index].replacements.empty()) {
            // A reconstruction of the same file replaces it already.
            ++report_.superseded;
            continue;
        }
        Proposal proposal;
        proposal.formatId = candidate.structure.kind == formats::mp4::MediaKind::Audio ? "m4a" : "mp4";
        proposal.mp4Candidate = candidate.data.id;
        proposal.filesystemCandidate = candidate.filesystemCandidate;
        proposal.data = std::move(candidate.data);
        proposal.carve = std::move(candidate.carving);
        proposal.mp4 = std::move(candidate.structure);
        proposal.mp4Warnings = std::move(candidate.warnings);
        if (candidate.allocation.has_value()) {
            const Mp4Allocation& from = *candidate.allocation;
            AllocationEvidence allocation;
            allocation.filesystem = from.filesystem;
            allocation.volumeOffset = from.volumeOffset;
            allocation.clusters = from.clusters;
            allocation.freeClusters = from.freeClusters;
            allocation.allocatedClusters = from.allocatedClusters;
            allocation.otherClusters = from.otherClusters;
            allocation.complete = from.complete;
            allocation.activeFiles = from.activeFiles;
            allocation.insideActiveFile = from.insideActiveFile;
            proposal.allocation = std::move(allocation);
        }
        const std::optional<std::uint64_t> start = proposal.data.sourceOffset();
        if (seed.has_value()) {
            Slot& slot = slots_[seed->volume][seed->index];
            if (slot.failedReconstruction.has_value()) {
                proposal.fragments = slot.failedReconstruction;
            }
            if (start.has_value()) {
                claimed_.emplace(*start, std::pair{seed, slot.replacements.size()});
            }
            slot.replacements.push_back(std::move(proposal));
            ++report_.superseded;
        } else {
            if (start.has_value()) {
                claimed_.emplace(*start, std::pair{std::optional<SlotRef>{}, loose_.size()});
            }
            loose_.push_back(std::move(proposal));
        }
    }
}

void Run::takeCarves(std::vector<FileCandidate>& carves) {
    // The same carve added twice counts once.
    std::vector<FileCandidate> unique;
    for (FileCandidate& carve : carves) {
        if (consumed_.contains({carve.sourceOffset, carve.formatId})) {
            continue;
        }
        if (std::none_of(unique.begin(), unique.end(),
                         [&](const FileCandidate& seen) { return sameCarve(seen, carve); })) {
            unique.push_back(std::move(carve));
        }
    }
    // By start; the best first: Valid, Truncated, not validated, Invalid; then the longest.
    std::stable_sort(unique.begin(), unique.end(), [](const FileCandidate& a, const FileCandidate& b) {
        if (a.sourceOffset != b.sourceOffset) {
            return a.sourceOffset < b.sourceOffset;
        }
        if (rank(a.validation.status) != rank(b.validation.status)) {
            return rank(a.validation.status) < rank(b.validation.status);
        }
        return a.length > b.length;
    });
    for (std::size_t first = 0; first < unique.size();) {
        std::size_t last = first + 1;
        while (last < unique.size() && unique[last].sourceOffset == unique[first].sourceOffset) {
            ++last;
        }
        const std::uint64_t start = unique[first].sourceOffset;
        std::vector<FileCandidate> group(std::make_move_iterator(unique.begin() + static_cast<std::ptrdiff_t>(first)),
                                         std::make_move_iterator(unique.begin() + static_cast<std::ptrdiff_t>(last)));
        first = last;

        // A replacement holds this start: the carves are its evidence.
        if (const auto claimed = claimed_.find(start); claimed != claimed_.end()) {
            const auto& [slot, index] = claimed->second;
            Proposal& owner = slot.has_value() ? slots_[slot->volume][slot->index].replacements[index] : loose_[index];
            report_.carvesCompeting += group.size();
            for (FileCandidate& carve : group) {
                owner.otherCarves.push_back(std::move(carve));
            }
            continue;
        }
        // A filesystem candidate starts here (one whose first cluster another
        // file has not taken since): the same file.
        std::optional<SlotRef> owner;
        const auto [from, to] = byStart_.equal_range(start);
        for (auto at = from; at != to; ++at) {
            const SlotRef ref = at->second;
            const RecoveryCandidate& candidate = volumes_[ref.volume].second->candidates[ref.index];
            Slot& slot = slots_[ref.volume][ref.index];
            if (!slot.replacements.empty() || slot.carve.has_value() || startReallocated(candidate)) {
                continue;
            }
            if (!owner.has_value() || (candidate.filesystemEvidence.state == filesystem::EntryState::Active &&
                                       volumes_[owner->volume].second->candidates[owner->index].isDeleted())) {
                owner = ref;
            }
        }
        if (owner.has_value()) {
            Slot& slot = slots_[owner->volume][owner->index];
            slot.carve = std::move(group.front());
            ++report_.carvesMerged;
            report_.carvesCompeting += group.size() - 1;
            for (std::size_t i = 1; i < group.size(); ++i) {
                slot.otherCarves.push_back(std::move(group[i]));
            }
            continue;
        }
        // A file of its own.
        Proposal proposal;
        proposal.data = fromCarve(group.front());
        proposal.formatId = group.front().formatId;
        if (group.front().validation.status != ValidationStatus::NotValidated) {
            proposal.knownStructure = group.front().validation;
        }
        if (const auto failed = failedCarves_.find({start, group.front().formatId}); failed != failedCarves_.end()) {
            proposal.fragments = failed->second;
        }
        proposal.carve = std::move(group.front());
        report_.carvesCompeting += group.size() - 1;
        for (std::size_t i = 1; i < group.size(); ++i) {
            proposal.otherCarves.push_back(std::move(group[i]));
        }
        proposal.examineAllocation = true;
        loose_.push_back(std::move(proposal));
    }
}

std::optional<std::size_t> Run::volumeAt(std::uint64_t sourceOffset) const {
    for (std::size_t v = 0; v < volumes_.size(); ++v) {
        FilesystemRecovery& recovery = *volumes_[v].first;
        const filesystem::FilesystemInfo& info = recovery.filesystem().info();
        const std::uint64_t begin = recovery.volumeOffset();
        if (sourceOffset >= begin && sourceOffset - begin < info.volumeSize) {
            return v;
        }
    }
    return std::nullopt;
}

const std::vector<ActiveData>& Run::activeData(std::size_t volume) {
    // A map's elements stay where they are when others are added, so the
    // reference outlives the lock.
    const std::lock_guard lock(activeLock_);
    auto found = active_.find(volume);
    if (found != active_.end()) {
        return found->second;
    }
    std::vector<ActiveData> data;
    const CandidateScan& scan = *volumes_[volume].second;
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
    return active_.emplace(volume, std::move(data)).first->second;
}

Result<AllocationScan> Run::allocationOf(std::size_t volume, std::uint64_t begin, std::uint64_t end) {
    const std::lock_guard lock(*volumeLocks_[volume]);
    FilesystemRecovery& recovery = *volumes_[volume].first;
    filesystem::IFilesystem& fs = recovery.filesystem();
    const filesystem::FilesystemInfo& info = fs.info();
    AllocationScan scan;
    AllocationEvidence& evidence = scan.evidence;
    evidence.filesystem = info.type;
    evidence.volumeOffset = recovery.volumeOffset();
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
                if (index - first >= options_.maxClusterChecks) {
                    evidence.complete = false;
                    break;
                }
                if (Status stop = cancelled(); !stop.ok()) {
                    return stop.error();
                }
                Result<filesystem::ClusterState> state =
                    fs.clusterState(filesystem::ClusterNumber{info.firstCluster + index});
                if (!state.ok() ||
                    (*state != filesystem::ClusterState::Free && *state != filesystem::ClusterState::Allocated)) {
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
    const std::vector<ActiveData>& data = activeData(volume);
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
        if (at->begin <= begin && at->end >= end && !scan.container.has_value()) {
            evidence.insideActiveFile = true;
            scan.container = at->candidate;
        }
        if (std::find(named.begin(), named.end(), at->candidate) == named.end()) {
            named.push_back(at->candidate);
            if (evidence.activeFiles.size() < kMaxActiveFiles) {
                const RecoveryCandidate& file = volumes_[volume].second->candidates[at->candidate];
                evidence.activeFiles.push_back(file.filesystemEvidence.path);
            }
        }
    }
    return scan;
}

Result<const carving::IFileFormat*> Run::detectFormat(carving::IContentReader& content, std::string_view extension) {
    std::uint64_t headerSize = 0;
    for (const auto& format : formats_.formats()) {
        headerSize = std::max<std::uint64_t>(headerSize, format->descriptor().headerSize);
    }
    const auto length = static_cast<std::size_t>(std::min(content.size(), headerSize));
    if (length == 0) {
        return static_cast<const carving::IFileFormat*>(nullptr);
    }
    Result<std::span<const std::byte>> read = content.read(0, length);
    if (!read.ok()) {
        return read.error();
    }
    const std::vector<std::byte> header(read->begin(), read->end());
    std::vector<const carving::IFileFormat*> plausible;
    for (const auto& format : formats_.formats()) {
        const std::size_t take = std::min<std::size_t>(header.size(), format->descriptor().headerSize);
        if (format->checkHeader(std::span(header).first(take)).plausible) {
            plausible.push_back(format.get());
        }
    }
    if (plausible.empty()) {
        // No header check takes it (they are carving's heuristics: a damaged
        // header fails them): the format the file's name gives, so that the
        // structure says what is wrong.
        for (const auto& format : formats_.formats()) {
            if (extensionMatches(extension, format->descriptor())) {
                return format.get();
            }
        }
        return static_cast<const carving::IFileFormat*>(nullptr);
    }
    if (plausible.size() == 1) {
        return plausible.front();
    }
    for (const carving::IFileFormat* format : plausible) {
        if (extensionMatches(extension, format->descriptor())) {
            return format;
        }
    }
    // Several formats take the header and the name says nothing: the first whose structure validates.
    for (const carving::IFileFormat* format : plausible) {
        Result<carving::ValidationResult> verdict = format->validator().validate(content);
        if (!verdict.ok()) {
            return verdict.error();
        }
        if (verdict->status == ValidationStatus::Valid) {
            return format;
        }
    }
    return plausible.front();
}

Result<EvaluatedCandidate> Run::evaluate(Proposal proposal, std::optional<SlotRef> slot) {
    EvaluatedCandidate candidate;
    candidate.filesystemCandidate = proposal.filesystemCandidate;
    candidate.mp4Candidate = proposal.mp4Candidate;
    candidate.mp4 = std::move(proposal.mp4);
    candidate.mp4Warnings = std::move(proposal.mp4Warnings);
    candidate.fragments = std::move(proposal.fragments);
    candidate.otherCarves = std::move(proposal.otherCarves);
    candidate.allocation = std::move(proposal.allocation);

    // A carve of its own: where it lies in the volume's allocation.
    if (proposal.examineAllocation && proposal.carve.has_value() && !proposal.data.sourceRegions.empty()) {
        const FileCandidate& carve = *proposal.carve;
        if (const std::optional<std::size_t> volume = volumeAt(carve.sourceOffset)) {
            const std::uint64_t end = carve.extents.empty()
                                          ? carve.sourceEnd()
                                          : carve.extents.back().sourceOffset + carve.extents.back().length;
            Result<AllocationScan> scan = allocationOf(*volume, carve.sourceOffset, std::max(end, carve.sourceOffset));
            if (!scan.ok()) {
                return scan.error();
            }
            if (scan->container.has_value()) {
                // Part of an active file: its clusters are that file's.
                const auto found = slotFirstId_.find(SlotRef{*volume, *scan->container});
                if (found != slotFirstId_.end()) {
                    candidate.container = found->second;
                }
            } else if (!scan->allocated.empty()) {
                proposal.data.sourceRegions = markAllocated(proposal.data.sourceRegions, scan->allocated);
                proposal.data.warnings.push_back(CandidateWarning::ClustersReallocated);
            }
            candidate.allocation = std::move(scan->evidence);
        }
    }

    Result<std::unique_ptr<CandidateContentReader>> content =
        CandidateContentReader::open(source_, proposal.data, options_.reads, options_.readCacheSize);
    if (!content.ok()) {
        return content.error();
    }
    const carving::IFileFormat* format = nullptr;
    if (!proposal.formatId.empty()) {
        format = formats_.find(proposal.formatId);
    }
    if (format == nullptr && !proposal.mergeCarve) {
        Result<const carving::IFileFormat*> detected = detectFormat(**content, proposal.data.extension);
        if (!detected.ok()) {
            return detected.error();
        }
        format = *detected;
    }
    Result<validation::ValidationState> state =
        validation::validateContent(**content, format, media_, options_.validation, proposal.knownStructure);
    if (!state.ok()) {
        return state.error();
    }

    // P12's rule: a carve that starts where the file starts gives the layout
    // when the metadata's own data does not validate and the carve does, or
    // when the metadata locates no data at all.
    const bool locatesNothing =
        proposal.data.bytes(RegionKind::Stored) + proposal.data.bytes(RegionKind::Embedded) == 0;
    if (proposal.mergeCarve && proposal.carve.has_value() && slot.has_value() &&
        state->structural.status != validation::LevelStatus::Passed &&
        (proposal.carve->validation.status == ValidationStatus::Valid || locatesNothing)) {
        const FileCandidate& carve = *proposal.carve;
        RecoveryCandidate hybrid = proposal.data;
        std::vector<SourceRegion> regions = fromCarve(carve).sourceRegions;
        bool takenSince = false;
        if (hybrid.isDeleted()) {
            Result<AllocationScan> scan = allocationOf(slot->volume, carve.sourceOffset, carve.sourceEnd());
            if (!scan.ok()) {
                return scan.error();
            }
            regions = markAllocated(regions, scan->allocated);
            takenSince = !regions.empty() && regions.front().reallocated;
        }
        if (!takenSince) {
            hybrid.method = RecoveryMethod::Hybrid;
            hybrid.expectedSize = carve.length;
            hybrid.embeddedData.clear();
            hybrid.sourceRegions = std::move(regions);
            hybrid.fragmentation = fromCarve(carve).fragmentation;
            // What the metadata said about the regions it located no longer
            // applies; LayoutGuessed stays (the carve assumes contiguity too).
            std::erase_if(hybrid.warnings, [](CandidateWarning warning) {
                return warning == CandidateWarning::DataMissing || warning == CandidateWarning::ClustersReallocated ||
                       warning == CandidateWarning::DataNotDecoded;
            });
            if (std::any_of(hybrid.sourceRegions.begin(), hybrid.sourceRegions.end(),
                            [](const SourceRegion& region) { return region.reallocated; })) {
                hybrid.warnings.push_back(CandidateWarning::ClustersReallocated);
            }
            Result<std::unique_ptr<CandidateContentReader>> hybridContent =
                CandidateContentReader::open(source_, hybrid, options_.reads, options_.readCacheSize);
            if (!hybridContent.ok()) {
                return hybridContent.error();
            }
            // The carve's own verdict is on exactly these bytes.
            std::optional<carving::ValidationResult> verdict;
            if (carve.validation.status != ValidationStatus::NotValidated) {
                verdict = carve.validation;
            }
            Result<validation::ValidationState> hybridState =
                validation::validateContent(**hybridContent, format, media_, options_.validation, verdict);
            if (!hybridState.ok()) {
                return hybridState.error();
            }
            proposal.data = std::move(hybrid);
            content = std::move(hybridContent);
            state = std::move(hybridState);
        }
    }

    // P12's rule: a deleted file's guessed layout that its structure
    // validates is the metadata's start and the structure's confirmation
    // together (unless another file has taken the first cluster since).
    if (proposal.filesystemLayout && proposal.data.method == RecoveryMethod::Filesystem &&
        proposal.data.filesystemEvidence.allocation.layout == LayoutEvidence::Guessed &&
        state->structural.status == validation::LevelStatus::Passed && !startReallocated(proposal.data)) {
        proposal.data.method = RecoveryMethod::Hybrid;
    }

    Result<ContentIdentity> identity = computeIdentity(**content, options_.identity);
    if (!identity.ok()) {
        return identity.error();
    }
    std::uint64_t unreadable = 0;
    for (const storage::BadRegion& region : (*content)->unreadable().regions()) {
        unreadable += region.length;
    }
    for (const auto& [begin, end] : (*content)->outsideSource()) {
        unreadable += end - begin;
    }
    candidate.formatId = format != nullptr ? format->descriptor().id : std::string();
    candidate.data = std::move(proposal.data);
    candidate.carve = std::move(proposal.carve);
    candidate.validation = std::move(*state);
    candidate.identity = std::move(*identity);
    candidate.unreadableBytes = unreadable;
    return candidate;
}

Status Run::deliver(EvaluatedCandidate&& candidate, const EvaluatedCandidateSink& sink) {
    candidate.id = EvaluatedCandidateId{nextId_++};
    if (!candidate.hasFilesystemEvidence()) {
        candidate.data.filename = carvedName(candidate.id.value(), candidate.data.extension);
    }
    if (const std::optional<std::uint64_t> original = duplicates_.add(candidate.identity, candidate.id.value())) {
        candidate.duplicateOf = EvaluatedCandidateId{*original};
    }
    // The evaluation's warnings, from the facts above.
    std::vector<EvaluationWarning>& warnings = candidate.warnings;
    const validation::ValidationState& state = candidate.validation;
    if (state.structural.status == validation::LevelStatus::Failed) {
        warnings.push_back(EvaluationWarning::StructureInvalid);
    }
    if (state.structural.status == validation::LevelStatus::Truncated ||
        state.media.status == validation::LevelStatus::Truncated ||
        state.playability.status == validation::LevelStatus::Truncated) {
        warnings.push_back(EvaluationWarning::ContentTruncated);
    }
    if (state.media.status == validation::LevelStatus::Failed) {
        warnings.push_back(EvaluationWarning::MediaInvalid);
    }
    if (state.playability.status == validation::LevelStatus::Failed) {
        warnings.push_back(EvaluationWarning::PlaybackFailed);
    }
    if (candidate.formatId.empty()) {
        warnings.push_back(EvaluationWarning::FormatUnknown);
    }
    if (candidate.unreadableBytes > 0) {
        warnings.push_back(EvaluationWarning::DataUnreadable);
    }
    if (candidate.duplicateOf.has_value()) {
        warnings.push_back(EvaluationWarning::DuplicateContent);
    }
    if (candidate.fragments.has_value() && candidate.fragments->alternative > 0) {
        warnings.push_back(EvaluationWarning::AlternativeLayout);
    }
    if (candidate.fragments.has_value() && candidate.fragments->status == ReconstructionStatus::Unrecoverable) {
        warnings.push_back(EvaluationWarning::ReconstructionFailed);
    }
    if (candidate.allocation.has_value() && candidate.allocation->insideActiveFile) {
        warnings.push_back(EvaluationWarning::InsideActiveFile);
    }

    countMethod(candidate.data.method);
    countStatus(candidate.validationStatus());
    report_.duplicates += candidate.duplicateOf.has_value() ? 1 : 0;
    report_.alternatives += candidate.hasWarning(EvaluationWarning::AlternativeLayout) ? 1 : 0;
    report_.bytesHashed += options_.identity.sha256 ? candidate.identity.size : 0;
    return sink(std::move(candidate));
}

void Run::countMethod(RecoveryMethod method) {
    switch (method) {
    case RecoveryMethod::Filesystem:
        ++report_.filesystem;
        break;
    case RecoveryMethod::Carving:
        ++report_.carving;
        break;
    case RecoveryMethod::Hybrid:
        ++report_.hybrid;
        break;
    case RecoveryMethod::Fragmented:
        ++report_.fragmented;
        break;
    }
}

void Run::countStatus(ValidationStatus status) {
    switch (status) {
    case ValidationStatus::Valid:
        ++report_.valid;
        break;
    case ValidationStatus::Truncated:
        ++report_.truncated;
        break;
    case ValidationStatus::Invalid:
        ++report_.invalid;
        break;
    case ValidationStatus::NotValidated:
        ++report_.notValidated;
        break;
    }
}

Status Run::replay(const EvaluationRecord& record) {
    if (record.id.value() != nextId_) {
        return makeError(ErrorCode::InvalidInput, "candidate evaluation: a resume record is out of order");
    }
    const std::optional<std::uint64_t> original = duplicates_.add(record.identity, record.id.value());
    if (original.has_value() != record.duplicate) {
        return makeError(ErrorCode::InvalidInput,
                         "candidate evaluation: a resume record does not fit the records before it");
    }
    ++nextId_;
    countMethod(record.method);
    countStatus(record.validationStatus);
    report_.duplicates += record.duplicate ? 1 : 0;
    report_.alternatives += record.alternative ? 1 : 0;
    report_.bytesHashed += options_.identity.sha256 ? record.identity.size : 0;
    return success();
}

void Run::plan(std::vector<FileCandidate>& carves, std::vector<Mp4Candidate>& mp4,
               std::vector<FragmentCandidate>& fragments) {
    buildSlots();
    takeFragments(fragments);
    takeMp4(mp4);
    takeCarves(carves);
    std::uint64_t id = options_.firstId;
    // The volumes' candidates, in scan order, each where its filesystem candidate is.
    for (std::size_t v = 0; v < volumes_.size(); ++v) {
        const CandidateScan& scan = *volumes_[v].second;
        for (std::size_t i = 0; i < scan.candidates.size(); ++i) {
            Slot& slot = slots_[v][i];
            const SlotRef ref{v, i};
            std::vector<Proposal> proposals;
            if (!slot.replacements.empty()) {
                proposals = std::move(slot.replacements);
            } else {
                Proposal own;
                own.data = scan.candidates[i];
                own.filesystemCandidate = scan.candidates[i].id;
                own.filesystemLayout = true;
                own.fragments = std::move(slot.failedReconstruction);
                if (slot.carve.has_value()) {
                    own.formatId = slot.carve->formatId;
                    own.carve = std::move(slot.carve);
                    own.mergeCarve = true;
                }
                own.otherCarves = std::move(slot.otherCarves);
                proposals.push_back(std::move(own));
            }
            slotFirstId_.emplace(ref, EvaluatedCandidateId{id});
            for (Proposal& proposal : proposals) {
                order_.push_back(Item{std::move(proposal), ref});
                ++id;
            }
        }
    }
    // The rest, in source order.
    std::stable_sort(loose_.begin(), loose_.end(), [](const Proposal& a, const Proposal& b) {
        return a.data.sourceOffset().value_or(UINT64_MAX) < b.data.sourceOffset().value_or(UINT64_MAX);
    });
    for (Proposal& proposal : loose_) {
        order_.push_back(Item{std::move(proposal), std::nullopt});
    }
    loose_.clear();
}

Result<EvaluationReport> Run::run(std::vector<FileCandidate> carves, std::vector<Mp4Candidate> mp4,
                                  std::vector<FragmentCandidate> fragments, const EvaluatedCandidateSink& sink) {
    const auto started = std::chrono::steady_clock::now();
    nextId_ = options_.firstId;
    for (std::size_t v = 0; v < volumes_.size(); ++v) {
        volumeLocks_.push_back(std::make_unique<std::mutex>());
    }
    plan(carves, mp4, fragments);
    if (options_.resume.size() > order_.size()) {
        return makeError(ErrorCode::InvalidInput,
                         "candidate evaluation: more candidates to resume after than the inputs give");
    }
    // What an earlier run delivered counts as delivered here.
    for (const EvaluationRecord& record : options_.resume) {
        if (Status replayed = replay(record); !replayed.ok()) {
            return replayed.error();
        }
    }
    std::size_t window = options_.window;
    if (options_.pool != nullptr && window == 0) {
        window = std::min<std::size_t>(EvaluationOptions::kMaxWindow, 2 * std::size_t{options_.pool->threadCount()});
    }
    const auto produce = [&](std::size_t index) -> Result<EvaluatedCandidate> {
        if (Status stop = cancelled(); !stop.ok()) {
            return stop.error();
        }
        Item& item = order_[index];
        return evaluate(std::move(item.proposal), item.slot);
    };
    const auto consume = [&](std::size_t, EvaluatedCandidate&& candidate) -> Status {
        return deliver(std::move(candidate), sink);
    };
    if (Status ran = runOrdered<EvaluatedCandidate>(options_.pool, window, options_.resume.size(), order_.size(),
                                                    produce, consume);
        !ran.ok()) {
        return ran.error();
    }
    report_.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    return report_;
}

}  // namespace

Status validate(const EvaluationOptions& options) {
    if (options.maxClusterChecks == 0) {
        return makeError(ErrorCode::InvalidInput, "candidate evaluation: maxClusterChecks must not be 0");
    }
    if (options.readCacheSize < carving::SourceContentReader::kMinCacheSize ||
        options.readCacheSize > carving::IContentReader::kMaxReadLength) {
        return makeError(ErrorCode::InvalidInput, "candidate evaluation: the read cache size is out of range");
    }
    if (Status reads = carving::validate(options.reads); !reads.ok()) {
        return reads;
    }
    if (options.window > EvaluationOptions::kMaxWindow) {
        return makeError(ErrorCode::InvalidInput, "candidate evaluation: the window is larger than " +
                                                      std::to_string(EvaluationOptions::kMaxWindow));
    }
    for (std::size_t i = 0; i < options.resume.size(); ++i) {
        if (options.resume[i].id.value() != options.firstId + i) {
            return makeError(ErrorCode::InvalidInput,
                             "candidate evaluation: the resume records are not numbered from the first id on");
        }
    }
    return validation::validate(options.validation.limits);
}

EvaluationRecord recordOf(const EvaluatedCandidate& candidate) {
    EvaluationRecord record;
    record.id = candidate.id;
    record.method = candidate.data.method;
    record.validationStatus = candidate.validationStatus();
    record.identity = candidate.identity;
    record.duplicate = candidate.duplicateOf.has_value();
    record.alternative = candidate.hasWarning(EvaluationWarning::AlternativeLayout);
    return record;
}

CandidateEvaluation::CandidateEvaluation(storage::IStorageSource& source, const carving::FormatRegistry& formats,
                                         const validation::MediaValidatorRegistry& media, EvaluationOptions options)
    : source_(&source), formats_(&formats), media_(&media), options_(std::move(options)) {}

Status CandidateEvaluation::addVolume(FilesystemRecovery& volume, const CandidateScan& scan) {
    if (scan.volumeOffset != volume.volumeOffset()) {
        return makeError(ErrorCode::InvalidInput, "candidate evaluation: the scan is of another volume");
    }
    for (const Volume& added : volumes_) {
        if (added.recovery == &volume || added.scan->volumeOffset == scan.volumeOffset) {
            return makeError(ErrorCode::InvalidInput, "candidate evaluation: the volume is added already");
        }
    }
    for (const RecoveryCandidate& candidate : scan.candidates) {
        if (Status valid = validateCandidate(candidate); !valid.ok()) {
            return valid;
        }
    }
    volumes_.push_back(Volume{&volume, &scan});
    return success();
}

Status CandidateEvaluation::addCarve(carving::FileCandidate carve) {
    if (Status valid = carving::validateFileCandidate(carve); !valid.ok()) {
        return valid;
    }
    carves_.push_back(std::move(carve));
    return success();
}

Status CandidateEvaluation::addMp4Candidate(Mp4Candidate candidate) {
    if (Status valid = validateCandidate(candidate.data); !valid.ok()) {
        return valid;
    }
    mp4_.push_back(std::move(candidate));
    return success();
}

Status CandidateEvaluation::addFragmentCandidate(FragmentCandidate candidate) {
    for (const ReconstructionHypothesis& hypothesis : candidate.hypotheses) {
        if (Status valid = validateCandidate(hypothesis.data); !valid.ok()) {
            return valid;
        }
    }
    fragments_.push_back(std::move(candidate));
    return success();
}

Result<EvaluationReport> CandidateEvaluation::run(const EvaluatedCandidateSink& sink) {
    if (Status valid = validate(options_); !valid.ok()) {
        return valid.error();
    }
    if (!source_->isOpen()) {
        return makeError(ErrorCode::InvalidInput, "candidate evaluation: the source is not open");
    }
    std::vector<std::pair<FilesystemRecovery*, const CandidateScan*>> volumes;
    volumes.reserve(volumes_.size());
    for (const Volume& volume : volumes_) {
        volumes.emplace_back(volume.recovery, volume.scan);
    }
    Run run(*source_, *formats_, *media_, options_, volumes);
    return run.run(carves_, mp4_, fragments_, sink);
}

}  // namespace recovery::evaluation
