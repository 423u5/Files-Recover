// Scans through the API (P19): what a scan delivers is what the engine
// delivers, the list a user interface shows, the progress and events of a
// scan, settings, and one operation at a time.

#include "api_test_support.hpp"

#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <set>

namespace recovery::api::test {
namespace {

std::optional<CandidateInfo> named(const std::vector<CandidateInfo>& candidates, std::string_view name) {
    const auto found = std::find_if(candidates.begin(), candidates.end(),
                                    [&](const CandidateInfo& candidate) { return candidate.name == name; });
    return found == candidates.end() ? std::nullopt : std::optional(*found);
}

TEST(ApiScanTest, ADeepScanDeliversWhatTheEngineDelivers) {
    ApiWorld world;
    const std::string id = world.startCardScan();
    ASSERT_FALSE(id.empty());
    const Progress done = world.waitIdle(id);
    EXPECT_EQ(done.operation, OperationKind::Scan);
    ASSERT_EQ(done.state, OperationState::Completed) << (done.error ? describe(*done.error) : "");
    ASSERT_TRUE(done.scan.has_value());
    EXPECT_EQ(done.scan->stage, ScanStage::Completed);
    EXPECT_EQ(done.scan->fraction, 1.0);
    EXPECT_EQ(done.scan->metrics.candidates, expectedCard().size());

    const std::vector<CandidateInfo> candidates = allCandidates(world.api(), id);
    ASSERT_EQ(candidates.size(), expectedCard().size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        EXPECT_EQ(candidates[i].id.value, i + 1);
        EXPECT_EQ(candidates[i].recovery, RecoveryState::NotRecovered);
    }
    const std::optional<CandidateInfo> photo = named(candidates, "PHOTO.JPG");
    const std::optional<CandidateInfo> copy = named(candidates, "COPY.JPG");
    const std::optional<CandidateInfo> deleted = named(candidates, "_LD.JPG");
    ASSERT_TRUE(photo && copy && deleted);
    EXPECT_EQ(photo->kind, MediaKind::Image);
    EXPECT_EQ(photo->format, "jpeg");
    EXPECT_EQ(photo->mediaType, "image/jpeg");
    EXPECT_EQ(photo->extension, "jpg");
    EXPECT_EQ(photo->path, "/PHOTO.JPG");
    EXPECT_EQ(photo->foundBy, FoundBy::Filesystem);
    EXPECT_EQ(photo->condition, Condition::Complete);
    EXPECT_EQ(photo->validation.result, ValidationResult::Valid);
    EXPECT_EQ(photo->validation.structural, CheckResult::Passed);
    EXPECT_EQ(photo->size, ::recovery::test::cardOriginals().at("PHOTO.JPG").size());
    EXPECT_EQ(photo->sha256.size(), 64u);
    EXPECT_FALSE(photo->deleted);
    EXPECT_FALSE(photo->duplicateOf.has_value());
    ASSERT_TRUE(copy->duplicateOf.has_value());
    EXPECT_EQ(*copy->duplicateOf, photo->id);
    EXPECT_EQ(copy->sha256, photo->sha256);
    EXPECT_TRUE(deleted->deleted);
    EXPECT_TRUE(std::any_of(candidates.begin(), candidates.end(),
                            [](const CandidateInfo& c) { return c.foundBy == FoundBy::Carving; }));

    // The session holds exactly what the engine's stages deliver.
    world.restart();
    EXPECT_EQ(journalCandidates(world.sessions(), id), expectedCard());
}

TEST(ApiScanTest, DocumentsAndArchivesAreToldApart) {
    ApiWorld world;
    const ::recovery::test::DocumentCard card = ::recovery::test::documentCard();
    const std::filesystem::path image = world.folder() / "documents.img";
    ::recovery::test::writeFile(image, card.image);
    const Result<std::string> id = world.api().startScan(SourceRef::imageFile(image), ScanSettings{});
    RECOVERY_ASSERT_OK(id);
    ASSERT_EQ(world.waitIdle(*id).state, OperationState::Completed);
    const std::vector<CandidateInfo> candidates = allCandidates(world.api(), *id);

    struct Expected {
        std::string_view name;
        MediaKind kind;
        std::string_view format;
        std::string_view mediaType;
    };
    for (const Expected& expected : std::vector<Expected>{
             {"REPORT.PDF", MediaKind::Document, "pdf", "application/pdf"},
             {"LETTER.DOC", MediaKind::Document, "ole2", "application/msword"},
             {"BOOK.DOCX", MediaKind::Document, "zip",
              "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
             {"APP.APK", MediaKind::Archive, "zip", "application/vnd.android.package-archive"},
             {"NOTES.ZIP", MediaKind::Archive, "zip", "application/zip"}}) {
        SCOPED_TRACE(expected.name);
        const std::optional<CandidateInfo> candidate = named(candidates, expected.name);
        ASSERT_TRUE(candidate.has_value());
        EXPECT_EQ(candidate->kind, expected.kind);
        EXPECT_EQ(candidate->format, expected.format);
        EXPECT_EQ(candidate->mediaType, expected.mediaType);
        EXPECT_EQ(candidate->condition, Condition::Complete);
    }
    // The DOCX whose entry is gone: carved, and named by its members.
    const auto carved = std::find_if(candidates.begin(), candidates.end(),
                                     [](const CandidateInfo& c) { return c.foundBy == FoundBy::Carving; });
    ASSERT_NE(carved, candidates.end());
    EXPECT_EQ(carved->kind, MediaKind::Document);
    EXPECT_EQ(carved->extension, "docx");
    EXPECT_TRUE(carved->name.ends_with(".docx")) << carved->name;
    EXPECT_EQ(carved->condition, Condition::Complete);
    EXPECT_EQ(toString(MediaKind::Document), "document");

    CandidateQuery query;
    query.filter.kinds = {MediaKind::Document};
    const Result<CandidatePage> documents = world.api().getCandidates(*id, query);
    RECOVERY_ASSERT_OK(documents);
    EXPECT_EQ(documents->matching, 4u);
}

TEST(ApiScanTest, AQuickScanReadsTheFilesystemOnly) {
    ApiWorld world;
    ScanSettings settings;
    settings.mode = ScanMode::Quick;
    const std::string id = world.startCardScan(settings);
    const Progress done = world.waitIdle(id);
    ASSERT_EQ(done.state, OperationState::Completed);
    EXPECT_EQ(done.scan->stageCount, 2u);
    const std::vector<CandidateInfo> candidates = allCandidates(world.api(), id);
    EXPECT_EQ(candidates.size(), expectedCard(ScanMode::Quick).size());
    EXPECT_TRUE(std::none_of(candidates.begin(), candidates.end(),
                             [](const CandidateInfo& c) { return c.foundBy == FoundBy::Carving; }));
    const Result<SessionDetails> details = world.api().getSession(id);
    RECOVERY_ASSERT_OK(details);
    EXPECT_EQ(details->settings.mode, ScanMode::Quick);
    world.restart();
    EXPECT_EQ(journalCandidates(world.sessions(), id), expectedCard(ScanMode::Quick));
}

TEST(ApiScanTest, SettingsOutOfRangeAreRefusedBeforeASessionIsMade) {
    ApiWorld world;
    ScanSettings settings;
    settings.alignment = 0;
    RECOVERY_EXPECT_ERROR(world.api().startScan(world.cardSource(), settings), ErrorCode::InvalidInput);
    settings = ScanSettings{};
    settings.activeFiles = false;
    settings.deletedFiles = false;
    settings.mode = ScanMode::Quick;
    const Result<std::string> nothing = world.api().startScan(world.cardSource(), settings);
    if (nothing.ok()) {
        // The engine may accept a scan that looks for no filesystem file.
        (void)world.waitIdle(*nothing);
    }
    settings = ScanSettings{};
    settings.maxSignatures = 0;
    RECOVERY_EXPECT_ERROR(world.api().startScan(world.cardSource(), settings), ErrorCode::InvalidInput);
    const Result<std::vector<SessionListing>> sessions = world.api().listSessions();
    RECOVERY_ASSERT_OK(sessions);
    EXPECT_EQ(sessions->size(), nothing.ok() ? 1u : 0u);
}

// L228: where the stages stopped at the limit is in the metrics.
TEST(ApiScanTest, TheMetricsSayWhereTheStagesStoppedAtTheLimit) {
    ApiWorld world;
    ScanSettings settings;
    settings.maxSignatures = 1;
    const std::string id = world.startCardScan(settings);
    const Progress done = world.waitIdle(id);
    ASSERT_EQ(done.state, OperationState::Completed);
    ASSERT_TRUE(done.scan.has_value());
    const ScanMetrics& metrics = done.scan->metrics;
    ASSERT_TRUE(metrics.carvingStoppedAt.has_value());
    EXPECT_LT(*metrics.carvingStoppedAt, metrics.sourceSize);
    EXPECT_EQ(metrics.carves, 1u);

    const std::string whole = world.startCardScan();
    const Progress all = world.waitIdle(whole);
    ASSERT_EQ(all.state, OperationState::Completed);
    EXPECT_FALSE(all.scan->metrics.carvingStoppedAt.has_value());
    EXPECT_FALSE(all.scan->metrics.mp4StoppedAt.has_value());
    EXPECT_FALSE(all.scan->metrics.fragmentsStoppedAt.has_value());
}

TEST(ApiScanTest, TheSettingsAreKeptWithTheSession) {
    ApiWorld world;
    ScanSettings settings;
    settings.carving = false;
    settings.alignment = 512;
    settings.sectorRetries = 3;
    settings.mediaValidation = false;
    const std::string id = world.startCardScan(settings);
    ASSERT_EQ(world.waitIdle(id).state, OperationState::Completed);
    const Result<SessionDetails> details = world.api().getSession(id);
    RECOVERY_ASSERT_OK(details);
    EXPECT_EQ(details->settings.mode, ScanMode::Deep);
    EXPECT_FALSE(details->settings.carving);
    EXPECT_EQ(details->settings.alignment, 512u);
    EXPECT_EQ(details->settings.sectorRetries, 3u);
    EXPECT_FALSE(details->settings.mediaValidation);
    EXPECT_EQ(details->source.kind, SourceKind::DiskImage);
    EXPECT_EQ(details->scan.state, RunState::Completed);
    EXPECT_FALSE(details->scan.resumable);
    ASSERT_EQ(details->volumes.size(), 1u);
    EXPECT_EQ(details->volumes[0].filesystem, FilesystemKind::Fat32);
    EXPECT_TRUE(details->volumes[0].scanned);
    EXPECT_EQ(details->partitionScheme, PartitionScheme::Unpartitioned);
}

TEST(ApiScanTest, TheEventsTellTheWholeScanInOrder) {
    ApiWorld world;
    const std::string id = world.startCardScan();
    ASSERT_TRUE(world.log().waitForFinished(id).has_value());
    const std::vector<Event> events = world.log().eventsOf(id);
    ASSERT_GE(events.size(), 3u);
    EXPECT_EQ(events.front().kind, EventKind::OperationStarted);
    EXPECT_EQ(events.front().progress.operation, OperationKind::Scan);
    EXPECT_EQ(events.back().kind, EventKind::OperationFinished);
    EXPECT_EQ(events.back().progress.state, OperationState::Completed);

    double fraction = 0.0;
    std::uint64_t next = 1;
    std::size_t progress = 0;
    for (const Event& event : events) {
        EXPECT_EQ(event.session, id);
        EXPECT_FALSE(event.imaging.has_value());
        if (event.kind == EventKind::Progress) {
            ++progress;
            ASSERT_TRUE(event.progress.scan.has_value());
            EXPECT_GE(event.progress.scan->fraction, fraction) << "the progress went back";
            fraction = event.progress.scan->fraction;
            EXPECT_GE(event.progress.scan->stageNumber, 1u);
            EXPECT_LE(event.progress.scan->stageNumber, event.progress.scan->stageCount);
        }
        if (event.kind == EventKind::CandidatesFound) {
            EXPECT_EQ(event.firstCandidate.value, next) << "candidates found out of order";
            next = event.firstCandidate.value + event.candidateCount;
        }
    }
    EXPECT_GT(progress, 0u);
    EXPECT_EQ(next, expectedCard().size() + 1) << "every candidate is found once";
}

TEST(ApiScanTest, ASessionRunsOneOperationAtATime) {
    std::atomic<bool> held{false};
    RecoveryApi* api = nullptr;
    ApiWorld world;
    world.hooks().onOperationProgress = [&](std::string_view session, const Progress& progress) {
        if (!held.exchange(true) && progress.scan.has_value()) {
            EXPECT_TRUE(api->pauseScan(session).ok());
        }
    };
    api = &world.api();
    const std::string id = world.startCardScan();
    ASSERT_TRUE(world.log().waitFor([&](const std::vector<Event>& events) {
        return std::any_of(events.begin(), events.end(), [](const Event& e) { return e.kind == EventKind::Paused; });
    }));
    RecoveryOptions options;
    options.destination = world.folder() / "out";
    RECOVERY_EXPECT_ERROR(api->recoverAll(id, options), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(api->closeSession(id), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(api->resumeRecovery(id), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(api->pauseRecovery(id), ErrorCode::InvalidInput);
    // Another session of the same source runs beside it.
    const std::string other = world.startCardScan();
    EXPECT_NE(other, id);
    EXPECT_EQ(world.waitIdle(other).state, OperationState::Completed);
    RECOVERY_ASSERT_OK(api->resumeScan(id));
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Completed);
}

TEST(ApiScanTest, AnIdleSessionReportsWhatItsScanRecorded) {
    ApiWorld world;
    const std::string id = world.startCardScan();
    ASSERT_EQ(world.waitIdle(id).state, OperationState::Completed);
    world.restart();
    RECOVERY_ASSERT_OK(world.api().openSession(id));
    const Result<Progress> idle = world.api().getProgress(id);
    RECOVERY_ASSERT_OK(idle);
    EXPECT_EQ(idle->operation, OperationKind::None);
    EXPECT_EQ(idle->state, OperationState::Idle);
    ASSERT_TRUE(idle->scan.has_value());
    EXPECT_EQ(idle->scan->stage, ScanStage::Completed);
    EXPECT_EQ(idle->scan->fraction, 1.0);
    EXPECT_EQ(idle->scan->metrics.candidates, expectedCard().size());
    // A complete scan has nothing to resume, nor anything to pause or cancel.
    RECOVERY_EXPECT_ERROR(world.api().resumeScan(id), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world.api().pauseScan(id), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world.api().cancelScan(id), ErrorCode::InvalidInput);
}

}  // namespace
}  // namespace recovery::api::test
