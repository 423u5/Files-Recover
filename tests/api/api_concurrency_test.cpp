// Concurrent queries (P19): a user interface asks for progress, candidates,
// the session and the session list from many threads while a scan or a
// recovery runs; every answer is consistent and none goes back. Two
// sessions run at once.

#include "api_test_support.hpp"

#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace recovery::api::test {
namespace {

constexpr int kQueryThreads = 8;

// Asks everything a user interface asks, from several threads, until `done`.
// Returns the number of answers.
std::uint64_t hammer(RecoveryApi& api, const std::string& session, std::atomic<bool>& done) {
    std::atomic<std::uint64_t> answers{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kQueryThreads; ++t) {
        threads.emplace_back([&, t] {
            double scanFraction = 0.0;
            double recoveryFraction = 0.0;
            std::uint64_t candidates = 0;
            while (!done.load()) {
                switch ((answers.load() + static_cast<std::uint64_t>(t)) % 5) {
                case 0: {
                    const Result<Progress> progress = api.getProgress(session);
                    ASSERT_TRUE(progress.ok()) << describe(progress.error());
                    if (progress->operation == OperationKind::Scan && progress->scan.has_value() &&
                        progress->state != OperationState::Idle) {
                        EXPECT_GE(progress->scan->fraction, scanFraction) << "the scan's progress went back";
                        scanFraction = progress->scan->fraction;
                    }
                    if (progress->operation == OperationKind::Recovery && progress->recovery.has_value()) {
                        EXPECT_GE(progress->recovery->fraction, recoveryFraction);
                        recoveryFraction = progress->recovery->fraction;
                        EXPECT_LE(progress->recovery->done, progress->recovery->total);
                    }
                    break;
                }
                case 1: {
                    const Result<CandidatePage> page = api.getCandidates(session);
                    ASSERT_TRUE(page.ok()) << describe(page.error());
                    EXPECT_GE(page->total, candidates) << "candidates went missing";
                    candidates = page->total;
                    EXPECT_EQ(page->candidates.size(), page->total);
                    for (std::size_t i = 0; i < page->candidates.size(); ++i) {
                        EXPECT_EQ(page->candidates[i].id.value, i + 1);
                    }
                    break;
                }
                case 2: {
                    const Result<SessionDetails> details = api.getSession(session);
                    ASSERT_TRUE(details.ok()) << describe(details.error());
                    EXPECT_EQ(details->id, session);
                    break;
                }
                case 3: {
                    const Result<std::vector<SessionListing>> listed = api.listSessions();
                    ASSERT_TRUE(listed.ok()) << describe(listed.error());
                    EXPECT_FALSE(listed->empty());
                    break;
                }
                default: {
                    if (candidates != 0) {
                        const Result<CandidateDetails> details =
                            api.getCandidateDetails(session, CandidateId{1}, false);
                        ASSERT_TRUE(details.ok()) << describe(details.error());
                    }
                    break;
                }
                }
                ++answers;
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    return answers.load();
}

TEST(ApiConcurrencyTest, QueriesFromManyThreadsWhileAScanAndARecoveryRun) {
    ApiWorld world;
    const std::string id = world.startCardScan();
    std::atomic<bool> done{false};
    std::thread waiter([&] {
        (void)world.api().waitForOperation(id, kWait);
        RecoveryOptions options;
        options.destination = world.folder() / "out";
        EXPECT_TRUE(world.api().recoverAll(id, options).ok());
        (void)world.api().waitForOperation(id, kWait);
        done = true;
    });
    const std::uint64_t answers = hammer(world.api(), id, done);
    waiter.join();
    EXPECT_GT(answers, 100u);
    const Result<Progress> progress = world.api().getProgress(id);
    RECOVERY_ASSERT_OK(progress);
    EXPECT_EQ(progress->operation, OperationKind::Recovery);
    EXPECT_EQ(progress->state, OperationState::Completed);
    EXPECT_EQ(::recovery::test::filesBelow(world.folder() / "out").size(), expectedCard().size());
}

TEST(ApiConcurrencyTest, TwoSessionsRunAtOnce) {
    ApiWorld world;
    const std::uint32_t disk = world.addDisk(::recovery::test::threeVolumeDisk());
    const std::string card = world.startCardScan();
    const Result<std::string> volumes = world.api().startScan(SourceRef::physicalDisk(disk));
    RECOVERY_ASSERT_OK(volumes);
    EXPECT_NE(*volumes, card);
    EXPECT_EQ(world.waitIdle(card).state, OperationState::Completed);
    EXPECT_EQ(world.waitIdle(*volumes).state, OperationState::Completed);
    EXPECT_EQ(allCandidates(world.api(), card).size(), expectedCard().size());
    const std::vector<CandidateInfo> found = allCandidates(world.api(), *volumes);
    std::set<std::string> names;
    for (const CandidateInfo& candidate : found) {
        names.insert(candidate.name);
    }
    EXPECT_TRUE(names.contains("A.JPG"));
    EXPECT_TRUE(names.contains("B.PNG"));
    EXPECT_TRUE(names.contains("C.WAV"));
    const Result<SessionDetails> details = world.api().getSession(*volumes);
    RECOVERY_ASSERT_OK(details);
    EXPECT_EQ(details->partitionScheme, PartitionScheme::Mbr);
    ASSERT_EQ(details->volumes.size(), 3u);
    EXPECT_EQ(details->volumes[0].filesystem, FilesystemKind::Fat32);
    EXPECT_EQ(details->volumes[1].filesystem, FilesystemKind::ExFat);
    EXPECT_EQ(details->volumes[2].filesystem, FilesystemKind::Ntfs);
    EXPECT_EQ(details->source.kind, SourceKind::PhysicalDisk);
    EXPECT_EQ(details->source.disk, disk);
}

}  // namespace
}  // namespace recovery::api::test
