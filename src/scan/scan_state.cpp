#include "scan/scan_state.hpp"

#include <algorithm>
#include <set>
#include <string>

namespace recovery::scan {

namespace {

Error refused(std::string message) {
    return makeError(ErrorCode::InvalidInput, "checkpoint: " + std::move(message));
}

}  // namespace

std::string_view toString(ScanStage stage) noexcept {
    switch (stage) {
    case ScanStage::Volumes:
        return "volumes";
    case ScanStage::Mp4Examination:
        return "mp4-examination";
    case ScanStage::FragmentSeeds:
        return "fragment-seeds";
    case ScanStage::SourcePass:
        return "source-pass";
    case ScanStage::Mp4Delivery:
        return "mp4-delivery";
    case ScanStage::Fragments:
        return "fragments";
    case ScanStage::Evaluation:
        return "evaluation";
    case ScanStage::Completed:
        return "completed";
    }
    return "unknown";
}

ScanStage nextStage(ScanStage stage, ScanMode mode) noexcept {
    if (mode == ScanMode::Quick) {
        return stage == ScanStage::Volumes ? ScanStage::Evaluation : ScanStage::Completed;
    }
    switch (stage) {
    case ScanStage::Volumes:
        return ScanStage::Mp4Examination;
    case ScanStage::Mp4Examination:
        return ScanStage::FragmentSeeds;
    case ScanStage::FragmentSeeds:
        return ScanStage::SourcePass;
    case ScanStage::SourcePass:
        return ScanStage::Mp4Delivery;
    case ScanStage::Mp4Delivery:
        return ScanStage::Fragments;
    case ScanStage::Fragments:
        return ScanStage::Evaluation;
    case ScanStage::Evaluation:
    case ScanStage::Completed:
        return ScanStage::Completed;
    }
    return ScanStage::Completed;
}

Status ScanCheckpoint::check(const ScanUpdate& update) const {
    if (update.sequence != sequence_ + 1) {
        return refused("update " + std::to_string(update.sequence) + " does not follow update " +
                       std::to_string(sequence_));
    }
    if (sequence_ == 0 && !update.identity.has_value()) {
        return refused("a scan's first update names the scan");
    }
    if (sequence_ != 0 && update.identity.has_value()) {
        return refused("only a scan's first update names the scan");
    }
    if (stage_ == ScanStage::Completed) {
        return refused("the scan is complete");
    }
    if (update.stage != stage_) {
        return refused("an update of stage '" + std::string(toString(update.stage)) + "' while stage '" +
                       std::string(toString(stage_)) + "' is in progress");
    }
    const auto belongs = [&](bool present, ScanStage stage, std::string_view part) -> Status {
        if (present && update.stage != stage) {
            return refused(std::string(part) + " in an update of stage '" + std::string(toString(update.stage)) +
                           "'");
        }
        return success();
    };
    for (const Status& part : {
             belongs(update.partitionTable.has_value() || update.volumePlan.has_value() || !update.volumes.empty(),
                     ScanStage::Volumes, "volumes"),
             belongs(!update.mp4Examinations.empty(), ScanStage::Mp4Examination, "MP4 examinations"),
             belongs(!update.seedExaminations.empty(), ScanStage::FragmentSeeds, "seed examinations"),
             belongs(update.pass.has_value() || !update.carves.empty() || update.mp4Changes.has_value() ||
                         !update.fragmentEvents.empty(),
                     ScanStage::SourcePass, "the source pass"),
             belongs(!update.mp4Candidates.empty() || update.mp4Report.has_value(), ScanStage::Mp4Delivery,
                     "MP4 candidates"),
             belongs(!update.fragmentCandidates.empty() || update.fragmentReport.has_value(), ScanStage::Fragments,
                     "fragment candidates"),
             belongs(!update.candidates.empty() || update.evaluationReport.has_value(), ScanStage::Evaluation,
                     "evaluated candidates"),
         }) {
        if (!part.ok()) {
            return part;
        }
    }
    const std::uint64_t sourceSize =
        identity_.has_value() ? identity_->sourceSize : (update.identity ? update.identity->sourceSize : 0);
    for (const storage::BadRegion& region : update.unreadable) {
        if (region.length == 0 || region.offset > sourceSize || region.length > sourceSize - region.offset) {
            return refused("an unreadable region outside the source");
        }
    }

    switch (update.stage) {
    case ScanStage::Volumes: {
        if (volumesPlanned_ == update.volumePlan.has_value() ||
            volumesPlanned_ == update.partitionTable.has_value()) {
            return refused("the volume stage's first update, and only it, plans the volumes");
        }
        const std::vector<VolumeRecord>& plan = update.volumePlan.has_value() ? *update.volumePlan : volumes_;
        for (const VolumeRecord& volume : plan) {
            if (volume.offset > sourceSize || volume.size > sourceSize - volume.offset) {
                return refused("a volume outside the source");
            }
        }
        std::set<std::size_t> done;
        for (std::size_t i = 0; i < plan.size(); ++i) {
            if (plan[i].scanned && !update.volumePlan.has_value()) {
                done.insert(i);
            }
        }
        for (const auto& [index, record] : update.volumes) {
            if (index >= plan.size() || done.contains(index) || !record.scanned ||
                record.offset != plan[index].offset || record.size != plan[index].size ||
                record.candidates.has_value() == record.error.has_value()) {
                return refused("a volume result that does not fit the volumes planned");
            }
            if (record.candidates.has_value() && record.candidates->volumeOffset != record.offset) {
                return refused("a volume's candidates are of another volume");
            }
            done.insert(index);
        }
        if (update.stageComplete && done.size() != plan.size()) {
            return refused("the volume stage ends before every volume is done");
        }
        break;
    }
    case ScanStage::Mp4Examination:
    case ScanStage::FragmentSeeds: {
        // In scan order, after the ones recorded.
        std::optional<std::pair<std::size_t, std::size_t>> last;
        if (update.stage == ScanStage::Mp4Examination && !mp4Examinations_.empty()) {
            last.emplace(mp4Examinations_.back().volume, mp4Examinations_.back().index);
        }
        if (update.stage == ScanStage::FragmentSeeds && !seedExaminations_.empty()) {
            last.emplace(seedExaminations_.back().volume, seedExaminations_.back().index);
        }
        const auto follows = [&](std::size_t volume, std::size_t index) {
            const std::pair<std::size_t, std::size_t> at{volume, index};
            const bool ok = !last.has_value() || *last < at;
            last = at;
            return ok;
        };
        for (const Mp4Examination& examination : update.mp4Examinations) {
            if (!follows(examination.volume, examination.index) ||
                (examination.pending.has_value() && examination.pending->volume != examination.volume)) {
                return refused("MP4 examinations out of scan order");
            }
        }
        for (const FragmentSeedExamination& examination : update.seedExaminations) {
            if (!follows(examination.volume, examination.index) || (examination.seed && examination.skipped)) {
                return refused("seed examinations out of scan order");
            }
        }
        break;
    }
    case ScanStage::SourcePass: {
        if (!update.pass.has_value()) {
            return refused("a source pass update without the pass's state");
        }
        const PassState& pass = *update.pass;
        const std::uint64_t previousPosition = pass_.has_value() ? pass_->position : 0;
        const std::uint64_t previousId = pass_.has_value() ? pass_->nextCarveId : 1;
        if (pass.position < previousPosition || pass.position > sourceSize) {
            return refused("the pass's position goes back, or beyond the source");
        }
        std::uint64_t id = previousId;
        for (const carving::FileCandidate& carve : update.carves) {
            if (carve.id.value() != id++) {
                return refused("carves out of order");
            }
            if (Status valid = carving::validateFileCandidate(carve); !valid.ok()) {
                return refused("a carve that is not well formed: " + valid.error().message);
            }
        }
        if (pass.nextCarveId != id) {
            return refused("the pass's next carve id does not follow its carves");
        }
        if (update.mp4Changes.has_value()) {
            for (const auto& [index, pending] : update.mp4Changes->pending) {
                if (index >= mp4State_.pending.size() || pending.volume != mp4State_.pending[index].volume) {
                    return refused("an MP4 change of a candidate that was not examined");
                }
            }
        }
        break;
    }
    case ScanStage::Mp4Delivery:
        if (!update.stageComplete) {
            return refused("MP4 candidates come in one update");
        }
        break;
    case ScanStage::Fragments: {
        std::uint64_t id = fragmentCandidates_.size() + 1;
        for (const FragmentCandidate& candidate : update.fragmentCandidates) {
            if (candidate.id.value() != id++) {
                return refused("fragment candidates out of order");
            }
        }
        break;
    }
    case ScanStage::Evaluation: {
        std::uint64_t id = evaluated_.size() + 1;
        for (const evaluation::EvaluatedCandidate& candidate : update.candidates) {
            if (candidate.id.value() != id++) {
                return refused("evaluated candidates out of order");
            }
        }
        break;
    }
    case ScanStage::Completed:
        break;
    }
    return success();
}

Status ScanCheckpoint::apply(const ScanUpdate& update) {
    if (Status valid = check(update); !valid.ok()) {
        return valid;
    }
    sequence_ = update.sequence;
    if (update.identity.has_value()) {
        identity_ = update.identity;
    }
    switch (update.stage) {
    case ScanStage::Volumes:
        if (update.partitionTable.has_value()) {
            partitionTable_ = update.partitionTable;
        }
        if (update.volumePlan.has_value()) {
            volumes_ = *update.volumePlan;
            volumesPlanned_ = true;
        }
        for (const auto& [index, record] : update.volumes) {
            volumes_[index] = record;
        }
        break;
    case ScanStage::Mp4Examination:
        for (const Mp4Examination& examination : update.mp4Examinations) {
            mp4State_.report.filesystemExamined += examination.analysed ? 1 : 0;
            mp4State_.report.filesystemMp4 += examination.video ? 1 : 0;
            if (examination.pending.has_value()) {
                mp4State_.pending.push_back(*examination.pending);
            }
            mp4Examinations_.push_back(examination);
        }
        break;
    case ScanStage::FragmentSeeds:
        seedExaminations_.insert(seedExaminations_.end(), update.seedExaminations.begin(),
                                 update.seedExaminations.end());
        break;
    case ScanStage::SourcePass:
        pass_ = update.pass;
        carves_.insert(carves_.end(), update.carves.begin(), update.carves.end());
        if (update.mp4Changes.has_value()) {
            const Mp4StepsChanges& changes = *update.mp4Changes;
            for (const auto& [index, pending] : changes.pending) {
                mp4State_.pending[index] = pending;
            }
            mp4State_.carved.insert(mp4State_.carved.end(), changes.carved.begin(), changes.carved.end());
            mp4State_.trustedStart = changes.trustedStart;
            mp4State_.trustedEnd = changes.trustedEnd;
            mp4State_.nextCarveId = changes.nextCarveId;
            mp4State_.report = changes.report;
        }
        fragmentEvents_.insert(fragmentEvents_.end(), update.fragmentEvents.begin(), update.fragmentEvents.end());
        break;
    case ScanStage::Mp4Delivery:
        mp4Candidates_ = update.mp4Candidates;
        mp4Report_ = update.mp4Report;
        break;
    case ScanStage::Fragments:
        fragmentCandidates_.insert(fragmentCandidates_.end(), update.fragmentCandidates.begin(),
                                   update.fragmentCandidates.end());
        if (update.fragmentReport.has_value()) {
            fragmentReport_ = update.fragmentReport;
        }
        break;
    case ScanStage::Evaluation:
        for (const evaluation::EvaluatedCandidate& candidate : update.candidates) {
            evaluated_.push_back(evaluation::recordOf(candidate));
        }
        if (update.evaluationReport.has_value()) {
            evaluationReport_ = update.evaluationReport;
        }
        break;
    case ScanStage::Completed:
        break;
    }
    for (const storage::BadRegion& region : update.unreadable) {
        (void)unreadable_.add(region);  // checked above
    }
    metrics_ = update.metrics;
    if (update.stageComplete) {
        stage_ = nextStage(stage_, identity_->mode);
    }
    return success();
}

}  // namespace recovery::scan
