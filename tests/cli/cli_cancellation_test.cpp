// Cancellation (P18): Ctrl+C at any point of a command. The command stops at
// its next consistent point, keeps what it did, says how to go on and exits
// with 3; run again, it goes on from there to the result of an uninterrupted
// run: the same candidates, the same files under the same names, the same
// image bytes. A Ctrl+C before a command begins is not lost.

#include "cli_test_support.hpp"
#include "imaging/image_metadata.hpp"
#include "session/recovery_session.hpp"
#include "support/test_files.hpp"

#include <gtest/gtest.h>

#include <array>

namespace recovery::cli {
namespace {

using test::arg;
using test::Bytes;
using test::Cli;
using test::CliResult;

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

class CliCancellationTest : public ::testing::Test {
protected:
    std::string scanned() {
        const CliResult result = cli.run({"scan", "--source", arg(image)});
        EXPECT_EQ(result.code, ExitCode::Success) << result;
        return test::sessionIdOf(result.out).value_or(std::string());
    }

    std::optional<session::SessionState> scanState(const std::string& id) const {
        const Result<session::SessionSummary> summary = session::readSessionSummary(sessions / id);
        EXPECT_TRUE(summary.ok());
        return summary.ok() ? summary->state : std::nullopt;
    }

    // A recover command that presses Ctrl+C once `done` files are written.
    CliResult recoverStoppedAfter(const std::string& id, const std::filesystem::path& out, std::uint64_t done) {
        bool asked = false;
        return cli.run({"recover", "--session", id, "--output", arg(out), "--workers", "1"},
                       [&](const ProgressEvent& event) {
                           if (!asked && event.recovery.has_value() && event.recovery->done >= done) {
                               asked = true;
                               (void)cli.interrupt().request();
                           }
                       });
    }

    ::recovery::test::TempDir dir;
    std::filesystem::path sessions = dir.path() / "sessions";
    Cli cli{sessions};
    std::filesystem::path image = test::writeImage(dir.path() / "card.img", test::card());
};

TEST_F(CliCancellationTest, AScanStoppedAtAnyStageGoesOnToTheSameResult) {
    constexpr std::array kStages = {scan::ScanStage::Volumes,     scan::ScanStage::Mp4Examination,
                                    scan::ScanStage::FragmentSeeds, scan::ScanStage::SourcePass,
                                    scan::ScanStage::Mp4Delivery,   scan::ScanStage::Fragments,
                                    scan::ScanStage::Evaluation};
    for (const scan::ScanStage stage : kStages) {
        SCOPED_TRACE(std::string(scan::toString(stage)));
        bool asked = false;
        const CliResult stopped = cli.run({"scan", "--source", arg(image)}, [&](const ProgressEvent& event) {
            if (!asked && event.scan.has_value() && event.scan->stage == stage) {
                asked = true;
                (void)cli.interrupt().request();
            }
        });
        ASSERT_EQ(stopped.code, ExitCode::Cancelled) << stopped;
        const std::optional<std::string> id = test::sessionIdOf(stopped.out);
        ASSERT_TRUE(id.has_value()) << stopped;
        EXPECT_TRUE(contains(stopped.err, "The scan was stopped; the session keeps what it did. To go on: recovery "
                                          "scan --session " +
                                              *id))
            << stopped;
        EXPECT_EQ(scanState(*id), session::SessionState::Cancelled);

        const CliResult resumed = cli.run({"scan", "--session", *id});
        ASSERT_EQ(resumed.code, ExitCode::Success) << resumed;
        EXPECT_TRUE(contains(resumed.err, "Resuming the deep scan of session " + *id + " (cancelled at stage "))
            << resumed;
        EXPECT_EQ(test::sessionCandidates(sessions, *id), test::expectedCard(ScanMode::Deep));
        EXPECT_EQ(scanState(*id), session::SessionState::Completed);
    }
}

TEST_F(CliCancellationTest, CtrlCBeforeTheScanBegins) {
    const CliResult stopped = cli.runInterrupted({"scan", "--source", arg(image)});
    ASSERT_EQ(stopped.code, ExitCode::Cancelled) << stopped;
    const std::optional<std::string> id = test::sessionIdOf(stopped.out);
    ASSERT_TRUE(id.has_value()) << stopped;
    EXPECT_TRUE(contains(stopped.err, "Stopped before the scan began. To scan: recovery scan --session " + *id))
        << stopped;
    EXPECT_FALSE(scanState(*id).has_value());

    const CliResult resumed = cli.run({"scan", "--session", *id});
    ASSERT_EQ(resumed.code, ExitCode::Success) << resumed;
    EXPECT_TRUE(contains(resumed.err, "(not started)")) << resumed;
    EXPECT_EQ(test::sessionCandidates(sessions, *id), test::expectedCard(ScanMode::Deep));
}

TEST_F(CliCancellationTest, RecoveryRefusesAScanThatDidNotFinish) {
    bool asked = false;
    const CliResult stopped = cli.run({"scan", "--source", arg(image)}, [&](const ProgressEvent& event) {
        if (!asked && event.scan.has_value() && event.scan->stage == scan::ScanStage::SourcePass) {
            asked = true;
            (void)cli.interrupt().request();
        }
    });
    ASSERT_EQ(stopped.code, ExitCode::Cancelled) << stopped;
    const std::string id = test::sessionIdOf(stopped.out).value_or("");
    const CliResult refused = cli.run({"recover", "--session", id, "--output", arg(dir.path() / "out")});
    EXPECT_EQ(refused.code, ExitCode::Error) << refused;
    EXPECT_TRUE(contains(refused.err, "the scan of session " + id + " is not complete (cancelled at stage source "
                                                                    "pass): finish it first with recovery scan "
                                                                    "--session " +
                                          id))
        << refused;
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "out"));
}

TEST_F(CliCancellationTest, ARecoveryStoppedAfterAnyFileGoesOnWithoutWritingTwice) {
    const std::string id = scanned();
    const std::filesystem::path reference = dir.path() / "reference";
    ASSERT_EQ(cli.run({"recover", "--session", id, "--output", arg(reference)}).code, ExitCode::Success);
    const std::map<std::string, Bytes> expected = test::filesBelow(reference);
    ASSERT_EQ(expected.size(), 10u);

    for (std::uint64_t done = 0; done < 10; ++done) {
        SCOPED_TRACE(done);
        const std::filesystem::path out = dir.path() / ("out" + std::to_string(done));
        const CliResult stopped = recoverStoppedAfter(id, out, done);
        ASSERT_EQ(stopped.code, ExitCode::Cancelled) << stopped;
        EXPECT_TRUE(contains(stopped.err, "Recovery was stopped; what was written is kept. To go on, run the same "
                                          "command again."))
            << stopped;
        // What was written is whole: every file there is one of the files, under its name.
        for (const auto& [name, bytes] : test::filesBelow(out)) {
            const auto found = expected.find(name);
            ASSERT_NE(found, expected.end()) << name;
            EXPECT_TRUE(found->second == bytes) << name;
        }

        const CliResult resumed = cli.run({"recover", "--session", id, "--output", arg(out)});
        ASSERT_EQ(resumed.code, ExitCode::Success) << resumed;
        EXPECT_TRUE(contains(resumed.err, "Resuming recovery job ")) << resumed;
        EXPECT_EQ(test::filesBelow(out), expected);
    }
}

TEST_F(CliCancellationTest, CtrlCBeforeRecoveringAddsNoJob) {
    const std::string id = scanned();
    const std::filesystem::path out = dir.path() / "out";
    const CliResult stopped = cli.runInterrupted({"recover", "--session", id, "--output", arg(out)});
    ASSERT_EQ(stopped.code, ExitCode::Cancelled) << stopped;
    EXPECT_TRUE(contains(stopped.out, "Not done yet:    10")) << stopped;
    EXPECT_TRUE(test::filesBelow(out).empty());
    {
        Result<std::unique_ptr<session::RecoverySession>> opened = session::RecoverySession::open(sessions / id);
        ASSERT_TRUE(opened.ok());
        EXPECT_TRUE((*opened)->info().jobs.empty());
    }
    const CliResult recovered = cli.run({"recover", "--session", id, "--output", arg(out)});
    ASSERT_EQ(recovered.code, ExitCode::Success) << recovered;
    EXPECT_EQ(test::filesBelow(out).size(), 10u);
}

TEST_F(CliCancellationTest, ImagingStoppedGoesOnWithResume) {
    const std::filesystem::path copy = dir.path() / "copy.img";
    bool asked = false;
    const CliResult stopped = cli.run({"image", "--source", arg(image), "--output", arg(copy), "--block-size", "64K"},
                                      [&](const ProgressEvent& event) {
                                          if (!asked && event.imaging.has_value() &&
                                              event.imaging->bytesCompleted >= 512 * 1024) {
                                              asked = true;
                                              (void)cli.interrupt().request();
                                          }
                                      });
    ASSERT_EQ(stopped.code, ExitCode::Cancelled) << stopped;
    EXPECT_TRUE(contains(stopped.out, "State:           stopped (resumable)")) << stopped;
    EXPECT_TRUE(contains(stopped.err, "Imaging was stopped. To go on: recovery image --source " + arg(image) +
                                          " --output " + arg(copy) + " --resume"))
        << stopped;
    const Result<imaging::ImageMetadata> metadata = imaging::readImageMetadata(imaging::metadataPathFor(copy));
    ASSERT_TRUE(metadata.ok());
    EXPECT_EQ(metadata->state, imaging::ImageState::Cancelled);
    EXPECT_LT(metadata->bytesCompleted, test::card().size());

    // Without --resume the image is not touched.
    const CliResult refused = cli.run({"image", "--source", arg(image), "--output", arg(copy)});
    EXPECT_EQ(refused.code, ExitCode::Error) << refused;
    EXPECT_TRUE(contains(refused.err, "exists and is not complete (cancelled): add --resume to go on with it"))
        << refused;

    const CliResult resumed = cli.run({"image", "--source", arg(image), "--output", arg(copy), "--resume"});
    ASSERT_EQ(resumed.code, ExitCode::Success) << resumed;
    EXPECT_TRUE(contains(resumed.out, "(resumed at ")) << resumed;
    EXPECT_TRUE(::recovery::test::readFile(copy) == test::card());
}

TEST_F(CliCancellationTest, CtrlCBeforeImagingAndInspecting) {
    const std::filesystem::path copy = dir.path() / "copy.img";
    const CliResult stopped = cli.runInterrupted({"image", "--source", arg(image), "--output", arg(copy)});
    ASSERT_EQ(stopped.code, ExitCode::Cancelled) << stopped;
    const CliResult resumed = cli.run({"image", "--source", arg(image), "--output", arg(copy), "--resume"});
    ASSERT_EQ(resumed.code, ExitCode::Success) << resumed;
    EXPECT_TRUE(::recovery::test::readFile(copy) == test::card());

    const CliResult inspected = cli.runInterrupted({"inspect", "--source", arg(image)});
    EXPECT_EQ(inspected.code, ExitCode::Cancelled) << inspected;
    EXPECT_TRUE(contains(inspected.err, "Stopped.")) << inspected;
}

}  // namespace
}  // namespace recovery::cli
