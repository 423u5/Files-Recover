// The API's events (P19): delivered on one thread of the API's, one at a
// time and in order; the callback may call the API; a slow user interface
// gets merged progress and never slows an operation; a full queue drops
// files and says how many; logs; and an API without a callback.

#include "api_test_support.hpp"

#include "scan/scan_test_support.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <latch>
#include <set>
#include <thread>

namespace recovery::api::test {
namespace {

std::size_t countOf(const std::vector<Event>& events, EventKind kind) {
    return static_cast<std::size_t>(
        std::count_if(events.begin(), events.end(), [kind](const Event& event) { return event.kind == kind; }));
}

// What a hook captures is declared before the world: the API's threads may
// call the hook until the world is gone.

TEST(ApiEventsTest, EventsArriveOneAtATimeOnOneThreadOfTheApis) {
    std::mutex mutex;
    std::set<std::thread::id> threads;
    std::atomic<int> inside{0};
    std::atomic<int> most{0};
    ApiWorld world;
    world.log().setHook([&](const Event&) {
        const int now = ++inside;
        most = std::max(most.load(), now);
        {
            const std::lock_guard lock(mutex);
            threads.insert(std::this_thread::get_id());
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        --inside;
    });
    const std::string id = world.startCardScan();
    ASSERT_TRUE(world.log().waitForFinished(id).has_value());
    EXPECT_EQ(most.load(), 1) << "two events were delivered at once";
    const std::lock_guard lock(mutex);
    EXPECT_EQ(threads.size(), 1u);
    EXPECT_FALSE(threads.contains(std::this_thread::get_id()));
}

TEST(ApiEventsTest, TheCallbackMayCallTheApi) {
    RecoveryApi* api = nullptr;
    std::atomic<std::uint64_t> listed{0};
    std::atomic<bool> recovered{false};
    ApiWorld world;
    world.log().setHook([&](const Event& event) {
        if (event.kind == EventKind::CandidatesFound) {
            const Result<CandidatePage> page = api->getCandidates(event.session);
            listed = page.ok() ? page->total : 0;
        }
        // The user interface starts the recovery from the scan's last event.
        if (event.kind == EventKind::OperationFinished && event.progress.operation == OperationKind::Scan) {
            RecoveryOptions options;
            options.destination = world.folder() / "out";
            recovered = api->recoverAll(event.session, options).ok();
        }
    });
    api = &world.api();
    const std::string id = world.startCardScan();
    const std::optional<Event> recovery = world.log().waitForFinished(id, 2);
    ASSERT_TRUE(recovery.has_value());
    EXPECT_TRUE(recovered.load());
    EXPECT_EQ(recovery->progress.operation, OperationKind::Recovery);
    EXPECT_EQ(recovery->progress.state, OperationState::Completed);
    EXPECT_EQ(listed.load(), expectedCard().size());
}

TEST(ApiEventsTest, ASlowUserInterfaceGetsMergedEventsAndNeverSlowsTheScan) {
    std::latch release(1);
    std::atomic<bool> blocked{false};
    ApiWorld world;
    world.log().setHook([&](const Event& event) {
        if (event.kind == EventKind::OperationStarted && !blocked.exchange(true)) {
            release.wait();
        }
    });
    const std::string id = world.startCardScan();
    // The scan completes while the user interface sits on its first event.
    const Result<Progress> done = world.api().waitForOperation(id, kWait);
    const bool nothingDelivered = world.log().events().empty();
    release.count_down();
    RECOVERY_ASSERT_OK(done);
    EXPECT_EQ(done->state, OperationState::Completed);
    EXPECT_TRUE(nothingDelivered);
    ASSERT_TRUE(world.log().waitForFinished(id).has_value());

    const std::vector<Event> events = world.log().eventsOf(id);
    EXPECT_EQ(events.front().kind, EventKind::OperationStarted);
    EXPECT_EQ(events.back().kind, EventKind::OperationFinished);
    EXPECT_LE(countOf(events, EventKind::Progress), 1u) << "queued progress is merged";
    ASSERT_EQ(countOf(events, EventKind::CandidatesFound), 1u) << "queued candidates are merged";
    const auto found = std::find_if(events.begin(), events.end(),
                                    [](const Event& e) { return e.kind == EventKind::CandidatesFound; });
    EXPECT_EQ(found->firstCandidate.value, 1u);
    EXPECT_EQ(found->candidateCount, expectedCard().size());
}

TEST(ApiEventsTest, AFullQueueDropsFilesAndSaysHowMany) {
    std::latch release(1);
    std::atomic<bool> blocked{false};
    ApiWorld world;
    world.hooks().maxQueuedFiles = 1;
    const std::string id = world.startCardScan();
    ASSERT_TRUE(world.log().waitForFinished(id).has_value());

    world.log().setHook([&](const Event& event) {
        if (event.kind == EventKind::OperationStarted && event.progress.operation == OperationKind::Recovery &&
            !blocked.exchange(true)) {
            release.wait();
        }
    });
    RecoveryOptions options;
    options.destination = world.folder() / "out";
    const Result<RecoveryStart> start = world.api().recoverAll(id, options);
    const OperationState ended = start.ok() ? world.waitIdle(id).state : OperationState::Failed;
    release.count_down();
    RECOVERY_ASSERT_OK(start);
    ASSERT_EQ(ended, OperationState::Completed);
    ASSERT_TRUE(world.log().waitForFinished(id, 2).has_value());

    std::uint64_t told = 0;
    std::uint64_t dropped = 0;
    bool afterStart = false;
    for (const Event& event : world.log().eventsOf(id)) {
        afterStart = afterStart || (event.kind == EventKind::OperationStarted &&
                                    event.progress.operation == OperationKind::Recovery);
        if (!afterStart) {
            continue;
        }
        told += event.files.size();
        dropped += event.filesDropped;
    }
    EXPECT_GT(dropped, 0u);
    EXPECT_EQ(told + dropped, expectedCard().size()) << "every file is told or counted as dropped";
    // What the events left out, the list has.
    for (const CandidateInfo& candidate : allCandidates(world.api(), id)) {
        EXPECT_NE(candidate.recovery, RecoveryState::NotRecovered) << candidate.name;
    }
}

TEST(ApiEventsTest, AnExceptionFromTheCallbackStopsNothing) {
    std::atomic<int> thrown{0};
    ApiWorld world;
    world.log().setHook([&](const Event&) {
        ++thrown;
        throw std::runtime_error("the user interface failed");
    });
    const std::string id = world.startCardScan();
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Completed);
    EXPECT_GT(thrown.load(), 1);
}

TEST(ApiEventsTest, AnApiWithoutACallbackWorksAllTheSame) {
    ::recovery::test::TempDir folder;
    ::recovery::test::writeFile(folder / "card.img", scan::test::makeCard());
    ApiOptions options;
    options.sessionsRoot = folder / "sessions";
    PlatformHooks hooks;
    hooks.diskResolver = [](const std::filesystem::path&) {
        return Result<std::vector<std::uint32_t>>(std::vector<std::uint32_t>{0});
    };
    Result<std::unique_ptr<RecoveryApi>> api = createRecoveryApi(options, hooks);
    RECOVERY_ASSERT_OK(api);
    const Result<std::string> id = (*api)->startScan(SourceRef::imageFile(folder / "card.img"));
    RECOVERY_ASSERT_OK(id);
    const Result<Progress> done = (*api)->waitForOperation(*id, kWait);
    RECOVERY_ASSERT_OK(done);
    EXPECT_EQ(done->state, OperationState::Completed);
    EXPECT_EQ(done->scan->metrics.candidates, expectedCard().size());
}

TEST(ApiEventsTest, TheEnginesLogReachesTheLogCallback) {
    std::mutex mutex;
    std::vector<LogEntry> entries;
    ApiWorld world;
    world.options().onLog = [&](const LogEntry& entry) {
        const std::lock_guard lock(mutex);
        entries.push_back(entry);
    };
    world.options().logLevel = LogLevel::Info;
    const std::string id = world.startCardScan();
    ASSERT_EQ(world.waitIdle(id).state, OperationState::Completed);
    const std::lock_guard lock(mutex);
    ASSERT_FALSE(entries.empty());
    EXPECT_TRUE(std::any_of(entries.begin(), entries.end(),
                            [](const LogEntry& entry) { return entry.message == "scan started"; }));
    for (const LogEntry& entry : entries) {
        EXPECT_GE(entry.level, LogLevel::Info);
        EXPECT_FALSE(entry.line.empty());
        EXPECT_EQ(entry.line.find('\n'), std::string::npos);
    }
}

}  // namespace
}  // namespace recovery::api::test
