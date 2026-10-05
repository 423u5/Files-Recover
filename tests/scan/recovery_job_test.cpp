// The recovery job (P15): files written on several workers get exactly the
// names one writer gives them one after the other (names that collide, in
// any case, with " (n)" suffixes, a file named like a directory); a job
// interrupted anywhere resumes without writing a file twice; files that
// cannot be written are reported and the job goes on; unreadable sectors are
// counted; a paused job reads nothing; checkpoints of another job are refused.

#include "scan/recovery_job.hpp"

#include "recovery/sha256.hpp"
#include "scan_test_support.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <thread>

namespace recovery::scan {
namespace {

using namespace std::chrono_literals;
using test::Bytes;

constexpr std::size_t kSourceSize = 256 * kKiB;

// Where each candidate's file went, relative to the destination, and its bytes.
using Layout = std::map<std::uint64_t, std::pair<std::filesystem::path, Bytes>>;

evaluation::EvaluatedCandidate candidate(std::uint64_t id, std::string name, std::string path, std::uint64_t offset,
                                         std::uint64_t length) {
    evaluation::EvaluatedCandidate made;
    made.id = evaluation::EvaluatedCandidateId{id};
    RecoveryCandidate& data = made.data;
    data.id = CandidateId{id};
    data.filename = std::move(name);
    data.filesystemEvidence.path = std::move(path);
    data.expectedSize = length;
    data.sourceRegions = {SourceRegion{0, length, RegionKind::Stored, offset, false}};
    return made;
}

// Candidates whose names collide in every way RecoveryWriter numbers them.
std::vector<evaluation::EvaluatedCandidate> collidingCandidates() {
    const std::vector<std::pair<std::string, std::string>> names = {
        {"photo.jpg", "/photo.jpg"},           {"PHOTO.JPG", "/PHOTO.JPG"},
        {"photo (1).jpg", "/photo (1).jpg"},   {"photo.jpg", "/photo.jpg"},
        {"song.mp3", "/MUSIC/song.mp3"},       {"DCIM", "/DCIM"},
        {"a.jpg", "/DCIM/a.jpg"},              {"a.jpg", "/dcim/a.jpg"},
        {"A.JPG", "/DCIM/A.JPG"},              {"b.jpg", "/DCIM/100CAM/b.jpg"},
        {"100CAM", "/DCIM/100CAM"},            {"b.jpg", "/DCIM/100CAM/b.jpg"},
        {"clip.mp4", "/clip.mp4"},             {"clip.mp4", "/clip.mp4"},
        {"recovered_000001.jpg", ""},          {"recovered_000002.png", ""},
        {"café.jpg", "/café.jpg"}, {"CAFÉ.JPG", "/CAFÉ.JPG"},
        {"notes.txt", "/MUSIC/notes.txt"},     {"MUSIC", "/MUSIC"},
    };
    std::vector<evaluation::EvaluatedCandidate> candidates;
    std::uint64_t offset = 0;
    for (std::size_t round = 0; round < 3; ++round) {
        for (const auto& [name, path] : names) {
            const std::uint64_t length = 1000 + (offset * 7) % 3000;
            candidates.push_back(candidate(candidates.size() + 1, name, path, offset % (kSourceSize - 5000), length));
            offset += 4321;
        }
    }
    return candidates;
}

// What one writer does, file after file, in candidate order.
Layout writeOneByOne(storage::IStorageSource& source, const std::filesystem::path& destination,
                     const std::vector<evaluation::EvaluatedCandidate>& candidates) {
    Layout layout;
    Result<RecoveryWriter> writer = RecoveryWriter::create(source, destination);
    EXPECT_TRUE(writer.ok());
    for (const evaluation::EvaluatedCandidate& item : candidates) {
        Result<RecoveredFile> file = writer->recover(item.data);
        if (file.ok()) {
            layout[item.id.value()] = {std::filesystem::relative(file->path, destination),
                                       ::recovery::test::readFile(file->path)};
        }
    }
    return layout;
}

Layout layoutOf(const RecoveryJobCheckpoint& checkpoint, const std::filesystem::path& destination) {
    Layout layout;
    for (const auto& [id, item] : checkpoint.items()) {
        if (item.file.has_value()) {
            layout[id] = {std::filesystem::relative(item.file->path, destination),
                          ::recovery::test::readFile(item.file->path)};
        }
    }
    return layout;
}

std::size_t filesBelow(const std::filesystem::path& root) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        count += entry.is_regular_file() ? 1 : 0;
    }
    return count;
}

RecoveryJobOptions jobOptions(std::uint32_t workers) {
    RecoveryJobOptions options;
    options.workerThreads = workers;
    options.checkpointItems = 4;
    options.progressInterval = std::chrono::milliseconds{0};
    return options;
}

struct JobCollector {
    RecoveryJobCheckpoint checkpoint;
    std::uint64_t updates = 0;
    std::function<Status(const RecoveryJobUpdate&)> hook;

    RecoveryJobUpdateSink sink() {
        return [this](const RecoveryJobUpdate& update) -> Status {
            if (hook) {
                if (Status hooked = hook(update); !hooked.ok()) {
                    return hooked;
                }
            }
            ++updates;
            return checkpoint.apply(update);
        };
    }
};

class RecoveryJobTest : public ::testing::Test {
protected:
    RecoveryJobTest() : source_(::recovery::test::makePattern(kSourceSize, 5)) { EXPECT_TRUE(source_.open().ok()); }

    ::recovery::test::MemoryStorageSource source_;
    ::recovery::test::TempDir dir_;
};

TEST_F(RecoveryJobTest, FilesGetTheNamesOneWriterGivesThemOneAfterTheOther) {
    const std::vector<evaluation::EvaluatedCandidate> candidates = collidingCandidates();
    const Layout expected = writeOneByOne(source_, dir_ / "one", candidates);
    ASSERT_EQ(expected.size(), candidates.size());
    for (const std::uint32_t workers : {1U, 3U, 8U}) {
        for (int repeat = 0; repeat < 3; ++repeat) {
            SCOPED_TRACE(std::to_string(workers) + " workers, run " + std::to_string(repeat));
            const std::filesystem::path out = dir_ / ("jobs-" + std::to_string(workers) + "-" + std::to_string(repeat));
            JobCollector collector;
            RecoveryJob job(source_, out, jobOptions(workers));
            const Result<RecoveryJobSummary> summary = job.run(candidates, collector.sink());
            RECOVERY_ASSERT_OK(summary);
            EXPECT_EQ(summary->outcome, RecoveryJobOutcome::Completed);
            EXPECT_TRUE(collector.checkpoint.complete());
            EXPECT_EQ(summary->metrics.recoveredFiles, candidates.size());
            EXPECT_EQ(summary->metrics.failedFiles, 0U);
            const Layout written = layoutOf(collector.checkpoint, out);
            ASSERT_EQ(written.size(), expected.size());
            for (const auto& [id, file] : expected) {
                EXPECT_EQ(written.at(id).first, file.first) << "candidate " << id;
                EXPECT_EQ(written.at(id).second, file.second) << "candidate " << id;
            }
            EXPECT_EQ(filesBelow(out), candidates.size());
        }
    }
}

TEST_F(RecoveryJobTest, AJobInterruptedAnywhereResumesWithoutWritingAFileTwice) {
    const std::vector<evaluation::EvaluatedCandidate> candidates = collidingCandidates();
    const Layout expected = writeOneByOne(source_, dir_ / "one", candidates);
    std::size_t resumed = 0;
    for (const std::size_t stopAfter : {1U, 5U, 17U, 33U, 59U}) {
        SCOPED_TRACE(stopAfter);
        const std::filesystem::path out = dir_ / ("job-" + std::to_string(stopAfter));
        JobCollector collector;
        RecoveryJobOptions options = jobOptions(4);
        JobControl control = options.control;
        options.onProgress = [control, stopAfter](const RecoveryJobProgress& progress) mutable {
            if (progress.done >= stopAfter) {
                control.requestCancellation();
            }
        };
        RecoveryJob first(source_, out, options);
        const Result<RecoveryJobSummary> stopped = first.run(candidates, collector.sink());
        RECOVERY_ASSERT_OK(stopped);
        if (stopped->outcome == RecoveryJobOutcome::Completed) {
            continue;  // it was done before the cancellation was seen
        }
        EXPECT_FALSE(collector.checkpoint.complete());
        // Every file named in the checkpoint exists, complete; nothing else was left behind.
        EXPECT_EQ(filesBelow(out), collector.checkpoint.items().size());
        ++resumed;
        RecoveryJob second(source_, out, jobOptions(2));
        const Result<RecoveryJobSummary> again = second.run(candidates, collector.sink(), &collector.checkpoint);
        RECOVERY_ASSERT_OK(again);
        EXPECT_EQ(again->outcome, RecoveryJobOutcome::Completed);
        EXPECT_EQ(again->metrics.recoveredFiles, candidates.size());
        const Layout written = layoutOf(collector.checkpoint, out);
        ASSERT_EQ(written.size(), expected.size());
        for (const auto& [id, file] : expected) {
            EXPECT_EQ(written.at(id).first, file.first) << "candidate " << id;
        }
        EXPECT_EQ(filesBelow(out), candidates.size());
    }
    EXPECT_GE(resumed, 3U);
}

TEST_F(RecoveryJobTest, FilesThatCannotBeWrittenAreReportedAndTheJobGoesOn) {
    std::vector<evaluation::EvaluatedCandidate> candidates;
    candidates.push_back(candidate(1, "good.jpg", "/good.jpg", 0, 2000));
    evaluation::EvaluatedCandidate lost = candidate(2, "lost.jpg", "/lost.jpg", 0, 2000);
    lost.data.sourceRegions = {SourceRegion{0, 2000, RegionKind::Missing, 0, false}};
    candidates.push_back(lost);
    candidates.push_back(candidate(3, "also good.png", "/also good.png", 4096, 1000));
    JobCollector collector;
    RecoveryJob job(source_, dir_ / "out", jobOptions(2));
    const Result<RecoveryJobSummary> summary = job.run(candidates, collector.sink());
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->metrics.files, 3U);
    EXPECT_EQ(summary->metrics.recoveredFiles, 2U);
    EXPECT_EQ(summary->metrics.failedFiles, 1U);
    EXPECT_EQ(summary->metrics.bytesRecovered, 3000U);
    const RecoveredItem& failed = collector.checkpoint.items().at(2);
    ASSERT_TRUE(failed.error.has_value());
    EXPECT_EQ(failed.error->code, ErrorCode::InvalidInput);
    EXPECT_FALSE(failed.file.has_value());
    EXPECT_EQ(filesBelow(dir_ / "out"), 2U);
}

TEST_F(RecoveryJobTest, UnreadableSectorsAreWrittenAsZerosAndCounted) {
    Bytes data = ::recovery::test::makePattern(kSourceSize, 9);
    ::recovery::test::MemoryStorageSource scratched(data);
    scratched.addBadSector(3);
    RECOVERY_ASSERT_OK(scratched.open());
    JobCollector collector;
    RecoveryJob job(scratched, dir_ / "out", jobOptions(2));
    const std::vector<evaluation::EvaluatedCandidate> candidates = {candidate(1, "scratched.jpg", "/s.jpg", 0, 4096)};
    const Result<RecoveryJobSummary> summary = job.run(candidates, collector.sink());
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->metrics.unreadableBytes, 512U);
    const RecoveredItem& item = collector.checkpoint.items().at(1);
    ASSERT_TRUE(item.file.has_value());
    const Bytes written = ::recovery::test::readFile(item.file->path);
    ASSERT_EQ(written.size(), 4096U);
    for (std::size_t i = 3 * 512; i < 4 * 512; ++i) {
        EXPECT_EQ(written[i], std::byte{0});
    }
    EXPECT_TRUE(std::equal(written.begin(), written.begin() + 3 * 512, data.begin()));
}

TEST_F(RecoveryJobTest, APausedJobReadsNothingUntilResumed) {
    const std::vector<evaluation::EvaluatedCandidate> candidates = collidingCandidates();
    RecoveryJobOptions options = jobOptions(3);
    JobControl control = options.control;
    std::atomic<bool> pausedOnce{false};
    options.onProgress = [control, &pausedOnce](const RecoveryJobProgress& progress) mutable {
        if (!pausedOnce && progress.done >= 10) {
            pausedOnce = true;
            control.pause();
        }
    };
    JobCollector collector;
    RecoveryJob job(source_, dir_ / "out", options);
    Result<RecoveryJobSummary> summary = makeError(ErrorCode::InternalError, "not run");
    std::thread writing([&] { summary = job.run(candidates, collector.sink()); });
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!job.progress().paused && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(job.progress().paused);
    std::this_thread::sleep_for(50ms);
    const std::size_t reads = source_.readCount();
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(source_.readCount(), reads);
    control.resume();
    writing.join();
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, RecoveryJobOutcome::Completed);
    EXPECT_EQ(summary->metrics.recoveredFiles, candidates.size());
}

TEST_F(RecoveryJobTest, AJobResumesOnlyFromACheckpointOfTheSameJob) {
    const std::vector<evaluation::EvaluatedCandidate> candidates = collidingCandidates();
    JobCollector collector;
    RecoveryJobOptions options = jobOptions(2);
    JobControl control = options.control;
    options.onProgress = [control](const RecoveryJobProgress& progress) mutable {
        if (progress.done >= 6) {
            control.requestCancellation();
        }
    };
    RecoveryJob first(source_, dir_ / "out", options);
    RECOVERY_ASSERT_OK(first.run(candidates, collector.sink()));
    ASSERT_FALSE(collector.checkpoint.empty());

    RecoveryJob elsewhere(source_, dir_ / "elsewhere", jobOptions(2));
    RECOVERY_EXPECT_ERROR(elsewhere.run(candidates, nullptr, &collector.checkpoint), ErrorCode::InvalidInput);
    RecoveryJob fewer(source_, dir_ / "out", jobOptions(2));
    const std::vector<evaluation::EvaluatedCandidate> others(candidates.begin() + 50, candidates.end());
    RECOVERY_EXPECT_ERROR(fewer.run(others, nullptr, &collector.checkpoint), ErrorCode::InvalidInput);
    std::vector<evaluation::EvaluatedCandidate> twice = candidates;
    twice.push_back(candidates.front());
    RecoveryJob duplicate(source_, dir_ / "out", jobOptions(2));
    RECOVERY_EXPECT_ERROR(duplicate.run(twice, nullptr), ErrorCode::InvalidInput);

    // An update that does not follow is refused, all of it.
    RecoveryJobCheckpoint copy = collector.checkpoint;
    RecoveryJobUpdate again;
    again.sequence = copy.sequence() + 1;
    again.items.push_back(copy.items().begin()->second);
    RECOVERY_EXPECT_ERROR(copy.apply(again), ErrorCode::InvalidInput);
    EXPECT_EQ(copy.sequence(), collector.checkpoint.sequence());
}

// The scan's results, written: every candidate of the card that has data.
TEST_F(RecoveryJobTest, AScansCandidatesAreWritten) {
    const Bytes card = test::makeCard();
    ::recovery::test::MemoryStorageSource source(card);
    RECOVERY_ASSERT_OK(source.open());
    test::Collector scan;
    ScanCoordinator coordinator(source, test::allFormats(), test::allMedia(), {}, test::testRunOptions());
    RECOVERY_ASSERT_OK(coordinator.run(scan.sink()));
    ASSERT_FALSE(scan.candidates.empty());
    JobCollector collector;
    RecoveryJob job(source, dir_ / "recovered", jobOptions(4));
    const Result<RecoveryJobSummary> summary = job.run(scan.candidates, collector.sink());
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->metrics.recoveredFiles + summary->metrics.failedFiles, scan.candidates.size());
    for (const evaluation::EvaluatedCandidate& candidate : scan.candidates) {
        const RecoveredItem& item = collector.checkpoint.items().at(candidate.id.value());
        ASSERT_TRUE(item.file.has_value()) << test::describe(candidate);
        // What was written is what the evaluation hashed.
        const Bytes written = ::recovery::test::readFile(item.file->path);
        EXPECT_EQ(written.size(), candidate.recoveredSize());
        EXPECT_EQ(sha256(written), *candidate.identity.sha256);
    }
}

}  // namespace
}  // namespace recovery::scan
