// Sessions through the API (P19): restored after the program closes, after
// a crash in the middle of a scan, open in one program at a time, ids
// checked; the session list; and reports.

#include "api_test_support.hpp"

#include "support/json_reader.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <sstream>

namespace recovery::api::test {
namespace {

// Copies the files of `from` to `to` as they are now, sharing them with the
// program that has them open (a crash leaves what was written).
void snapshotFolder(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::create_directories(to);
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(from)) {
        std::ifstream in(entry.path(), std::ios::binary);
        std::ofstream out(to / entry.path().filename(), std::ios::binary);
        out << in.rdbuf();
    }
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

TEST(ApiSessionTest, ASessionIsRestoredAfterTheProgramCloses) {
    ApiWorld world;
    const std::string id = world.startCardScan();
    ASSERT_EQ(world.waitIdle(id).state, OperationState::Completed);
    RecoveryOptions options;
    options.destination = world.folder() / "out";
    RECOVERY_ASSERT_OK(world.api().recoverCandidate(id, candidateNamed(world.api(), id, "PHOTO.JPG").id, options));
    ASSERT_EQ(world.waitIdle(id).state, OperationState::Completed);
    const std::vector<CandidateInfo> before = allCandidates(world.api(), id);
    world.restart();

    const Result<std::vector<SessionListing>> listed = world.api().listSessions();
    RECOVERY_ASSERT_OK(listed);
    ASSERT_EQ(listed->size(), 1u);
    const SessionListing& listing = listed->front();
    EXPECT_EQ(listing.id, id);
    EXPECT_FALSE(listing.error.has_value());
    EXPECT_FALSE(listing.open);
    EXPECT_EQ(listing.scanState, RunState::Completed);
    EXPECT_EQ(listing.candidates, expectedCard().size());
    EXPECT_EQ(listing.jobs, 1u);
    EXPECT_EQ(listing.sourceKind, SourceKind::DiskImage);
    EXPECT_EQ(listing.mode, ScanMode::Deep);

    const Result<SessionDetails> details = world.api().openSession(id);
    RECOVERY_ASSERT_OK(details);
    EXPECT_EQ(details->id, id);
    EXPECT_EQ(details->scan.state, RunState::Completed);
    EXPECT_EQ(details->scan.candidates, expectedCard().size());
    ASSERT_EQ(details->jobs.size(), 1u);
    EXPECT_EQ(details->jobs[0].state, RunState::Completed);
    EXPECT_EQ(details->jobs[0].recovered, 1u);
    EXPECT_TRUE(details->repairs.empty());
    // Opening it again changes nothing.
    RECOVERY_ASSERT_OK(world.api().openSession(id));

    const std::vector<CandidateInfo> after = allCandidates(world.api(), id);
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < after.size(); ++i) {
        EXPECT_EQ(after[i].name, before[i].name);
        EXPECT_EQ(after[i].condition, before[i].condition);
        EXPECT_EQ(after[i].sha256, before[i].sha256);
        EXPECT_EQ(after[i].recovery, before[i].recovery);
        EXPECT_EQ(after[i].recoveredFile, before[i].recoveredFile);
    }
    const Result<SessionListing> listedOpen = [&]() -> Result<SessionListing> {
        Result<std::vector<SessionListing>> again = world.api().listSessions();
        if (!again.ok()) {
            return again.error();
        }
        return again->front();
    }();
    RECOVERY_ASSERT_OK(listedOpen);
    EXPECT_TRUE(listedOpen->open);
}

TEST(ApiSessionTest, AScanInterruptedByACrashResumesWhereItStopped) {
    std::filesystem::path crashed;
    std::atomic<bool> taken{false};
    ApiWorld world;
    crashed = world.folder() / "crashed";
    world.hooks().onOperationProgress = [&](std::string_view session, const Progress& progress) {
        if (progress.scan.has_value() && progress.scan->stage == ScanStage::SourcePass && !taken.exchange(true)) {
            // What a crash at this point leaves on disk.
            snapshotFolder(world.sessions() / std::string(session), crashed / std::string(session));
        }
    };
    const std::string id = world.startCardScan();
    ASSERT_EQ(world.waitIdle(id).state, OperationState::Completed);
    ASSERT_TRUE(taken.load());
    world.restart();

    // The program starts again on the folder the crash left.
    world.options().sessionsRoot = crashed;
    world.hooks().onOperationProgress = nullptr;
    const Result<std::vector<SessionListing>> listed = world.api().listSessions();
    RECOVERY_ASSERT_OK(listed);
    ASSERT_EQ(listed->size(), 1u);
    EXPECT_EQ(listed->front().scanState, RunState::Running) << "a listing cannot tell a crash from a run";
    const Result<SessionDetails> details = world.api().openSession(id);
    RECOVERY_ASSERT_OK(details);
    EXPECT_EQ(details->scan.state, RunState::Interrupted);
    EXPECT_TRUE(details->scan.resumable);
    EXPECT_NE(details->scan.stage, ScanStage::Completed);
    RECOVERY_ASSERT_OK(world.api().resumeScan(id));
    EXPECT_EQ(world.waitIdle(id).state, OperationState::Completed);
    world.restart();
    EXPECT_EQ(journalCandidates(crashed, id), expectedCard());
}

TEST(ApiSessionTest, ASessionIsOpenInOneProgramAtATime) {
    ApiWorld first;
    const std::string id = first.startCardScan();
    ASSERT_EQ(first.waitIdle(id).state, OperationState::Completed);

    ApiWorld second;
    second.options().sessionsRoot = first.sessions();
    const Result<SessionDetails> refused = second.api().openSession(id);
    ASSERT_FALSE(refused.ok());
    EXPECT_NE(refused.error().message.find(id), std::string::npos);

    RECOVERY_ASSERT_OK(first.api().closeSession(id));
    RECOVERY_EXPECT_ERROR(first.api().getSession(id), ErrorCode::InvalidInput);
    const Result<SessionDetails> opened = second.api().openSession(id);
    RECOVERY_ASSERT_OK(opened);
    EXPECT_EQ(opened->scan.state, RunState::Completed);
}

TEST(ApiSessionTest, SessionIdsAreChecked) {
    ApiWorld world;
    RECOVERY_EXPECT_ERROR(world.api().openSession("../outside"), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world.api().openSession(""), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world.api().openSession("C:\\Windows"), ErrorCode::InvalidInput);
    EXPECT_FALSE(world.api().openSession("20260101-000000-00000000").ok());
    RECOVERY_EXPECT_ERROR(world.api().getSession("20260101-000000-00000000"), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world.api().closeSession("20260101-000000-00000000"), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world.api().getProgress("no such"), ErrorCode::InvalidInput);
    const Result<std::vector<SessionListing>> none = world.api().listSessions();
    RECOVERY_ASSERT_OK(none);
    EXPECT_TRUE(none->empty());
}

TEST(ApiSessionTest, ReportsAreWrittenAsJsonAndText) {
    ApiWorld world;
    const std::string id = world.startCardScan();
    ASSERT_EQ(world.waitIdle(id).state, OperationState::Completed);

    const std::filesystem::path json = world.folder() / "report.json";
    RECOVERY_ASSERT_OK(world.api().exportReport(id, json));
    std::string error;
    const std::optional<::recovery::test::json::Value> document =
        ::recovery::test::json::parse(readText(json), &error);
    ASSERT_TRUE(document.has_value()) << error;
    EXPECT_EQ((*document)["format"].string(), "recovery-session-report");
    EXPECT_EQ((*document)["session"]["id"].string(), id);
    EXPECT_EQ((*document)["candidates"].size(), expectedCard().size());

    const std::filesystem::path text = world.folder() / "report.txt";
    ReportOptions options;
    options.format = ReportFormat::Text;
    options.details = true;
    RECOVERY_ASSERT_OK(world.api().exportReport(id, text, options));
    const std::string report = readText(text);
    EXPECT_NE(report.find("Session " + id), std::string::npos);
    EXPECT_NE(report.find("PHOTO.JPG"), std::string::npos);

    // An existing file is never overwritten; the source never written.
    RECOVERY_EXPECT_ERROR(world.api().exportReport(id, json), ErrorCode::DestinationError);
    EXPECT_FALSE(world.api().exportReport(id, world.cardImage()).ok());
    RECOVERY_EXPECT_ERROR(world.api().exportReport(id, ""), ErrorCode::InvalidInput);
}

TEST(ApiSessionTest, AReportOfAPhysicalDiskIsNeverWrittenOnIt) {
    ApiWorld world;
    const std::uint32_t disk = world.addDisk(::recovery::test::readFile(world.cardImage()));
    // A folder of a volume on the disk scanned.
    std::filesystem::create_directories(world.folder() / "on-the-disk");
    world.placeOn(world.folder() / "on-the-disk", disk);
    const Result<std::string> id = world.api().startScan(SourceRef::physicalDisk(disk));
    RECOVERY_ASSERT_OK(id);
    ASSERT_EQ(world.waitIdle(*id).state, OperationState::Completed);
    RECOVERY_EXPECT_ERROR(world.api().exportReport(*id, world.folder() / "on-the-disk" / "report.json"),
                          ErrorCode::DestinationError);
    EXPECT_FALSE(std::filesystem::exists(world.folder() / "on-the-disk" / "report.json"));
    RECOVERY_EXPECT_OK(world.api().exportReport(*id, world.folder() / "elsewhere.json"));
}

TEST(ApiSessionTest, ASessionCannotBeKeptOnTheDiskItScans) {
    ApiWorld world;
    const std::uint32_t disk = world.addDisk(::recovery::test::readFile(world.cardImage()));
    world.placeOn(world.sessions(), disk);
    const Result<std::string> id = world.api().startScan(SourceRef::physicalDisk(disk));
    RECOVERY_EXPECT_ERROR(id, ErrorCode::DestinationError);
}

}  // namespace
}  // namespace recovery::api::test
