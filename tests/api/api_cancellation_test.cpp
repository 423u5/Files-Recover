// Cancellation, pause and resume through the API (P19): a scan cancelled in
// every stage resumes to deliver what an uninterrupted scan delivers; a
// cancellation asked before the engine's first report; pause and resume,
// and a cancellation while paused; a recovery cancelled and resumed writes
// every file once; and an API destroyed while a scan runs leaves a session
// that resumes later.

#include "api_test_support.hpp"

#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <atomic>

namespace recovery::api::test {
namespace {

bool hasEvent(const std::vector<Event>& events, EventKind kind) {
    return std::any_of(events.begin(), events.end(), [kind](const Event& event) { return event.kind == kind; });
}

class ApiCancelStageTest : public ::testing::TestWithParam<ScanStage> {};

TEST_P(ApiCancelStageTest, AScanCancelledInThisStageResumesToTheSameCandidates) {
    const ScanStage stage = GetParam();
    std::atomic<bool> cancelled{false};
    RecoveryApi* api = nullptr;
    ApiWorld world;
    world.hooks().onOperationProgress = [&](std::string_view session, const Progress& progress) {
        if (progress.scan.has_value() && progress.scan->stage == stage && !cancelled.exchange(true)) {
            EXPECT_TRUE(api->cancelScan(session).ok());
        }
    };
    api = &world.api();
    const std::string id = world.startCardScan();
    const Progress stopped = world.waitIdle(id);
    ASSERT_TRUE(cancelled.load()) << "the scan never reported stage " << toString(stage);
    EXPECT_EQ(stopped.state, OperationState::Cancelled);

    const Result<SessionDetails> details = api->getSession(id);
    RECOVERY_ASSERT_OK(details);
    EXPECT_EQ(details->scan.state, RunState::Cancelled);
    EXPECT_TRUE(details->scan.resumable);
    EXPECT_EQ(details->running, OperationKind::None);

    RECOVERY_ASSERT_OK(api->resumeScan(id));
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Completed);
    world.restart();
    EXPECT_EQ(journalCandidates(world.sessions(), id), expectedCard());
}

INSTANTIATE_TEST_SUITE_P(EveryStage, ApiCancelStageTest,
                         ::testing::Values(ScanStage::Volumes, ScanStage::Mp4Examination, ScanStage::FragmentSeeds,
                                           ScanStage::SourcePass, ScanStage::Mp4Delivery, ScanStage::Fragments,
                                           ScanStage::Evaluation),
                         [](const ::testing::TestParamInfo<ScanStage>& info) {
                             std::string name(toString(info.param));
                             name.erase(std::remove(name.begin(), name.end(), '-'), name.end());
                             return name;
                         });

TEST(ApiCancellationTest, ACancellationAskedAtOnceStopsTheScanAtItsStart) {
    ApiWorld world;
    const std::string id = world.startCardScan();
    RECOVERY_ASSERT_OK(world.api().cancelScan(id));
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Cancelled);
    const std::optional<Event> finished = world.log().waitForFinished(id);
    ASSERT_TRUE(finished.has_value());
    EXPECT_EQ(finished->progress.state, OperationState::Cancelled);
    RECOVERY_ASSERT_OK(world.api().resumeScan(id));
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Completed);
}

TEST(ApiCancellationTest, APausedScanWaitsUntilResumed) {
    std::atomic<bool> paused{false};
    RecoveryApi* api = nullptr;
    ApiWorld world;
    world.hooks().onOperationProgress = [&](std::string_view session, const Progress& progress) {
        if (progress.scan.has_value() && progress.scan->stage == ScanStage::SourcePass && !paused.exchange(true)) {
            EXPECT_TRUE(api->pauseScan(session).ok());
        }
    };
    api = &world.api();
    const std::string id = world.startCardScan();
    ASSERT_TRUE(world.log().waitFor([&](const std::vector<Event>&) { return paused.load(); }));

    // Paused: it stays so, and says so everywhere.
    const Result<Progress> waited = api->waitForOperation(id, std::chrono::milliseconds(300));
    RECOVERY_ASSERT_OK(waited);
    EXPECT_EQ(waited->state, OperationState::Paused);
    EXPECT_EQ(waited->scan->stage, ScanStage::SourcePass);
    const Result<SessionDetails> details = api->getSession(id);
    RECOVERY_ASSERT_OK(details);
    EXPECT_EQ(details->running, OperationKind::Scan);
    EXPECT_TRUE(details->paused);
    EXPECT_EQ(details->scan.state, RunState::Paused);
    RECOVERY_ASSERT_OK(api->pauseScan(id));  // paused already: nothing to do

    RECOVERY_ASSERT_OK(api->resumeScan(id));
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Completed);
    const std::vector<Event> events = world.log().eventsOf(id);
    EXPECT_TRUE(hasEvent(events, EventKind::Paused));
    EXPECT_TRUE(hasEvent(events, EventKind::Resumed));
    world.restart();
    EXPECT_EQ(journalCandidates(world.sessions(), id), expectedCard());
}

TEST(ApiCancellationTest, APausedScanCanBeCancelled) {
    std::atomic<bool> paused{false};
    RecoveryApi* api = nullptr;
    ApiWorld world;
    world.hooks().onOperationProgress = [&](std::string_view session, const Progress& progress) {
        if (progress.scan.has_value() && progress.scan->stage == ScanStage::SourcePass && !paused.exchange(true)) {
            EXPECT_TRUE(api->pauseScan(session).ok());
        }
    };
    api = &world.api();
    const std::string id = world.startCardScan();
    ASSERT_TRUE(world.log().waitFor([&](const std::vector<Event>&) { return paused.load(); }));
    RECOVERY_ASSERT_OK(api->cancelScan(id));
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Cancelled);
    const Result<SessionDetails> details = api->getSession(id);
    RECOVERY_ASSERT_OK(details);
    EXPECT_EQ(details->scan.state, RunState::Cancelled);
}

TEST(ApiCancellationTest, ACancelledRecoveryResumesWithoutWritingAFileTwice) {
    std::atomic<bool> cancelled{false};
    RecoveryApi* api = nullptr;
    ApiWorld world;
    world.hooks().onOperationProgress = [&](std::string_view session, const Progress& progress) {
        if (progress.recovery.has_value() && !cancelled.exchange(true)) {
            EXPECT_TRUE(api->cancelRecovery(session).ok());
        }
    };
    api = &world.api();
    const std::string id = world.startCardScan();
    ASSERT_EQ(world.waitIdle(id).state, OperationState::Completed);
    const std::filesystem::path out = world.folder() / "out";
    RecoveryOptions options;
    options.destination = out;
    const Result<RecoveryStart> start = api->recoverAll(id, options);
    RECOVERY_ASSERT_OK(start);
    ASSERT_TRUE(start->newJob.has_value());
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Cancelled);
    Result<SessionDetails> details = api->getSession(id);
    RECOVERY_ASSERT_OK(details);
    ASSERT_EQ(details->jobs.size(), 1u);
    EXPECT_EQ(details->jobs[0].state, RunState::Cancelled);

    // Asked again for the same folder: the unfinished job is finished, and
    // nothing is added to it.
    const Result<RecoveryStart> again = api->recoverAll(id, options);
    RECOVERY_ASSERT_OK(again);
    EXPECT_EQ(again->jobs, std::vector<std::uint32_t>{*start->newJob});
    EXPECT_FALSE(again->newJob.has_value());
    EXPECT_EQ(again->inUnfinishedJobs + again->alreadyRecovered, expectedCard().size());
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Completed);

    const std::map<std::string, Bytes> files = ::recovery::test::filesBelow(out);
    EXPECT_EQ(files.size(), expectedCard().size());
    for (const auto& [name, bytes] : files) {
        EXPECT_EQ(name.find(" ("), std::string::npos) << name << ": a file was written twice";
    }
    for (const auto& [name, bytes] : ::recovery::test::cardOriginals()) {
        ASSERT_TRUE(files.contains(name)) << name;
        EXPECT_EQ(files.at(name), bytes) << name;
    }
    details = api->getSession(id);
    RECOVERY_ASSERT_OK(details);
    EXPECT_EQ(details->jobs[0].state, RunState::Completed);
    EXPECT_EQ(details->jobs[0].recovered, expectedCard().size());
}

TEST(ApiCancellationTest, ResumeRecoveryFinishesTheJobsThatDidNotEnd) {
    std::atomic<bool> cancelled{false};
    RecoveryApi* api = nullptr;
    ApiWorld world;
    world.hooks().onOperationProgress = [&](std::string_view session, const Progress& progress) {
        if (progress.recovery.has_value() && !cancelled.exchange(true)) {
            EXPECT_TRUE(api->cancelRecovery(session).ok());
        }
    };
    api = &world.api();
    const std::string id = world.startCardScan();
    ASSERT_EQ(world.waitIdle(id).state, OperationState::Completed);
    RecoveryOptions options;
    options.destination = world.folder() / "out";
    RECOVERY_ASSERT_OK(api->recoverAll(id, options));
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Cancelled);
    RECOVERY_ASSERT_OK(api->resumeRecovery(id));
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Completed);
    EXPECT_EQ(::recovery::test::filesBelow(options.destination).size(), expectedCard().size());
    // Nothing is left to resume.
    RECOVERY_EXPECT_ERROR(api->resumeRecovery(id), ErrorCode::InvalidInput);
}

TEST(ApiCancellationTest, DestroyingTheApiStopsTheScanWhichResumesLater) {
    std::atomic<bool> paused{false};
    RecoveryApi* api = nullptr;
    ApiWorld world;
    world.hooks().onOperationProgress = [&](std::string_view session, const Progress& progress) {
        if (progress.scan.has_value() && progress.scan->stage == ScanStage::SourcePass && !paused.exchange(true)) {
            EXPECT_TRUE(api->pauseScan(session).ok());
        }
    };
    api = &world.api();
    const std::string id = world.startCardScan();
    ASSERT_TRUE(world.log().waitFor([&](const std::vector<Event>&) { return paused.load(); }));
    world.restart();  // the program closes while the scan is paused

    world.hooks().onOperationProgress = nullptr;
    const Result<SessionDetails> reopened = world.api().openSession(id);
    RECOVERY_ASSERT_OK(reopened);
    EXPECT_EQ(reopened->scan.state, RunState::Cancelled);
    EXPECT_TRUE(reopened->scan.resumable);
    RECOVERY_ASSERT_OK(world.api().resumeScan(id));
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Completed);
    world.restart();
    EXPECT_EQ(journalCandidates(world.sessions(), id), expectedCard());
}

}  // namespace
}  // namespace recovery::api::test
