// The CLI and physical disks (L147, L177): `disks` lists them as --source
// takes them; a session's disk found again under another number (Windows
// numbers disks as they are attached) resumes its scan and recovers there,
// and says so; a disk with the session's content in another device is not
// the session's; reports and logs written without opening the disk stay off
// every disk that may be it.
//
// The disks are simulated (support/disk_rack.hpp): no real device is opened.

#include "cli_test_support.hpp"
#include "report/text_format.hpp"
#include "scan/scan_test_support.hpp"
#include "support/disk_rack.hpp"
#include "support/test_files.hpp"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <mutex>

namespace recovery::cli {
namespace {

using test::arg;
using test::Cli;
using test::CliResult;
using ::recovery::test::DiskRack;
using ::recovery::test::FakeDeviceConfig;

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

const test::Bytes& otherCard() {
    static const test::Bytes bytes = scan::test::makeCard(4096, 0xBEEF);
    return bytes;
}

class CliDiskTest : public ::testing::Test {
protected:
    CliDiskTest() {
        (void)rack->add(7, test::card(), {}, "SER-A");
        cli.environment().diskOpener = rack;
        cli.environment().diskLister = rack->lister();
        // A folder put on a disk with placeOn(), else disk 0.
        cli.environment().diskResolver = [places = places](const std::filesystem::path& path)
            -> Result<std::vector<std::uint32_t>> {
            const std::lock_guard lock(places->mutex);
            for (const auto& [folder, disk] : places->disks) {
                const std::filesystem::path relative = path.lexically_normal().lexically_relative(folder);
                if (!relative.empty() && *relative.begin() != "..") {
                    return std::vector<std::uint32_t>{disk};
                }
            }
            return std::vector<std::uint32_t>{0};
        };
    }

    // Paths below `folder` (created) are on disk `disk`.
    void placeOn(const std::filesystem::path& folder, std::uint32_t disk) {
        std::filesystem::create_directories(folder);
        const std::lock_guard lock(places->mutex);
        places->disks[folder.lexically_normal()] = disk;
    }

    // A deep scan of disk `number`; Ctrl+C in the source pass when `stop`.
    std::string scanDisk(std::uint32_t number, bool stop) {
        bool asked = false;
        const CliResult result =
            cli.run({"scan", "--source", "\\\\.\\PhysicalDrive" + std::to_string(number)},
                    [&](const ProgressEvent& event) {
                        if (stop && !asked && event.scan.has_value() &&
                            event.scan->stage == scan::ScanStage::SourcePass) {
                            asked = true;
                            (void)cli.interrupt().request();
                        }
                    });
        EXPECT_EQ(result.code, stop ? ExitCode::Cancelled : ExitCode::Success) << result;
        return test::sessionIdOf(result.out).value_or(std::string());
    }

    struct Places {
        std::mutex mutex;
        std::map<std::filesystem::path, std::uint32_t> disks;
    };

    ::recovery::test::TempDir dir;
    std::filesystem::path sessions = dir.path() / "sessions";
    Cli cli{sessions};
    std::shared_ptr<DiskRack> rack = std::make_shared<DiskRack>();
    std::shared_ptr<Places> places = std::make_shared<Places>();
};

// L177.
TEST_F(CliDiskTest, DisksAreListedAsTheSourceTakesThem) {
    (void)rack->add(8, otherCard(), {}, "SER-D");
    auto empty = std::make_shared<FakeDeviceConfig>();
    empty->description.vendor = "Simulated";
    empty->description.product = "Empty Reader";
    rack->attach(9, empty);
    placeOn(sessions, 8);

    const CliResult listed = cli.run({"disks"});
    ASSERT_EQ(listed.code, ExitCode::Success) << listed;
    const std::vector<std::string> lines = [&] {
        std::vector<std::string> split;
        std::size_t start = 0;
        for (std::size_t end = listed.out.find('\n'); end != std::string::npos; end = listed.out.find('\n', start)) {
            split.push_back(listed.out.substr(start, end - start));
            start = end + 1;
        }
        return split;
    }();
    ASSERT_EQ(lines.size(), 4U) << listed;
    EXPECT_TRUE(lines[0].starts_with("Disk ")) << lines[0];
    EXPECT_TRUE(contains(lines[0], "Serial number")) << lines[0];
    const auto row = [&](std::size_t index, std::vector<std::string> parts) {
        for (const std::string& part : parts) {
            EXPECT_TRUE(contains(lines[index], part)) << lines[index] << " lacks " << part;
        }
    };
    row(1, {"\\\\.\\PhysicalDrive7", report::formatSize(test::card().size()), "USB", "Simulated Card Reader",
            "SER-A", "removable"});
    row(2, {"\\\\.\\PhysicalDrive8", "SER-D", "removable, holds the sessions folder"});
    row(3, {"\\\\.\\PhysicalDrive9", "no medium", "Simulated Empty Reader"});
    EXPECT_TRUE(contains(listed.err, "Give a disk to --source as it is listed, or as a drive letter on it (the whole "
                                     "disk is read). Reading a disk needs administrator rights; listing them does "
                                     "not."))
        << listed;

    cli.environment().diskLister = []() -> Result<std::vector<storage::AttachedDisk>> {
        return makeError(ErrorCode::IoError, "the system cannot be asked");
    };
    const CliResult failed = cli.run({"disks"});
    EXPECT_EQ(failed.code, ExitCode::Error) << failed;
    EXPECT_TRUE(contains(failed.err, "cannot list the disks: IO_ERROR: the system cannot be asked")) << failed;
}

// L147: the disk plugged in again under another number (another card took
// its number) is found, said, scanned to the end and recovered from.
TEST_F(CliDiskTest, AScanResumesOnTheSessionsDiskUnderItsNewNumber) {
    const std::string id = scanDisk(7, true);
    ASSERT_FALSE(id.empty());
    rack->renumber(7, 9);
    (void)rack->add(7, otherCard(), {}, "SER-C");

    const CliResult resumed = cli.run({"scan", "--session", id});
    ASSERT_EQ(resumed.code, ExitCode::Success) << resumed;
    EXPECT_TRUE(contains(resumed.err, "The session's disk is \\\\.\\PhysicalDrive9 now (\\\\.\\PhysicalDrive7 when "
                                      "last used): the same device (serial SER-A), unchanged."))
        << resumed;
    EXPECT_EQ(test::sessionCandidates(sessions, id), test::expectedCard(ScanMode::Deep));

    const CliResult recovered = cli.run({"recover", "--session", id, "--output", arg(dir.path() / "out")});
    ASSERT_EQ(recovered.code, ExitCode::Success) << recovered;
    // Under its number now: nothing more to say.
    EXPECT_FALSE(contains(recovered.err, "when last used")) << recovered;
    EXPECT_FALSE(test::filesBelow(dir.path() / "out").empty());
    const CliResult reported = cli.run({"report", "--session", id});
    ASSERT_EQ(reported.code, ExitCode::Success) << reported;
    EXPECT_TRUE(contains(reported.out, "physical disk 9")) << reported;
}

// L147, the user's choice: the session's content in another device (the
// card in another reader, a copy) is not the session's disk.
TEST_F(CliDiskTest, TheSameContentInAnotherDeviceIsNotTheSessionsDisk) {
    const std::string id = scanDisk(7, false);
    ASSERT_FALSE(id.empty());
    (void)rack->detach(7);
    (void)rack->add(7, test::card(), {}, "SER-B");
    const CliResult refused = cli.run({"recover", "--session", id, "--output", arg(dir.path() / "out")});
    EXPECT_EQ(refused.code, ExitCode::Error) << refused;
    EXPECT_TRUE(contains(refused.err, "the session's disk, disk 7 when last used (Simulated Card Reader, serial "
                                      "SER-A, "))
        << refused;
    EXPECT_TRUE(contains(refused.err, "is not attached, or it has changed: disk 7: the disk is another device than "
                                      "the session's: its serial number is 'SER-B', the session's disk's 'SER-A'"))
        << refused;
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "out"));
}

// L147: a report or a log written without opening the disk is kept off the
// disk wherever it may be now, not off its old number alone.
TEST_F(CliDiskTest, ReportsAndLogsStayOffEveryDiskThatMayBeTheSessions) {
    const std::string id = scanDisk(7, false);
    ASSERT_FALSE(id.empty());
    // Plugged in again as disk 9 (the session does not know yet); disk 7 is
    // another card's.
    rack->renumber(7, 9);
    (void)rack->add(7, otherCard(), {}, "SER-C");
    placeOn(dir.path() / "on9", 9);
    placeOn(dir.path() / "on7", 7);

    const CliResult report = cli.run({"report", "--session", id, "--output", arg(dir.path() / "on9" / "r.txt")});
    EXPECT_EQ(report.code, ExitCode::Error) << report;
    EXPECT_TRUE(contains(report.err, "destination is on the source disk (PhysicalDrive9)")) << report;
    const CliResult log = cli.run({"report", "--session", id, "--log", arg(dir.path() / "on9" / "log.txt")});
    EXPECT_EQ(log.code, ExitCode::Error) << log;
    EXPECT_TRUE(contains(log.err, "cannot open the log file")) << log;
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "on9" / "r.txt"));
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "on9" / "log.txt"));

    // Its old number is another device's now: a report may go there.
    const CliResult elsewhere = cli.run({"report", "--session", id, "--output", arg(dir.path() / "on7" / "r.txt")});
    EXPECT_EQ(elsewhere.code, ExitCode::Success) << elsewhere;
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "on7" / "r.txt"));
}

}  // namespace
}  // namespace recovery::cli
