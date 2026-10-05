// The scan coordinator (P15) on a used FAT32 card with a file for every
// stage: a scan delivers what the stages of P7-P14 deliver one after the
// other, whatever the number of workers; a scan interrupted in any stage
// (cancelled, its sink failing, a source failing) or after any update
// resumes from its updates and ends as an uninterrupted scan, without
// delivering a candidate twice or doing again work an update recorded; a
// paused scan reads nothing until resumed; checkpoints refuse what does not
// follow; the metrics add up.

#include "scan/scan_coordinator.hpp"

#include "scan_test_support.hpp"
#include "support/memory_source.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <thread>

namespace recovery::scan {
namespace {

using namespace std::chrono_literals;
using test::Bytes;
using test::Collector;

// One card for every test: the reference scan runs once.
const Bytes& card() {
    static const Bytes bytes = test::makeCard();
    return bytes;
}

const std::vector<std::string>& expectedDeep() {
    static const std::vector<std::string> expected = [] {
        ::recovery::test::MemoryStorageSource source(card());
        EXPECT_TRUE(source.open().ok());
        return test::describe(test::referenceScan(source, ScanMode::Deep));
    }();
    return expected;
}

struct Outcome {
    Result<ScanSummary> summary = makeError(ErrorCode::InternalError, "not run");
    Collector collector;
};

// Runs a scan of `source`, resuming from `resume` when given.
Result<ScanSummary> runScan(storage::IStorageSource& source, Collector& collector, ScanConfiguration configuration,
                            ScanRunOptions options, const ScanCheckpoint* resume = nullptr) {
    ScanCoordinator coordinator(source, test::allFormats(), test::allMedia(), std::move(configuration),
                                std::move(options));
    return coordinator.run(collector.sink(), resume);
}

// Runs until the scan completes, resuming after each interruption (at most
// `attempts` runs); every run starts from the collector's checkpoint.
std::size_t runToTheEnd(storage::IStorageSource& source, Collector& collector, const ScanConfiguration& configuration,
                        const ScanRunOptions& options, std::size_t attempts = 1000) {
    std::size_t runs = 0;
    while (!collector.checkpoint.completed() && runs < attempts) {
        ++runs;
        const ScanCheckpoint resume = collector.checkpoint;
        const Result<ScanSummary> summary = runScan(source, collector, configuration, options, &resume);
        // An interruption the test made (a hook's error) is expected; others are not.
        if (!summary.ok() && summary.error().message.find("interrupted by the test") == std::string::npos) {
            ADD_FAILURE() << describe(summary.error());
            break;
        }
    }
    return runs;
}

void expectSameAs(const std::vector<std::string>& expected, const std::vector<evaluation::EvaluatedCandidate>& actual) {
    const std::vector<std::string> lines = test::describe(actual);
    ASSERT_EQ(lines.size(), expected.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        EXPECT_EQ(lines[i], expected[i]) << "candidate " << i;
    }
}

// ===========================================================================
// What a scan delivers
// ===========================================================================

TEST(ScanCoordinatorTest, ADeepScanDeliversWhatTheStagesDeliverOneAfterTheOther) {
    ::recovery::test::MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    Collector collector;
    const Result<ScanSummary> summary = runScan(source, collector, {}, test::testRunOptions());
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, ScanOutcome::Completed);
    EXPECT_EQ(summary->stage, ScanStage::Completed);
    EXPECT_TRUE(collector.checkpoint.completed());
    expectSameAs(expectedDeep(), collector.candidates);

    // Every stage found something on this card.
    const ScanCheckpoint& checkpoint = collector.checkpoint;
    ASSERT_EQ(checkpoint.volumes().size(), 1U);
    ASSERT_TRUE(checkpoint.volumes()[0].candidates.has_value());
    EXPECT_GE(checkpoint.volumes()[0].candidates->candidates.size(), 7U);
    EXPECT_FALSE(checkpoint.carves().empty());
    EXPECT_FALSE(checkpoint.mp4Candidates().empty());
    EXPECT_FALSE(checkpoint.fragmentCandidates().empty());
    std::set<RecoveryMethod> methods;
    std::size_t duplicates = 0;
    for (const evaluation::EvaluatedCandidate& candidate : collector.candidates) {
        methods.insert(candidate.data.method);
        duplicates += candidate.duplicateOf.has_value() ? 1 : 0;
    }
    EXPECT_TRUE(methods.contains(RecoveryMethod::Filesystem));
    EXPECT_TRUE(methods.contains(RecoveryMethod::Hybrid));
    EXPECT_TRUE(methods.contains(RecoveryMethod::Carving));
    EXPECT_TRUE(methods.contains(RecoveryMethod::Fragmented)) << [&] {
        std::string all;
        for (const std::string& line : test::describe(collector.candidates)) {
            all += line + "\n";
        }
        for (const FragmentCandidate& fragment : collector.checkpoint.fragmentCandidates()) {
            all += "fragment " + fragment.name + " " + std::string(toString(fragment.status)) + ": " +
                   fragment.reason + "\n";
        }
        return all;
    }();
    EXPECT_GE(duplicates, 1U);

    // The metrics add up.
    const ScanMetrics& metrics = summary->metrics;
    EXPECT_EQ(metrics.sourceSize, card().size());
    EXPECT_EQ(metrics.bytesScanned, card().size());
    EXPECT_GE(metrics.bytesRead, card().size());
    EXPECT_EQ(metrics.candidates, collector.candidates.size());
    EXPECT_EQ(metrics.filesFound, checkpoint.volumes()[0].candidates->candidates.size());
    EXPECT_EQ(metrics.carves, checkpoint.carves().size());
    EXPECT_EQ(metrics.duplicates, duplicates);
    std::size_t invalid = 0;
    for (const evaluation::EvaluatedCandidate& candidate : collector.candidates) {
        invalid += candidate.validationStatus() == carving::ValidationStatus::Invalid ? 1 : 0;
    }
    EXPECT_EQ(metrics.validationFailures, invalid);
    EXPECT_EQ(metrics.unreadableBytes, 0U);
    EXPECT_EQ(checkpoint.metrics().candidates, metrics.candidates);
}

TEST(ScanCoordinatorTest, TheNumberOfWorkersChangesNothing) {
    for (const std::uint32_t workers : {1U, 2U, 8U}) {
        SCOPED_TRACE(workers);
        ::recovery::test::MemoryStorageSource source(card());
        RECOVERY_ASSERT_OK(source.open());
        Collector collector;
        ScanRunOptions options = test::testRunOptions(workers);
        options.window = workers == 8 ? 0 : 1;  // also one item at a time
        RECOVERY_ASSERT_OK(runScan(source, collector, {}, options));
        expectSameAs(expectedDeep(), collector.candidates);
    }
}

TEST(ScanCoordinatorTest, AQuickScanEvaluatesTheFilesTheMetadataKnows) {
    ::recovery::test::MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    const std::vector<std::string> expected = test::describe(test::referenceScan(source, ScanMode::Quick));
    Collector collector;
    ScanConfiguration configuration;
    configuration.mode = ScanMode::Quick;
    const Result<ScanSummary> summary = runScan(source, collector, configuration, test::testRunOptions());
    RECOVERY_ASSERT_OK(summary);
    expectSameAs(expected, collector.candidates);
    EXPECT_TRUE(collector.checkpoint.carves().empty());
    EXPECT_FALSE(collector.checkpoint.pass().has_value());
    EXPECT_EQ(summary->metrics.bytesScanned, 0U);
    // Only what the metadata names, each read in full once: far less than the card.
    for (const evaluation::EvaluatedCandidate& candidate : collector.candidates) {
        EXPECT_TRUE(candidate.hasFilesystemEvidence()) << test::describe(candidate);
    }
}

TEST(ScanCoordinatorTest, TheStagesTurnedOffAddNothing) {
    ::recovery::test::MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    Collector collector;
    ScanConfiguration configuration;
    configuration.carving = false;
    configuration.mp4 = false;
    configuration.fragments = false;
    RECOVERY_ASSERT_OK(runScan(source, collector, configuration, test::testRunOptions()));
    EXPECT_TRUE(collector.checkpoint.completed());
    EXPECT_TRUE(collector.checkpoint.carves().empty());
    EXPECT_TRUE(collector.checkpoint.mp4Candidates().empty());
    EXPECT_TRUE(collector.checkpoint.fragmentCandidates().empty());
    expectSameAs(test::describe(test::referenceScan(source, ScanMode::Quick)), collector.candidates);
}

// ===========================================================================
// Interruptions
// ===========================================================================

// Cancels the scan once it reaches `stage` and `units` of its units are done.
class CancelAt {
public:
    CancelAt(ScanStage stage, std::uint64_t units) : stage_(stage), units_(units) {}

    void attach(ScanRunOptions& options) {
        JobControl control = options.control;
        options.onProgress = [this, control](const ScanProgress& progress) mutable {
            if (!fired_ && progress.stage == stage_ && progress.stageDone >= units_) {
                fired_ = true;
                control.requestCancellation();
            }
        };
    }
    [[nodiscard]] bool fired() const noexcept { return fired_; }

private:
    ScanStage stage_;
    std::uint64_t units_;
    bool fired_ = false;
};

TEST(ScanCoordinatorTest, AScanCancelledInEveryStageResumesAndEndsAsAnUninterruptedOne) {
    struct Case {
        ScanStage stage;
        std::uint64_t units;
    };
    const std::vector<Case> cases = {
        {ScanStage::Volumes, 0},       {ScanStage::Mp4Examination, 2}, {ScanStage::FragmentSeeds, 2},
        {ScanStage::SourcePass, 1},    {ScanStage::SourcePass, 600 * kKiB}, {ScanStage::Mp4Delivery, 0},
        {ScanStage::Fragments, 0},     {ScanStage::Evaluation, 4},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string(toString(c.stage)) + " at " + std::to_string(c.units));
        ::recovery::test::MemoryStorageSource source(card());
        RECOVERY_ASSERT_OK(source.open());
        Collector collector;
        ScanRunOptions options = test::testRunOptions();
        // One item at a time, so that the cancellation lands between two of
        // them (work in flight when it comes is finished and handed out).
        options.window = 1;
        CancelAt cancel(c.stage, c.units);
        cancel.attach(options);
        const Result<ScanSummary> first = runScan(source, collector, {}, options);
        RECOVERY_ASSERT_OK(first);
        ASSERT_TRUE(cancel.fired());
        EXPECT_EQ(first->outcome, ScanOutcome::Cancelled);
        EXPECT_FALSE(collector.checkpoint.completed());
        // The stage it was cancelled in, or the next when that one had just ended.
        EXPECT_LE(collector.checkpoint.stage(), nextStage(c.stage, ScanMode::Deep));
        const std::size_t before = collector.candidates.size();

        // Resume with a fresh control, from what the updates recorded.
        const std::size_t runs = runToTheEnd(source, collector, {}, test::testRunOptions(2));
        EXPECT_EQ(runs, 1U);
        expectSameAs(expectedDeep(), collector.candidates);
        // Nothing delivered before was delivered again (ids are unique and in order).
        for (std::size_t i = 0; i < collector.candidates.size(); ++i) {
            EXPECT_EQ(collector.candidates[i].id.value(), i + 1);
        }
        EXPECT_LE(before, collector.candidates.size());
    }
}

// The scan stops after every one of its updates in turn (each run is
// cancelled once it has handed out one more update) and is resumed until it
// ends: the candidates are those of an uninterrupted scan, each once.
TEST(ScanCoordinatorTest, AScanResumedAfterEveryUpdateEndsAsAnUninterruptedOne) {
    ::recovery::test::MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    Collector collector;
    std::size_t runs = 0;
    while (!collector.checkpoint.completed() && runs < 500) {
        ++runs;
        ScanRunOptions options = test::testRunOptions(3);
        JobControl control = options.control;
        const std::uint64_t stopAt = collector.checkpoint.sequence() + 1;
        collector.hook = [control, stopAt](const ScanUpdate& update) mutable {
            if (update.sequence >= stopAt) {
                control.requestCancellation();
            }
            return success();
        };
        const ScanCheckpoint resume = collector.checkpoint;
        const Result<ScanSummary> summary = runScan(source, collector, {}, options, &resume);
        RECOVERY_ASSERT_OK(summary);
        if (!collector.checkpoint.completed()) {
            EXPECT_EQ(summary->outcome, ScanOutcome::Cancelled);
        }
    }
    EXPECT_TRUE(collector.checkpoint.completed());
    EXPECT_GE(runs, 10U);
    expectSameAs(expectedDeep(), collector.candidates);
    // The pass committed each hit once: its carves, numbered without gaps.
    const std::vector<carving::FileCandidate>& carves = collector.checkpoint.carves();
    for (std::size_t i = 0; i < carves.size(); ++i) {
        EXPECT_EQ(carves[i].id.value(), i + 1);
    }
}

// A crash before an update is saved: the sink fails, the scan stops with its
// error, and the update is not in the checkpoint. Resumed from what was
// saved, the scan ends as an uninterrupted one.
TEST(ScanCoordinatorTest, AScanWhoseUpdateWasNotSavedResumesFromTheOneBefore) {
    for (const std::uint64_t failAt : {1ULL, 2ULL, 5ULL, 9ULL, 14ULL, 20ULL, 30ULL}) {
        SCOPED_TRACE(failAt);
        ::recovery::test::MemoryStorageSource source(card());
        RECOVERY_ASSERT_OK(source.open());
        Collector collector;
        collector.hook = [failAt](const ScanUpdate& update) -> Status {
            if (update.sequence == failAt) {
                return makeError(ErrorCode::DestinationError, "the session store failed (interrupted by the test)");
            }
            return success();
        };
        const Result<ScanSummary> first = runScan(source, collector, {}, test::testRunOptions());
        if (first.ok()) {
            // The scan had fewer updates than that.
            EXPECT_TRUE(collector.checkpoint.completed());
            continue;
        }
        EXPECT_EQ(first.error().code, ErrorCode::DestinationError);
        EXPECT_EQ(collector.checkpoint.sequence(), failAt - 1);
        collector.hook = nullptr;
        EXPECT_EQ(runToTheEnd(source, collector, {}, test::testRunOptions()), 1U);
        expectSameAs(expectedDeep(), collector.candidates);
    }
}

// A read that fails for a reason other than a bad sector stops the scan; once
// the fault is gone, the scan resumes from its updates.
TEST(ScanCoordinatorTest, AScanStoppedByAFailingSourceResumesOnceItReadsAgain) {
    ::recovery::test::MemoryStorageSource failing(card());
    // In the second half of the card: after the volume stage, during the pass.
    failing.addFatalSector(card().size() / 512 * 3 / 4);
    RECOVERY_ASSERT_OK(failing.open());
    Collector collector;
    const Result<ScanSummary> first = runScan(failing, collector, {}, test::testRunOptions());
    ASSERT_FALSE(first.ok());
    EXPECT_NE(first.error().code, ErrorCode::Cancelled);
    EXPECT_FALSE(collector.checkpoint.completed());

    ::recovery::test::MemoryStorageSource healthy(card());
    RECOVERY_ASSERT_OK(healthy.open());
    EXPECT_EQ(runToTheEnd(healthy, collector, {}, test::testRunOptions()), 1U);
    expectSameAs(expectedDeep(), collector.candidates);
}

TEST(ScanCoordinatorTest, APausedScanReadsNothingUntilResumed) {
    ::recovery::test::VirtualSource source(card().size());
    source.plant(0, card());
    RECOVERY_ASSERT_OK(source.open());
    Collector collector;
    ScanRunOptions options = test::testRunOptions(4);
    JobControl control = options.control;
    std::atomic<bool> pausedOnce{false};
    options.onProgress = [control, &pausedOnce](const ScanProgress& progress) mutable {
        if (!pausedOnce && progress.stage == ScanStage::SourcePass && progress.stageDone > 256 * kKiB) {
            pausedOnce = true;
            control.pause();
        }
    };
    ScanCoordinator coordinator(source, test::allFormats(), test::allMedia(), {}, options);
    Result<ScanSummary> summary = makeError(ErrorCode::InternalError, "not run");
    std::thread scanning([&] { summary = coordinator.run(collector.sink()); });

    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!coordinator.progress().paused && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(coordinator.progress().paused);
    // Reads already under way end; after that nothing is read.
    std::this_thread::sleep_for(50ms);
    const std::size_t reads = source.stats().reads;
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(source.stats().reads, reads);
    EXPECT_TRUE(coordinator.progress().paused);
    control.resume();
    scanning.join();
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, ScanOutcome::Completed);
    EXPECT_FALSE(coordinator.progress().paused);
    expectSameAs(expectedDeep(), collector.candidates);
}

TEST(ScanCoordinatorTest, CancellingAPausedScanEndsIt) {
    ::recovery::test::MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    Collector collector;
    ScanRunOptions options = test::testRunOptions(2);
    JobControl control = options.control;
    control.pause();  // paused before it starts: it waits at its first safe point
    ScanCoordinator coordinator(source, test::allFormats(), test::allMedia(), {}, options);
    Result<ScanSummary> summary = makeError(ErrorCode::InternalError, "not run");
    std::thread scanning([&] { summary = coordinator.run(collector.sink()); });
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!coordinator.progress().paused && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(coordinator.progress().paused);
    control.requestCancellation();
    scanning.join();
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, ScanOutcome::Cancelled);
    EXPECT_FALSE(collector.checkpoint.completed());
    // And it resumes from there.
    EXPECT_EQ(runToTheEnd(source, collector, {}, test::testRunOptions()), 1U);
    expectSameAs(expectedDeep(), collector.candidates);
}

// ===========================================================================
// Checkpoints that do not fit
// ===========================================================================

TEST(ScanCoordinatorTest, AScanResumesOnlyFromACheckpointOfTheSameScan) {
    ::recovery::test::MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    Collector collector;
    ScanRunOptions options = test::testRunOptions();
    CancelAt cancel(ScanStage::SourcePass, 1);
    cancel.attach(options);
    RECOVERY_ASSERT_OK(runScan(source, collector, {}, options));
    ASSERT_FALSE(collector.checkpoint.completed());

    Collector other;
    ScanConfiguration changed;
    changed.alignment = 512;
    RECOVERY_EXPECT_ERROR(runScan(source, other, changed, test::testRunOptions(), &collector.checkpoint),
                          ErrorCode::InvalidInput);
    Bytes bigger = card();
    bigger.resize(bigger.size() + 512 * 8);
    ::recovery::test::MemoryStorageSource biggerSource(bigger);
    RECOVERY_ASSERT_OK(biggerSource.open());
    RECOVERY_EXPECT_ERROR(runScan(biggerSource, other, {}, test::testRunOptions(), &collector.checkpoint),
                          ErrorCode::InvalidInput);
    EXPECT_EQ(other.updates, 0U);
    // Another worker count is the same scan.
    EXPECT_EQ(runToTheEnd(source, collector, {}, test::testRunOptions(1)), 1U);
    expectSameAs(expectedDeep(), collector.candidates);
}

TEST(ScanCoordinatorTest, ACheckpointRefusesUpdatesThatDoNotFollow) {
    ::recovery::test::MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    std::vector<ScanUpdate> updates;
    ScanCoordinator coordinator(source, test::allFormats(), test::allMedia(), {}, test::testRunOptions());
    RECOVERY_ASSERT_OK(coordinator.run([&](const ScanUpdate& update) {
        updates.push_back(update);
        return success();
    }));
    ASSERT_GE(updates.size(), 10U);

    ScanCheckpoint checkpoint;
    // The first update names the scan; one cannot start elsewhere.
    RECOVERY_EXPECT_ERROR(checkpoint.apply(updates[1]), ErrorCode::InvalidInput);
    RECOVERY_ASSERT_OK(checkpoint.apply(updates[0]));
    // The same update twice, or one skipped.
    RECOVERY_EXPECT_ERROR(checkpoint.apply(updates[0]), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(checkpoint.apply(updates[2]), ErrorCode::InvalidInput);
    // An update of another stage, or with parts that are not its stage's.
    ScanUpdate wrong = updates[1];
    wrong.stage = ScanStage::Evaluation;
    RECOVERY_EXPECT_ERROR(checkpoint.apply(wrong), ErrorCode::InvalidInput);
    wrong = updates[1];
    wrong.carves.emplace_back();
    RECOVERY_EXPECT_ERROR(checkpoint.apply(wrong), ErrorCode::InvalidInput);
    // A refused update changes nothing.
    EXPECT_EQ(checkpoint.sequence(), 1U);
    for (std::size_t i = 1; i < updates.size(); ++i) {
        RECOVERY_ASSERT_OK(checkpoint.apply(updates[i]));
    }
    EXPECT_TRUE(checkpoint.completed());
    EXPECT_EQ(checkpoint.evaluated().size(), expectedDeep().size());
    // Nothing follows a completed scan.
    ScanUpdate after = updates.back();
    after.sequence = checkpoint.sequence() + 1;
    RECOVERY_EXPECT_ERROR(checkpoint.apply(after), ErrorCode::InvalidInput);
}

TEST(ScanCoordinatorTest, APassUpdateMustContinueThePass) {
    ::recovery::test::MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    std::vector<ScanUpdate> updates;
    ScanCoordinator coordinator(source, test::allFormats(), test::allMedia(), {}, test::testRunOptions());
    RECOVERY_ASSERT_OK(coordinator.run([&](const ScanUpdate& update) {
        updates.push_back(update);
        return success();
    }));
    ScanCheckpoint checkpoint;
    std::size_t i = 0;
    for (; i < updates.size() && !(updates[i].stage == ScanStage::SourcePass && !updates[i].carves.empty()); ++i) {
        RECOVERY_ASSERT_OK(checkpoint.apply(updates[i]));
    }
    ASSERT_LT(i, updates.size());
    ScanUpdate update = updates[i];
    ScanUpdate gap = update;
    gap.carves.front().id = carving::FileCandidateId{gap.carves.front().id.value() + 1};
    RECOVERY_EXPECT_ERROR(checkpoint.apply(gap), ErrorCode::InvalidInput);
    ScanUpdate beyond = update;
    beyond.pass->position = card().size() + 1;
    RECOVERY_EXPECT_ERROR(checkpoint.apply(beyond), ErrorCode::InvalidInput);
    RECOVERY_ASSERT_OK(checkpoint.apply(update));
    ScanUpdate back = updates[i + 1];
    if (back.stage == ScanStage::SourcePass && back.pass->position > 0) {
        back.pass->position = 0;
        RECOVERY_EXPECT_ERROR(checkpoint.apply(back), ErrorCode::InvalidInput);
    }
}

// ===========================================================================
// Unreadable sectors
// ===========================================================================

TEST(ScanCoordinatorTest, UnreadableSectorsAreCountedOnceAndKeptAcrossResumes) {
    ::recovery::test::MemoryStorageSource source(card());
    // Free clusters in the middle of the card, away from the files' data.
    const std::uint64_t first = card().size() / 512 * 5 / 8;
    for (std::uint64_t sector = first; sector < first + 6; ++sector) {
        source.addBadSector(sector);
    }
    RECOVERY_ASSERT_OK(source.open());
    Collector collector;
    ScanRunOptions options = test::testRunOptions();
    CancelAt cancel(ScanStage::Fragments, 0);
    cancel.attach(options);
    RECOVERY_ASSERT_OK(runScan(source, collector, {}, options));
    ASSERT_FALSE(collector.checkpoint.completed());
    EXPECT_EQ(collector.checkpoint.unreadable().totalBytes(), 6U * 512);
    EXPECT_EQ(runToTheEnd(source, collector, {}, test::testRunOptions()), 1U);
    EXPECT_EQ(collector.checkpoint.unreadable().totalBytes(), 6U * 512);
    EXPECT_EQ(collector.checkpoint.metrics().unreadableBytes, 6U * 512);
    ASSERT_TRUE(collector.checkpoint.pass().has_value());
    EXPECT_EQ(collector.checkpoint.pass()->scan.unreadableBytes, 6U * 512);
}

}  // namespace
}  // namespace recovery::scan
