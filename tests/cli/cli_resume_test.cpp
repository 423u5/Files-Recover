// Sessions resumed (P18): what a crash or a kill leaves (the journal cut
// after any record, or in the middle of one; a recovered file half written)
// is taken up by running the command again, through the CLI alone, to the
// result of an uninterrupted run. A complete scan is not run again; a source
// that is gone or has changed is refused, and the session is kept for when
// it is back.

#include "cli/format.hpp"
#include "cli_test_support.hpp"
#include "recovery/text.hpp"
#include "session/recovery_session.hpp"
#include "session/session_test_support.hpp"
#include "support/test_files.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace recovery::cli {
namespace {

using test::arg;
using test::Bytes;
using test::Cli;
using test::CliResult;

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::uint16_t typeOf(session::RecordType type) {
    return static_cast<std::uint16_t>(type);
}

class CliResumeTest : public ::testing::Test {
protected:
    std::string scanned() {
        const CliResult result = cli.run({"scan", "--source", arg(image)});
        EXPECT_EQ(result.code, ExitCode::Success) << result;
        return test::sessionIdOf(result.out).value_or(std::string());
    }

    std::filesystem::path folderOf(const std::string& id) const { return sessions / id; }

    // The journal as a crash leaves it: its first `size` bytes.
    void cutJournal(const std::string& id, const Bytes& journal, std::uint64_t size) const {
        session::test::writeJournal(folderOf(id), std::span(journal).first(static_cast<std::size_t>(size)));
    }

    ::recovery::test::TempDir dir;
    std::filesystem::path sessions = dir.path() / "sessions";
    Cli cli{sessions};
    std::filesystem::path image = test::writeImage(dir.path() / "card.img", test::card());
};

TEST_F(CliResumeTest, AScanGoesOnAfterACrashAtAnyRecord) {
    const std::string id = scanned();
    const Bytes journal = session::test::journalBytes(folderOf(id));
    const std::vector<session::JournalRecord> records = session::test::readRecords(folderOf(id));
    ASSERT_GE(records.size(), 4u);
    // The records of a deep scan of the card: the session, Started, the
    // updates, Completed. Cut after each of them but the last, and in the
    // middle of every update.
    std::vector<std::pair<std::uint64_t, bool>> cuts;  // (size, torn)
    for (std::size_t i = 0; i + 1 < records.size(); ++i) {
        cuts.emplace_back(records[i].offset + records[i].size, false);
        if (records[i + 1].type == typeOf(session::RecordType::ScanUpdate)) {
            cuts.emplace_back(records[i + 1].offset + records[i + 1].size / 2, true);
        }
    }
    for (const auto& [size, torn] : cuts) {
        SCOPED_TRACE("cut at " + std::to_string(size) + (torn ? " (in a record)" : ""));
        cutJournal(id, journal, size);
        const CliResult resumed = cli.run({"scan", "--session", id});
        ASSERT_EQ(resumed.code, ExitCode::Success) << resumed;
        EXPECT_EQ(contains(resumed.err, "The session's last record was incomplete"), torn) << resumed;
        EXPECT_EQ(test::sessionCandidates(sessions, id), test::expectedCard(ScanMode::Deep));
    }
}

TEST_F(CliResumeTest, AScanCompleteButNotRecordedSoIsNotRunAgain) {
    const std::string id = scanned();
    const Bytes journal = session::test::journalBytes(folderOf(id));
    const std::vector<session::JournalRecord> records = session::test::readRecords(folderOf(id));
    ASSERT_EQ(records.back().type, typeOf(session::RecordType::ScanState));
    // The last update was recorded; the crash came before Completed was.
    cutJournal(id, journal, records.back().offset);
    const CliResult resumed = cli.run({"scan", "--session", id});
    ASSERT_EQ(resumed.code, ExitCode::Success) << resumed;
    EXPECT_TRUE(contains(resumed.err, "is complete; there is nothing to resume")) << resumed;
    EXPECT_EQ(test::sessionCandidates(sessions, id), test::expectedCard(ScanMode::Deep));
}

TEST_F(CliResumeTest, AReportShowsAScanInterruptedByACrash) {
    const std::string id = scanned();
    const Bytes journal = session::test::journalBytes(folderOf(id));
    const std::vector<session::JournalRecord> records = session::test::readRecords(folderOf(id));
    // Started and three updates, then nothing.
    ASSERT_GE(records.size(), 6u);
    cutJournal(id, journal, records[4].offset + records[4].size);

    const CliResult text = cli.run({"report", "--session", id});
    ASSERT_EQ(text.code, ExitCode::Success) << text;
    EXPECT_TRUE(contains(text.out, "State:         started (interrupted: the program ended while it ran) at stage"))
        << text;
    const CliResult json = cli.run({"report", "--session", id, "--format", "json"});
    const std::optional<test::json::Value> value = test::json::parse(json.out);
    ASSERT_TRUE(value.has_value()) << json;
    EXPECT_EQ((*value)["scan"]["state"].string(), "started");
    EXPECT_TRUE((*value)["scan"]["interrupted"].boolean());
    EXPECT_TRUE((*value)["scan"]["runnable"].boolean());

    const CliResult listed = cli.run({"report"});
    EXPECT_TRUE(contains(listed.out, "started (")) << listed;

    const CliResult resumed = cli.run({"scan", "--session", id});
    ASSERT_EQ(resumed.code, ExitCode::Success) << resumed;
    EXPECT_EQ(test::sessionCandidates(sessions, id), test::expectedCard(ScanMode::Deep));
}

// A recovery job killed while writing its k-th file: the files it began are
// on disk (the last one half written) and no update says they are done.
// Running the command again rewrites them under the same names.
TEST_F(CliResumeTest, ARecoveryGoesOnAfterACrashInTheMiddleOfAFile) {
    const std::string id = scanned();
    const std::uint64_t scanEnd = session::test::journalBytes(folderOf(id)).size();
    const std::filesystem::path out = dir.path() / "out";
    ASSERT_EQ(cli.run({"recover", "--session", id, "--output", arg(out), "--workers", "1"}).code, ExitCode::Success);
    const std::map<std::string, Bytes> expected = test::filesBelow(out);
    ASSERT_EQ(expected.size(), 10u);
    const Bytes journal = session::test::journalBytes(folderOf(id));
    const std::vector<session::JournalRecord> records = session::test::readRecords(folderOf(id));

    // The files begun, in order, and where each one's record ends.
    struct Started {
        std::string name;
        std::uint64_t end = 0;
    };
    std::vector<Started> started;
    for (const session::JournalRecord& record : records) {
        if (record.offset < scanEnd) {
            continue;
        }
        if (record.type == typeOf(session::RecordType::JobUpdate)) {
            break;
        }
        if (record.type == typeOf(session::RecordType::FileStarted)) {
            const session::RecordPayload payload = session::test::decoded(record);
            const auto& file = std::get<session::FileStartedRecord>(payload);
            const Result<std::filesystem::path> path = pathFromUtf8(file.path);
            ASSERT_TRUE(path.ok());
            started.push_back(Started{toUtf8(path->filename()), record.offset + record.size});
        }
    }
    ASSERT_EQ(started.size(), 10u);

    for (const std::size_t k : {1u, 4u, 10u}) {
        SCOPED_TRACE("crash while writing file " + std::to_string(k));
        std::filesystem::remove_all(out);
        std::filesystem::create_directories(out);
        for (std::size_t i = 0; i < k; ++i) {
            const Bytes& bytes = expected.at(started[i].name);
            const std::size_t written = i + 1 == k ? bytes.size() / 2 : bytes.size();
            ::recovery::test::writeFile(out / started[i].name, Bytes(bytes.begin(), bytes.begin() + written));
        }
        cutJournal(id, journal, started[k - 1].end);

        const CliResult resumed = cli.run({"recover", "--session", id, "--output", arg(out)});
        ASSERT_EQ(resumed.code, ExitCode::Success) << resumed;
        EXPECT_TRUE(contains(resumed.err, "Resuming recovery job 1")) << resumed;
        EXPECT_EQ(test::filesBelow(out), expected);
        const CliResult report = cli.run({"report", "--session", id});
        EXPECT_TRUE(contains(report.out, "10 of 10 files done: 10 recovered, 0 failed")) << report;
    }
}

TEST_F(CliResumeTest, ACompleteScanIsNotRunAgain) {
    const std::string id = scanned();
    const Bytes before = session::test::journalBytes(folderOf(id));
    const CliResult again = cli.run({"scan", "--session", id});
    ASSERT_EQ(again.code, ExitCode::Success) << again;
    EXPECT_TRUE(contains(again.err, "The scan of session " + id + " is complete; there is nothing to resume."))
        << again;
    EXPECT_TRUE(contains(again.out, "Candidates:      10")) << again;
    EXPECT_TRUE(session::test::journalBytes(folderOf(id)) == before);
}

TEST_F(CliResumeTest, AScanGoesOnOnlyWithItsSource) {
    bool asked = false;
    const CliResult stopped = cli.run({"scan", "--source", arg(image)}, [&](const ProgressEvent& event) {
        if (!asked && event.scan.has_value() && event.scan->stage == scan::ScanStage::SourcePass) {
            asked = true;
            (void)cli.interrupt().request();
        }
    });
    ASSERT_EQ(stopped.code, ExitCode::Cancelled) << stopped;
    const std::string id = test::sessionIdOf(stopped.out).value_or("");
    const Bytes journal = session::test::journalBytes(folderOf(id));

    // Changed where the fingerprint looks.
    Bytes changed = test::card();
    changed[100] ^= std::byte{0xFF};
    (void)test::writeImage(image, changed);
    const CliResult different = cli.run({"scan", "--session", id});
    EXPECT_EQ(different.code, ExitCode::Error) << different;
    EXPECT_TRUE(contains(different.err, "the source is not the session's")) << different;
    EXPECT_TRUE(contains(different.err, "it must be attached and unchanged")) << different;

    // Gone.
    std::filesystem::remove(image);
    const CliResult gone = cli.run({"scan", "--session", id});
    EXPECT_EQ(gone.code, ExitCode::Error) << gone;
    EXPECT_TRUE(contains(gone.err, "cannot read the session's source")) << gone;
    // Nothing was recorded meanwhile.
    EXPECT_TRUE(session::test::journalBytes(folderOf(id)) == journal);

    // Back as it was: the scan goes on.
    (void)test::writeImage(image, test::card());
    const CliResult resumed = cli.run({"scan", "--session", id});
    ASSERT_EQ(resumed.code, ExitCode::Success) << resumed;
    EXPECT_EQ(test::sessionCandidates(sessions, id), test::expectedCard(ScanMode::Deep));
}

}  // namespace
}  // namespace recovery::cli
