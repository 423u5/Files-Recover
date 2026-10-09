// Invalid paths and error reporting (P18): sources that do not exist, are
// folders or drives, cannot be opened or are in use; outputs that exist, are
// files or are on the source disk; sessions that do not exist, cannot be read
// or are in use; sources gone or changed since the scan. Every error exits
// with 2, says what failed on the error stream ("recovery <command>:
// error: ..."), and leaves no output and no session behind. Names read from
// a disk cannot put control characters on the terminal.

#include "cli/format.hpp"
#include "cli_test_support.hpp"
#include "session/recovery_session.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/test_files.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <set>

namespace recovery::cli {
namespace {

using test::arg;
using test::Bytes;
using test::Cli;
using test::CliResult;

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

class CliErrorsTest : public ::testing::Test {
protected:
    // The command failed, said so on the error stream, and printed nothing as a result.
    static void expectError(const CliResult& result, std::string_view command, std::string_view says) {
        EXPECT_EQ(result.code, ExitCode::Error) << result;
        EXPECT_NE(result.err.find("recovery " + std::string(command) + ": error: "), std::string::npos) << result;
        EXPECT_TRUE(contains(result.err, says)) << says << "\n" << result;
    }

    std::string scanned() {
        const CliResult result = cli.run({"scan", "--source", arg(image)});
        EXPECT_EQ(result.code, ExitCode::Success) << result;
        return test::sessionIdOf(result.out).value_or(std::string());
    }

    ::recovery::test::TempDir dir;
    std::filesystem::path sessions = dir.path() / "sessions";
    Cli cli{sessions};
    std::filesystem::path image = test::writeImage(dir.path() / "card.img", test::card());
};

TEST_F(CliErrorsTest, ASourceThatDoesNotExist) {
    const std::string missing = arg(dir.path() / "nope.img");
    for (const std::vector<std::string>& line :
         {std::vector<std::string>{"inspect", "--source", missing},
          std::vector<std::string>{"image", "--source", missing, "--output", arg(dir.path() / "copy.img")},
          std::vector<std::string>{"scan", "--source", missing}}) {
        const CliResult result = cli.run(line);
        expectError(result, line.front(), "the image file '" + missing + "' does not exist");
        EXPECT_TRUE(result.out.empty()) << result;
    }
    EXPECT_FALSE(std::filesystem::exists(sessions));
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "copy.img"));
}

TEST_F(CliErrorsTest, ASourceThatIsAFolderOrADrive) {
    expectError(cli.run({"inspect", "--source", arg(dir.path())}), "inspect", "is a folder, not an image file");
    // A drive: the disk it is on is named.
    expectError(cli.run({"inspect", "--source", "C:\\"}), "inspect",
                "'C:\\' is a drive, not an image file: the engine reads whole disks; use --source "
                "\\\\.\\PhysicalDrive0 for the disk it is on (as an administrator)");
    cli.environment().diskResolver = [](const std::filesystem::path&) -> Result<std::vector<std::uint32_t>> {
        return std::vector<std::uint32_t>{1, 2};
    };
    expectError(cli.run({"scan", "--source", "C:"}), "scan", "use --source \\\\.\\PhysicalDriveN for the disk");
    EXPECT_FALSE(std::filesystem::exists(sessions));
}

TEST_F(CliErrorsTest, ADiskThatCannotBeOpened) {
    const test::SimulatedDisk disk = test::simulatedDisk(test::card());
    cli.environment().diskOpener = disk.opener;
    disk.opener->failOpenWith(makeError(ErrorCode::IoError, "access denied", 5));
    expectError(cli.run({"inspect", "--source", "\\\\.\\PhysicalDrive7"}), "inspect",
                "Reading a physical disk needs administrator rights");
    disk.opener->failOpenWith(makeError(ErrorCode::IoError, "not found", 2));
    expectError(cli.run({"scan", "--source", "\\\\.\\PhysicalDrive7"}), "scan", "There is no such disk");
    EXPECT_FALSE(std::filesystem::exists(sessions));
}

TEST_F(CliErrorsTest, AnImageAnotherProgramWrites) {
    // Images are opened sharing reading only: an open writer keeps the CLI out.
    std::ofstream writer(image, std::ios::binary | std::ios::app);
    ASSERT_TRUE(writer.is_open());
    expectError(cli.run({"inspect", "--source", arg(image)}), "inspect",
                "Another program has it open for writing; close it and try again");
}

TEST_F(CliErrorsTest, ImageOutputs) {
    // An existing file is never overwritten.
    const std::filesystem::path taken = dir.path() / "taken.img";
    ::recovery::test::writeFile(taken, Bytes(100, std::byte{7}));
    expectError(cli.run({"image", "--source", arg(image), "--output", arg(taken)}), "image",
                "exists: the CLI never overwrites a file, choose another name");
    EXPECT_EQ(::recovery::test::readFile(taken), Bytes(100, std::byte{7}));
    // Nothing to resume.
    expectError(cli.run({"image", "--source", arg(image), "--output", arg(dir.path() / "new.img"), "--resume"}),
                "image", "there is no image");
    // The source itself.
    expectError(cli.run({"image", "--source", arg(image), "--output", arg(image), "--resume"}), "image",
                "destination is the source image itself");
    EXPECT_TRUE(::recovery::test::readFile(image) == test::card());
    // On the source disk.
    const test::SimulatedDisk disk = test::simulatedDisk(test::card());
    cli.environment().diskOpener = disk.opener;
    cli.environment().diskResolver = [](const std::filesystem::path&) -> Result<std::vector<std::uint32_t>> {
        return std::vector<std::uint32_t>{7};
    };
    expectError(cli.run({"image", "--source", "\\\\.\\PhysicalDrive7", "--output", arg(dir.path() / "disk.img")}),
                "image", "destination is on the source disk (PhysicalDrive7)");
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "disk.img"));
}

TEST_F(CliErrorsTest, SessionsFolders) {
    // A file where the folder should be.
    const std::filesystem::path file = dir.path() / "file.txt";
    ::recovery::test::writeFile(file, Bytes(3, std::byte{1}));
    expectError(cli.run({"scan", "--source", arg(image), "--sessions-dir", arg(file)}), "scan",
                "cannot create a session in '" + arg(file) + "'");
    // No default folder and none given.
    cli.environment().defaultSessionsRoot.reset();
    expectError(cli.run({"scan", "--source", arg(image)}), "scan",
                "there is no default sessions folder (%LOCALAPPDATA% is not set): give one with --sessions-dir");
    expectError(cli.run({"report"}), "report", "there is no default sessions folder");
    // On the source disk.
    cli.environment().defaultSessionsRoot = sessions;
    const test::SimulatedDisk disk = test::simulatedDisk(test::card());
    cli.environment().diskOpener = disk.opener;
    cli.environment().diskResolver = [](const std::filesystem::path&) -> Result<std::vector<std::uint32_t>> {
        return std::vector<std::uint32_t>{7};
    };
    expectError(cli.run({"scan", "--source", "\\\\.\\PhysicalDrive7"}), "scan",
                "destination is on the source disk (PhysicalDrive7)");
    EXPECT_FALSE(std::filesystem::exists(sessions));
}

TEST_F(CliErrorsTest, SessionsThatDoNotExistOrCannotBeRead) {
    const std::string unknown = "20990101-000000-00000000";
    const std::string says = "there is no session '" + unknown + "' in '" + arg(sessions) + "'";
    expectError(cli.run({"scan", "--session", unknown}), "scan", says);
    expectError(cli.run({"recover", "--session", unknown, "--output", arg(dir.path() / "out")}), "recover", says);
    expectError(cli.run({"report", "--session", unknown}), "report", says);
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "out"));

    // A journal that is not one.
    std::filesystem::create_directories(sessions / "bogus");
    ::recovery::test::writeFile(sessions / "bogus" / "session.journal", ::recovery::test::makePattern(4096, 3));
    const Bytes garbage = ::recovery::test::readFile(sessions / "bogus" / "session.journal");
    expectError(cli.run({"report", "--session", "bogus"}), "report", "cannot open session 'bogus': INVALID_FORMAT");
    expectError(cli.run({"scan", "--session", "bogus"}), "scan", "cannot open session 'bogus'");
    // Refused, it is left as it is.
    EXPECT_TRUE(::recovery::test::readFile(sessions / "bogus" / "session.journal") == garbage);
    // The listing names it as unreadable.
    const CliResult listed = cli.run({"report"});
    ASSERT_EQ(listed.code, ExitCode::Success) << listed;
    EXPECT_TRUE(contains(listed.out, "bogus")) << listed;
    EXPECT_TRUE(contains(listed.out, "unreadable")) << listed;
}

TEST_F(CliErrorsTest, ASessionInUse) {
    const std::string id = scanned();
    Result<std::unique_ptr<session::RecoverySession>> held = session::RecoverySession::open(sessions / id);
    ASSERT_TRUE(held.ok());
    const std::string says = "session '" + id + "' is in use: another recovery command is running it";
    expectError(cli.run({"scan", "--session", id}), "scan", says);
    expectError(cli.run({"recover", "--session", id, "--output", arg(dir.path() / "out")}), "recover", says);
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "out"));
    EXPECT_TRUE((*held)->info().jobs.empty());
}

TEST_F(CliErrorsTest, RecoveryDestinations) {
    const std::string id = scanned();
    // A file.
    const std::filesystem::path file = dir.path() / "file.txt";
    ::recovery::test::writeFile(file, Bytes(3, std::byte{1}));
    expectError(cli.run({"recover", "--session", id, "--output", arg(file)}), "recover",
                "'" + arg(file) + "' exists and is not a folder");
    // The source image.
    expectError(cli.run({"recover", "--session", id, "--output", arg(image)}), "recover", "is not a folder");
    // Ids the session does not have.
    expectError(cli.run({"recover", "--session", id, "--output", arg(dir.path() / "out"), "--ids", "1,99"}),
                "recover", "there is no candidate 99: session " + id + " has 10");
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "out"));
    // No job was recorded for any of them.
    Result<std::unique_ptr<session::RecoverySession>> opened = session::RecoverySession::open(sessions / id);
    ASSERT_TRUE(opened.ok());
    EXPECT_TRUE((*opened)->info().jobs.empty());
}

TEST_F(CliErrorsTest, RecoveryToTheSourceDisk) {
    const test::SimulatedDisk disk = test::simulatedDisk(test::card());
    cli.environment().diskOpener = disk.opener;
    const CliResult scan = cli.run({"scan", "--source", "\\\\.\\PhysicalDrive7"});
    ASSERT_EQ(scan.code, ExitCode::Success) << scan;
    const std::string id = test::sessionIdOf(scan.out).value_or("");
    // The output folder turns out to be on disk 7. (The resolver is asked
    // about the nearest folder that exists: the output folder itself.)
    const std::filesystem::path out = dir.path() / "out";
    std::filesystem::create_directory(out);
    cli.environment().diskResolver = [&](const std::filesystem::path& path) -> Result<std::vector<std::uint32_t>> {
        const bool output = path.native().find(L"out") != std::wstring::npos;
        return std::vector<std::uint32_t>{output ? 7u : 0u};
    };
    expectError(cli.run({"recover", "--session", id, "--output", arg(out)}), "recover",
                "destination is on the source disk (PhysicalDrive7); writing there could overwrite recoverable data");
    EXPECT_TRUE(test::filesBelow(out).empty());
    // Nor a report, nor a log there.
    expectError(cli.run({"report", "--session", id, "--output", arg(out / "report.txt")}), "report",
                "destination is on the source disk");
    expectError(cli.run({"report", "--session", id, "--log", arg(out / "run.log")}), "report",
                "cannot open the log file");
    EXPECT_FALSE(std::filesystem::exists(out / "report.txt"));
    EXPECT_FALSE(std::filesystem::exists(out / "run.log"));
}

TEST_F(CliErrorsTest, TheSessionsSourceIsGoneOrChanged) {
    const std::string id = scanned();
    const std::filesystem::path moved = dir.path() / "moved.img";
    std::filesystem::rename(image, moved);
    const CliResult gone = cli.run({"recover", "--session", id, "--output", arg(dir.path() / "out")});
    expectError(gone, "recover", "cannot read the session's source: INVALID_INPUT: the image file '" + arg(image) +
                                     "' does not exist");
    EXPECT_TRUE(contains(gone.err, "it must be attached and unchanged")) << gone;

    // Back, but changed where the fingerprint looks: the session refuses it.
    Bytes changed = test::card();
    changed[100] ^= std::byte{0xFF};
    std::filesystem::remove(moved);
    (void)test::writeImage(image, changed);
    const CliResult different = cli.run({"recover", "--session", id, "--output", arg(dir.path() / "out")});
    expectError(different, "recover",
                "the source is not the one the session was made for, or it has changed since");
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "out"));
    Result<std::unique_ptr<session::RecoverySession>> opened = session::RecoverySession::open(sessions / id);
    ASSERT_TRUE(opened.ok());
    EXPECT_TRUE((*opened)->info().jobs.empty());
}

TEST_F(CliErrorsTest, ReportOutputsAreNeverOverwritten) {
    const std::string id = scanned();
    const std::filesystem::path taken = dir.path() / "taken.txt";
    ::recovery::test::writeFile(taken, Bytes(5, std::byte{9}));
    expectError(cli.run({"report", "--session", id, "--output", arg(taken)}), "report",
                "cannot create the report '" + arg(taken) + "' (an existing file is never overwritten)");
    expectError(cli.run({"report", "--output", arg(taken)}), "report", "an existing file is never overwritten");
    EXPECT_EQ(::recovery::test::readFile(taken), Bytes(5, std::byte{9}));
}

TEST_F(CliErrorsTest, ErrorsAreOnTheErrorStreamOnly) {
    const CliResult result = cli.run({"inspect", "--source", arg(dir.path() / "nope.img")});
    EXPECT_TRUE(result.out.empty()) << result;
    EXPECT_TRUE(result.err.starts_with("recovery inspect: error: cannot read the source: INVALID_INPUT: ")) << result;
    EXPECT_TRUE(result.err.ends_with("\n")) << result;
}

// File names are read from the disk: one may hold an escape sequence that
// would clear the terminal, or an override that disguises its extension
// ("invoice<RLO>txt.jpg" shows as "invoicegpj.txt").
TEST_F(CliErrorsTest, NamesFromTheDiskCannotControlTheTerminal) {
    ::recovery::test::Fat32ImageBuilder builder;
    const Bytes photo = ::recovery::test::makeJpeg({});
    (void)builder.addFile(::recovery::test::Fat32ImageBuilder::root(), "evil\x1B[2J.jpg", photo);
    (void)builder.addFile(::recovery::test::Fat32ImageBuilder::root(), "invoice\xE2\x80\xAEtxt.jpg", photo);
    const std::filesystem::path card = test::writeImage(dir.path() / "evil.img", builder.build());
    const CliResult scan = cli.run({"scan", "--source", arg(card), "--mode", "quick"});
    ASSERT_EQ(scan.code, ExitCode::Success) << scan;
    const std::string id = test::sessionIdOf(scan.out).value_or("");

    const CliResult text = cli.run({"report", "--session", id});
    ASSERT_EQ(text.code, ExitCode::Success) << text;
    EXPECT_FALSE(contains(text.out, "\x1B")) << text;
    EXPECT_FALSE(contains(text.out, "\xE2\x80\xAE")) << text;
    EXPECT_TRUE(contains(text.out, "/evil\\x1B[2J.jpg")) << text;
    EXPECT_TRUE(contains(text.out, "/invoice\\u{202E}txt.jpg")) << text;

    // JSON keeps the names exactly, escaped as JSON escapes them.
    const CliResult json = cli.run({"report", "--session", id, "--format", "json"});
    ASSERT_EQ(json.code, ExitCode::Success) << json;
    EXPECT_FALSE(contains(json.out, "\x1B")) << json;
    const std::optional<test::json::Value> value = test::json::parse(json.out);
    ASSERT_TRUE(value.has_value());
    std::set<std::string> names;
    for (std::size_t i = 0; i < (*value)["candidates"].size(); ++i) {
        names.insert((*value)["candidates"][i]["name"].string());
    }
    EXPECT_TRUE(names.contains("evil\x1B[2J.jpg"));
    EXPECT_TRUE(names.contains("invoice\xE2\x80\xAEtxt.jpg"));

    // And the files written get safe names.
    const CliResult recovered = cli.run({"recover", "--session", id, "--output", arg(dir.path() / "out")});
    ASSERT_EQ(recovered.code, ExitCode::Success) << recovered;
    for (const auto& [name, bytes] : test::filesBelow(dir.path() / "out")) {
        EXPECT_FALSE(contains(name, "\x1B")) << name;
        EXPECT_TRUE(bytes == photo) << name;
    }
}

}  // namespace
}  // namespace recovery::cli
