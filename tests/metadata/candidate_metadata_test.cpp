// Candidate metadata (P17): the condition a candidate would be recovered
// in, what a list shows about it, duplicate content under different names,
// and what recovery jobs did with it; on candidates made here, and on the
// scan tests' FAT32 card: its candidates' media metadata read from the
// source, its duplicates, and a session's recovery jobs.

#include "metadata/candidate_metadata.hpp"
#include "metadata/media_metadata.hpp"
#include "metadata/recovery_status.hpp"

#include "scan/scan_test_support.hpp"
#include "session/recovery_session.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <map>

namespace recovery::metadata {
namespace {

using evaluation::EvaluatedCandidate;
using evaluation::EvaluatedCandidateId;
using evaluation::EvaluationWarning;
using validation::LevelStatus;

EvaluatedCandidateId id(std::uint64_t value) {
    return EvaluatedCandidateId{value};
}

// A whole file of 1000 bytes on the source, its structure validated.
EvaluatedCandidate complete(std::uint64_t value = 1) {
    EvaluatedCandidate candidate;
    candidate.id = id(value);
    candidate.formatId = "jpeg";
    candidate.data.method = RecoveryMethod::Carving;
    candidate.data.filename = "recovered_000001.jpg";
    candidate.data.extension = "jpg";
    candidate.data.expectedSize = 1000;
    candidate.data.sourceRegions = {SourceRegion{0, 1000, RegionKind::Stored, 8192, false}};
    candidate.data.fragmentation.fragmentCount = 1;
    candidate.identity.size = 1000;
    candidate.validation.structural.status = LevelStatus::Passed;
    candidate.validation.media.status = LevelStatus::Passed;
    return candidate;
}

std::vector<ConditionReason> reasons(std::initializer_list<ConditionReason> list) {
    return std::vector<ConditionReason>(list);
}

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------

TEST(ConditionTest, CompleteWhenEveryByteIsThereAndValid) {
    const ConditionAssessment assessment = assessCondition(complete());
    EXPECT_EQ(assessment.condition, RecoveryCondition::Complete);
    EXPECT_TRUE(assessment.reasons.empty());
    EXPECT_EQ(toString(assessment.condition), "COMPLETE");
}

TEST(ConditionTest, UnverifiedWhenNothingCheckedIt) {
    EvaluatedCandidate candidate = complete();
    candidate.formatId.clear();
    candidate.validation = {};
    ConditionAssessment assessment = assessCondition(candidate);
    EXPECT_EQ(assessment.condition, RecoveryCondition::Unverified);
    EXPECT_EQ(assessment.reasons, reasons({ConditionReason::NotValidated}));
    // An empty file: complete when its format validates it, else unverified.
    EvaluatedCandidate empty = complete();
    empty.data.expectedSize = 0;
    empty.data.sourceRegions.clear();
    empty.identity.size = 0;
    EXPECT_EQ(assessCondition(empty).condition, RecoveryCondition::Complete);
    empty.validation = {};
    EXPECT_EQ(assessCondition(empty).condition, RecoveryCondition::Unverified);
}

TEST(ConditionTest, CorruptedByEachKindOfDamage) {
    EvaluatedCandidate failed = complete();
    failed.validation.media.status = LevelStatus::Failed;
    EvaluatedCandidate unreadable = complete();
    unreadable.unreadableBytes = 512;
    EvaluatedCandidate reallocated = complete();
    reallocated.data.sourceRegions = {SourceRegion{0, 500, RegionKind::Stored, 8192, false},
                                      SourceRegion{500, 500, RegionKind::Stored, 8692, true}};
    EvaluatedCandidate damaged = complete();
    damaged.fragments = evaluation::FragmentEvidence{};
    damaged.fragments->status = ReconstructionStatus::Corrupted;
    const std::vector<std::pair<EvaluatedCandidate, ConditionReason>> cases = {
        {failed, ConditionReason::ValidationFailed},
        {unreadable, ConditionReason::DataUnreadable},
        {reallocated, ConditionReason::ClustersReallocated},
        {damaged, ConditionReason::ReconstructionCorrupted},
    };
    for (const auto& [candidate, reason] : cases) {
        const ConditionAssessment assessment = assessCondition(candidate);
        EXPECT_EQ(assessment.condition, RecoveryCondition::Corrupted) << toString(reason);
        EXPECT_EQ(assessment.reasons, reasons({reason})) << toString(reason);
    }
}

TEST(ConditionTest, PartialWinsOverCorruptedAndListsBoth) {
    EvaluatedCandidate missing = complete();
    missing.data.sourceRegions = {SourceRegion{0, 600, RegionKind::Stored, 8192, false},
                                  SourceRegion{600, 400, RegionKind::Missing, 0, false}};
    missing.identity.size = 600;
    missing.validation.structural.status = LevelStatus::Truncated;
    missing.validation.media.status = LevelStatus::NotRun;
    missing.unreadableBytes = 10;
    const ConditionAssessment assessment = assessCondition(missing);
    EXPECT_EQ(assessment.condition, RecoveryCondition::Partial);
    EXPECT_EQ(assessment.reasons, reasons({ConditionReason::DataMissing, ConditionReason::ContentTruncated,
                                           ConditionReason::DataUnreadable}));

    EvaluatedCandidate partial = complete();
    partial.fragments = evaluation::FragmentEvidence{};
    partial.fragments->status = ReconstructionStatus::Partial;
    EXPECT_EQ(assessCondition(partial).condition, RecoveryCondition::Partial);
    EXPECT_TRUE(assessCondition(partial).has(ConditionReason::ReconstructionPartial));
}

TEST(ConditionTest, UnrecoverableWhenNothingIsLocatedOrValidates) {
    EvaluatedCandidate nothing = complete();
    nothing.data.sourceRegions = {SourceRegion{0, 1000, RegionKind::Missing, 0, false}};
    nothing.identity.size = 0;
    nothing.validation = {};
    ConditionAssessment assessment = assessCondition(nothing);
    EXPECT_EQ(assessment.condition, RecoveryCondition::Unrecoverable);
    EXPECT_EQ(assessment.reasons, reasons({ConditionReason::NothingLocated, ConditionReason::DataMissing,
                                           ConditionReason::NotValidated}));

    EvaluatedCandidate failed = complete();
    failed.warnings.push_back(EvaluationWarning::ReconstructionFailed);
    failed.validation.structural.status = LevelStatus::Failed;
    assessment = assessCondition(failed);
    EXPECT_EQ(assessment.condition, RecoveryCondition::Unrecoverable);
    EXPECT_EQ(assessment.reasons,
              reasons({ConditionReason::ReconstructionFailed, ConditionReason::ValidationFailed}));
}

TEST(ConditionTest, AmbiguousComesFirst) {
    EvaluatedCandidate candidate = complete();
    candidate.warnings.push_back(EvaluationWarning::AlternativeLayout);
    candidate.fragments = evaluation::FragmentEvidence{};
    candidate.fragments->status = ReconstructionStatus::Ambiguous;
    candidate.data.sourceRegions.push_back(SourceRegion{1000, 24, RegionKind::Missing, 0, false});
    candidate.data.expectedSize = 1024;
    const ConditionAssessment assessment = assessCondition(candidate);
    EXPECT_EQ(assessment.condition, RecoveryCondition::Ambiguous);
    EXPECT_EQ(assessment.reasons, reasons({ConditionReason::AlternativeLayout, ConditionReason::DataMissing}));
    EXPECT_EQ(toString(ConditionReason::AlternativeLayout), "ALTERNATIVE_LAYOUT");
}

// ---------------------------------------------------------------------------
// What a list shows
// ---------------------------------------------------------------------------

TEST(DescribeCandidateTest, FilesystemCandidate) {
    EvaluatedCandidate candidate = complete(7);
    candidate.data.method = RecoveryMethod::Filesystem;
    candidate.data.filename = "IMG_0001.JPG";
    candidate.data.filesystemEvidence.path = "/DCIM/IMG_0001.JPG";
    candidate.data.filesystemEvidence.state = filesystem::EntryState::Deleted;
    filesystem::Timestamp modified;
    modified.time = std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{1'000'000}};
    modified.local = true;
    candidate.data.filesystemEvidence.modified = modified;
    candidate.filesystemCandidate = CandidateId{3};
    candidate.identity.sha256 = Sha256Digest{};
    candidate.duplicateOf = id(2);
    candidate.validation.playability.status = LevelStatus::Unsupported;
    const CandidateMetadata metadata = describeCandidate(candidate);
    EXPECT_EQ(metadata.id, id(7));
    EXPECT_EQ(metadata.name, "IMG_0001.JPG");
    EXPECT_EQ(metadata.path, "/DCIM/IMG_0001.JPG");
    EXPECT_EQ(metadata.extension, "jpg");
    EXPECT_TRUE(metadata.deleted);
    EXPECT_EQ(metadata.method, RecoveryMethod::Filesystem);
    EXPECT_EQ(metadata.kind, MediaKind::Image);
    EXPECT_EQ(metadata.formatId, "jpeg");
    EXPECT_EQ(metadata.mediaType, "image/jpeg");
    EXPECT_EQ(metadata.size, 1000U);
    EXPECT_EQ(metadata.expectedSize, 1000U);
    EXPECT_EQ(metadata.sourceOffset, 8192U);
    EXPECT_EQ(metadata.fragments, 1U);
    ASSERT_TRUE(metadata.modified.has_value());
    EXPECT_TRUE(metadata.modified->local);
    EXPECT_FALSE(metadata.created.has_value());
    EXPECT_EQ(metadata.validation, carving::ValidationStatus::Valid);
    EXPECT_EQ(metadata.structural, LevelStatus::Passed);
    EXPECT_EQ(metadata.media, LevelStatus::Passed);
    EXPECT_EQ(metadata.playability, LevelStatus::Unsupported);
    EXPECT_EQ(metadata.deepestPassed, validation::ValidationLevel::Media);
    EXPECT_EQ(metadata.condition, RecoveryCondition::Complete);
    EXPECT_TRUE(metadata.reasons.empty());
    EXPECT_TRUE(metadata.sha256.has_value());
    EXPECT_EQ(metadata.duplicateOf, id(2));
    EXPECT_FALSE(metadata.container.has_value());
}

TEST(DescribeCandidateTest, CarvedCandidateHasNoPathAndIsNotCalledDeleted) {
    EvaluatedCandidate candidate = complete(4);
    candidate.container = id(1);
    candidate.formatId = "mp4";
    const CandidateMetadata metadata = describeCandidate(candidate);
    EXPECT_TRUE(metadata.path.empty());
    EXPECT_FALSE(metadata.deleted);
    EXPECT_EQ(metadata.method, RecoveryMethod::Carving);
    EXPECT_EQ(metadata.kind, MediaKind::Video);
    EXPECT_EQ(metadata.mediaType, "video/mp4");
    EXPECT_EQ(metadata.container, id(1));
    EXPECT_FALSE(metadata.created.has_value());
}

// ---------------------------------------------------------------------------
// Duplicates
// ---------------------------------------------------------------------------

TEST(DuplicateGroupsTest, GroupsFromTheEvaluationsLinks) {
    std::vector<EvaluatedCandidate> candidates;
    for (std::uint64_t i = 1; i <= 7; ++i) {
        candidates.push_back(complete(i));
    }
    candidates[2].duplicateOf = id(1);  // 3 -> 1
    candidates[4].duplicateOf = id(1);  // 5 -> 1
    candidates[5].duplicateOf = id(2);  // 6 -> 2
    // Delivered out of id order: the group still lists them by id.
    std::swap(candidates[2], candidates[4]);
    const DuplicateGroups groups = DuplicateGroups::build(candidates);
    ASSERT_EQ(groups.groups().size(), 2U);
    EXPECT_EQ(groups.groups()[0].members, (std::vector<EvaluatedCandidateId>{id(1), id(3), id(5)}));
    EXPECT_EQ(groups.groups()[0].original(), id(1));
    EXPECT_EQ(groups.groups()[0].size, 1000U);
    EXPECT_EQ(groups.groups()[1].members, (std::vector<EvaluatedCandidateId>{id(2), id(6)}));
    EXPECT_EQ(groups.duplicateCount(), 3U);
    ASSERT_NE(groups.groupOf(id(5)), nullptr);
    EXPECT_EQ(groups.groupOf(id(5))->original(), id(1));
    EXPECT_EQ(groups.groupOf(id(1)), groups.groupOf(id(3)));
    EXPECT_EQ(groups.groupOf(id(4)), nullptr);
    EXPECT_EQ(groups.groupOf(id(7)), nullptr);
}

TEST(DuplicateGroupsTest, APageWithoutTheOriginalStillNamesIt) {
    std::vector<EvaluatedCandidate> page = {complete(10), complete(11)};
    page[0].duplicateOf = id(2);
    page[1].duplicateOf = id(2);
    const DuplicateGroups groups = DuplicateGroups::build(page);
    ASSERT_EQ(groups.groups().size(), 1U);
    EXPECT_EQ(groups.groups()[0].members, (std::vector<EvaluatedCandidateId>{id(2), id(10), id(11)}));
    EXPECT_TRUE(DuplicateGroups::build({}).groups().empty());
}

// ---------------------------------------------------------------------------
// Recovery status
// ---------------------------------------------------------------------------

TEST(RecoveryJobIndexTest, StatesOverJobs) {
    RecoveryJobIndex index;
    scan::RecoveredItem written;
    written.candidate = id(1);
    written.file = RecoveredFile{CandidateId{1}, "D:/out/a.jpg", ReconstructionReport{}};
    scan::RecoveredItem partial;
    partial.candidate = id(2);
    partial.file = RecoveredFile{CandidateId{2}, "D:/out/b.jpg", ReconstructionReport{}};
    partial.file->report.unreadableBytes = 512;
    scan::RecoveredItem failed;
    failed.candidate = id(3);
    failed.error = makeError(ErrorCode::PartialRecovery, "no data located");
    // Job 1 wrote 1 and 2, could not write 3, has not reached 4.
    index.addJob(1, std::vector<EvaluatedCandidateId>{id(1), id(2), id(3), id(4)},
                 std::vector<scan::RecoveredItem>{written, partial, failed});
    // Job 2 writes 3 again and has not run.
    index.addJob(2, std::vector<EvaluatedCandidateId>{id(3)}, {});
    EXPECT_EQ(index.jobCount(), 2U);

    const CandidateRecovery first = index.recoveryOf(id(1));
    EXPECT_EQ(first.state, RecoveryState::Recovered);
    EXPECT_TRUE(first.complete);
    ASSERT_EQ(first.jobs.size(), 1U);
    EXPECT_EQ(first.jobs[0].job, 1U);
    EXPECT_EQ(first.jobs[0].path, std::filesystem::path("D:/out/a.jpg"));
    EXPECT_TRUE(first.jobs[0].report.has_value());

    const CandidateRecovery second = index.recoveryOf(id(2));
    EXPECT_EQ(second.state, RecoveryState::Recovered);
    EXPECT_FALSE(second.complete);

    // Failed in job 1, pending in job 2: pending wins.
    const CandidateRecovery third = index.recoveryOf(id(3));
    EXPECT_EQ(third.state, RecoveryState::Pending);
    ASSERT_EQ(third.jobs.size(), 2U);
    EXPECT_EQ(third.jobs[0].state, RecoveryState::Failed);
    ASSERT_TRUE(third.jobs[0].error.has_value());
    EXPECT_EQ(third.jobs[0].error->code, ErrorCode::PartialRecovery);
    EXPECT_EQ(third.jobs[1].state, RecoveryState::Pending);

    EXPECT_EQ(index.stateOf(id(4)), RecoveryState::Pending);
    EXPECT_EQ(index.stateOf(id(5)), RecoveryState::NotRecovered);
    EXPECT_TRUE(index.recoveryOf(id(5)).jobs.empty());

    RecoveryJobIndex failedOnly;
    failedOnly.addJob(1, std::vector<EvaluatedCandidateId>{id(3)}, std::vector<scan::RecoveredItem>{failed});
    EXPECT_EQ(failedOnly.stateOf(id(3)), RecoveryState::Failed);
    EXPECT_EQ(toString(RecoveryState::Failed), "failed");
}

// ---------------------------------------------------------------------------
// The scan tests' card
// ---------------------------------------------------------------------------

const std::vector<std::byte>& card() {
    static const std::vector<std::byte> bytes = scan::test::makeCard();
    return bytes;
}

const std::vector<EvaluatedCandidate>& cardCandidates() {
    static const std::vector<EvaluatedCandidate> candidates = [] {
        ::recovery::test::MemoryStorageSource source(card());
        const bool opened = source.open().ok();
        return opened ? scan::test::referenceScan(source, ScanMode::Deep) : std::vector<EvaluatedCandidate>{};
    }();
    return candidates;
}

const EvaluatedCandidate* named(std::string_view name) {
    for (const EvaluatedCandidate& candidate : cardCandidates()) {
        if (candidate.data.filename == name) {
            return &candidate;
        }
    }
    return nullptr;
}

TEST(CardMetadataTest, DuplicateContentUnderDifferentNames) {
    const std::vector<EvaluatedCandidate>& candidates = cardCandidates();
    ASSERT_FALSE(candidates.empty());
    const EvaluatedCandidate* photo = named("PHOTO.JPG");
    const EvaluatedCandidate* copy = named("COPY.JPG");
    ASSERT_NE(photo, nullptr);
    ASSERT_NE(copy, nullptr);
    const DuplicateGroups groups = DuplicateGroups::build(candidates);
    const DuplicateGroup* group = groups.groupOf(copy->id);
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->original(), photo->id);
    EXPECT_EQ(group->members, (std::vector<EvaluatedCandidateId>{photo->id, copy->id}));
    EXPECT_EQ(group->sha256, photo->identity.sha256);
    EXPECT_EQ(group->size, photo->recoveredSize());
    const CandidateMetadata original = describeCandidate(*photo);
    const CandidateMetadata duplicate = describeCandidate(*copy);
    EXPECT_NE(original.name, duplicate.name);
    EXPECT_EQ(original.sha256, duplicate.sha256);
    EXPECT_FALSE(original.duplicateOf.has_value());
    EXPECT_EQ(duplicate.duplicateOf, photo->id);
    // The MP4 the card holds twice: once named, once only carving finds.
    const EvaluatedCandidate* clip = named("CLIP.MP4");
    ASSERT_NE(clip, nullptr);
    const DuplicateGroup* clips = groups.groupOf(clip->id);
    ASSERT_NE(clips, nullptr);
    ASSERT_EQ(clips->members.size(), 2U);
    EXPECT_EQ(clips->original(), clip->id);
    for (const EvaluatedCandidate& candidate : candidates) {
        if (candidate.id == clips->members[1]) {
            EXPECT_EQ(candidate.data.method, RecoveryMethod::Carving);
            EXPECT_TRUE(describeCandidate(candidate).path.empty());
        }
    }
    // Each group's members share one digest.
    for (const DuplicateGroup& each : groups.groups()) {
        for (const EvaluatedCandidateId member : each.members) {
            for (const EvaluatedCandidate& candidate : candidates) {
                if (candidate.id == member) {
                    EXPECT_EQ(candidate.identity.sha256, each.sha256);
                }
            }
        }
    }
    EXPECT_EQ(groups.duplicateCount(), 2U);
}

TEST(CardMetadataTest, MediaMetadataOfEveryCandidate) {
    ::recovery::test::MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    std::map<std::string, MediaMetadata> byName;
    for (const EvaluatedCandidate& candidate : cardCandidates()) {
        Result<MediaMetadata> metadata = readMediaMetadata(source, candidate);
        RECOVERY_ASSERT_OK(metadata);
        EXPECT_EQ(metadata->formatId, candidate.formatId);
        const CandidateMetadata described = describeCandidate(candidate);
        if (described.condition == RecoveryCondition::Complete) {
            EXPECT_EQ(metadata->kind, described.kind) << candidate.data.filename;
            EXPECT_FALSE(metadata->previews.empty()) << candidate.data.filename;
            EXPECT_EQ(metadata->issueCount, 0U) << candidate.data.filename;
        }
        byName[candidate.data.filename] = std::move(metadata).value();
    }
    ASSERT_TRUE(byName.contains("PHOTO.JPG"));
    ASSERT_TRUE(byName["PHOTO.JPG"].image.has_value());
    EXPECT_EQ(byName["PHOTO.JPG"].image->width, 128U);
    EXPECT_EQ(byName["PHOTO.JPG"].image->height, 96U);
    // Deleted FAT entries lose their first character: FRAG.JPG, in two
    // fragments that reconstruction put together, and OLD.JPG.
    ASSERT_TRUE(byName.contains("_RAG.JPG"));
    ASSERT_TRUE(byName["_RAG.JPG"].image.has_value());
    EXPECT_EQ(byName["_RAG.JPG"].image->width, 160U);
    EXPECT_EQ(byName["_RAG.JPG"].image->height, 120U);
    ASSERT_TRUE(byName.contains("_LD.JPG"));
    ASSERT_TRUE(byName["_LD.JPG"].image.has_value());
    EXPECT_EQ(byName["_LD.JPG"].image->width, 96U);
    ASSERT_TRUE(byName["PICTURE.PNG"].image.has_value());
    EXPECT_EQ(byName["PICTURE.PNG"].image->width, 32U);
    ASSERT_TRUE(byName["SOUND.WAV"].audio.has_value());
    EXPECT_EQ(byName["SOUND.WAV"].audio->codec, "pcm");
    EXPECT_EQ(byName["SOUND.WAV"].audio->sampleRate, 44100U);
    ASSERT_TRUE(byName["CLIP.MP4"].video.has_value());
    EXPECT_EQ(byName["CLIP.MP4"].video->codec, "h264");
    EXPECT_EQ(byName["CLIP.MP4"].video->width, 64U);
    ASSERT_TRUE(byName["CLIP.MP4"].movie.has_value());
    EXPECT_EQ(byName["CLIP.MP4"].movie->tracks.size(), 2U);
}

TEST(CardMetadataTest, RecoveryStatusOfASessionsJobs) {
    ::recovery::test::TempDir dir;
    ::recovery::test::MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    Result<std::unique_ptr<session::RecoverySession>> created =
        session::RecoverySession::create(dir.path() / "sessions", source, {});
    RECOVERY_ASSERT_OK(created);
    session::RecoverySession& session = **created;
    RECOVERY_ASSERT_OK(session.runScan(source, scan::test::allFormats(), scan::test::allMedia(),
                                       scan::test::testRunOptions()));
    const std::vector<EvaluatedCandidate> candidates = session.candidates();
    ASSERT_GE(candidates.size(), 5U);

    // Nothing recovered yet.
    EXPECT_EQ(RecoveryJobIndex::fromSession(session).stateOf(id(1)), RecoveryState::NotRecovered);

    // Job 1 writes the first three; job 2 the fourth, and does not run.
    Result<std::uint32_t> first = session.addRecoveryJob(dir.path() / "out", {id(1), id(2), id(3)});
    RECOVERY_ASSERT_OK(first);
    RECOVERY_ASSERT_OK(session.runRecovery(*first, source));
    Result<std::uint32_t> second = session.addRecoveryJob(dir.path() / "later", {id(4)});
    RECOVERY_ASSERT_OK(second);

    const RecoveryJobIndex index = RecoveryJobIndex::fromSession(session);
    EXPECT_EQ(index.jobCount(), 2U);
    const std::vector<scan::RecoveredItem> items = session.recoveredItems(*first);
    ASSERT_EQ(items.size(), 3U);
    for (const scan::RecoveredItem& item : items) {
        const CandidateRecovery recovery = index.recoveryOf(item.candidate);
        ASSERT_EQ(recovery.jobs.size(), 1U);
        EXPECT_EQ(recovery.jobs[0].job, *first);
        if (item.file.has_value()) {
            EXPECT_EQ(recovery.state, RecoveryState::Recovered);
            EXPECT_EQ(recovery.jobs[0].path, item.file->path);
            EXPECT_TRUE(std::filesystem::exists(recovery.jobs[0].path));
            EXPECT_EQ(recovery.complete, item.file->report.allBytesRead());
        } else {
            EXPECT_EQ(recovery.state, RecoveryState::Failed);
        }
    }
    EXPECT_EQ(index.stateOf(id(4)), RecoveryState::Pending);
    EXPECT_EQ(index.recoveryOf(id(4)).jobs[0].job, *second);
    EXPECT_EQ(index.stateOf(id(5)), RecoveryState::NotRecovered);
}

}  // namespace
}  // namespace recovery::metadata
