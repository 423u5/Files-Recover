// Recovery sessions (P16) on the scan tests' FAT32 card, the plan's tests:
//
//  * a normal session: a deep scan and recovery jobs, the session opened
//    again as after a restart, with everything it stores;
//  * pause and resume, live and across a restart (the process ending while
//    the scan is paused);
//  * crash simulation: the journal cut after every record, and in the middle
//    of records, opened again and resumed, without doing again what was
//    recorded; recovery jobs cut after every file, with the half-written file
//    a crash leaves, resumed with the same names;
//  * corrupt session data: damage before intact records, damaged headers and
//    session records, records that check but cannot be used, random damage;
//  * version compatibility: newer formats and records refused untouched,
//    records an older reader may skip skipped, sessions of another engine
//    opened without resuming their scan.
//
// Besides: only the session's source is used, one session object at a time,
// one operation at a time, and sessions are listed without opening them.

#include "session/recovery_session.hpp"

#include "formats/image_formats.hpp"
#include "recovery/sha256.hpp"
#include "recovery/text.hpp"
#include "recovery/version.hpp"
#include "scan/scan_test_support.hpp"
#include "session_test_support.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <thread>

namespace recovery::session {
namespace {

using namespace std::chrono_literals;
using test::Bytes;
using ::recovery::test::MemoryStorageSource;
using ::recovery::test::TempDir;

const Bytes& card() {
    static const Bytes bytes = scan::test::makeCard();
    return bytes;
}

// What a deep scan of the card delivers: the stages run one after the other.
const std::vector<std::string>& expectedDeep() {
    static const std::vector<std::string> expected = [] {
        MemoryStorageSource source(card());
        const bool opened = source.open().ok();
        return opened ? scan::test::describe(scan::test::referenceScan(source, ScanMode::Deep))
                      : std::vector<std::string>{};
    }();
    return expected;
}

std::vector<std::string> lines(const RecoverySession& session) {
    return scan::test::describe(session.candidates());
}

void expectExpectedCandidates(const RecoverySession& session) {
    const std::vector<std::string>& expected = expectedDeep();
    ASSERT_FALSE(expected.empty());
    const std::vector<std::string> actual = lines(session);
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(actual[i], expected[i]) << "candidate " << i;
    }
}

std::unique_ptr<RecoverySession> created(const std::filesystem::path& root, storage::IStorageSource& source,
                                         scan::ScanConfiguration configuration = {}) {
    Result<std::unique_ptr<RecoverySession>> session = RecoverySession::create(root, source, configuration);
    EXPECT_TRUE(session.ok()) << (session.ok() ? std::string() : describe(session.error()));
    return session.ok() ? std::move(session).value() : nullptr;
}

std::unique_ptr<RecoverySession> opened(const std::filesystem::path& folder) {
    Result<std::unique_ptr<RecoverySession>> session = RecoverySession::open(folder);
    EXPECT_TRUE(session.ok()) << (session.ok() ? std::string() : describe(session.error()));
    return session.ok() ? std::move(session).value() : nullptr;
}

Result<scan::ScanSummary> runScan(RecoverySession& session, storage::IStorageSource& source,
                                  scan::ScanRunOptions options = scan::test::testRunOptions()) {
    return session.runScan(source, scan::test::allFormats(), scan::test::allMedia(), std::move(options));
}

// The journal of a session whose deep scan of the card is complete (made once).
struct Scanned {
    Bytes journal;
    std::vector<JournalRecord> records;
};

const Scanned& scanned() {
    static const Scanned result = [] {
        Scanned made;
        TempDir dir;
        MemoryStorageSource source(card());
        if (!source.open().ok()) {
            return made;
        }
        Result<std::unique_ptr<RecoverySession>> session = RecoverySession::create(dir.path(), source, {});
        if (!session.ok()) {
            return made;
        }
        const std::filesystem::path folder = (*session)->folder();
        const Result<scan::ScanSummary> summary = runScan(**session, source);
        session->reset();
        if (summary.ok() && summary->outcome == scan::ScanOutcome::Completed) {
            made.journal = test::journalBytes(folder);
            made.records = test::readRecords(folder);
        }
        return made;
    }();
    return result;
}

Bytes prefix(const Bytes& bytes, std::uint64_t size) {
    return Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
}

std::optional<scan::ScanUpdate> scanUpdateOf(const JournalRecord& record) {
    if (record.type != static_cast<std::uint16_t>(RecordType::ScanUpdate)) {
        return std::nullopt;
    }
    return std::get<scan::ScanUpdate>(test::decoded(record));
}

// The files below `root`, relative path -> bytes.
std::map<std::string, Bytes> filesBelow(const std::filesystem::path& root) {
    std::map<std::string, Bytes> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file()) {
            files[toUtf8(std::filesystem::relative(entry.path(), root))] = ::recovery::test::readFile(entry.path());
        }
    }
    return files;
}

bool waitFor(const std::function<bool()>& condition, std::chrono::milliseconds limit = 20s) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > until) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

// ===========================================================================
// A normal session
// ===========================================================================

TEST(RecoverySessionTest, ASessionKeepsEverythingItStoresAcrossARestart) {
    TempDir dir;
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    std::filesystem::path folder;
    SessionInfo before;
    {
        std::unique_ptr<RecoverySession> session = created(dir.path(), source);
        ASSERT_NE(session, nullptr);
        folder = session->folder();
        EXPECT_EQ(toUtf8(folder.parent_path()), toUtf8(std::filesystem::absolute(dir.path()).lexically_normal()));
        EXPECT_EQ(toUtf8(folder.filename()), session->id());
        EXPECT_FALSE(session->info().scan.state.has_value());
        const Result<scan::ScanSummary> summary = runScan(*session, source);
        RECOVERY_ASSERT_OK(summary);
        EXPECT_EQ(summary->outcome, scan::ScanOutcome::Completed);
        expectExpectedCandidates(*session);
        before = session->info();
    }

    // Opened again, as by another process after a restart.
    const std::unique_ptr<RecoverySession> session = opened(folder);
    ASSERT_NE(session, nullptr);
    const SessionInfo after = session->info();
    // The session, the engine, the times.
    EXPECT_EQ(after.id, before.id);
    EXPECT_EQ(after.formatVersion, kJournalFormatVersion);
    EXPECT_EQ(after.engineVersion, kEngineVersion);
    EXPECT_EQ(after.created, before.created);
    EXPECT_EQ(after.updated, before.updated);
    EXPECT_LE(after.created, after.updated);
    // The source and its type.
    EXPECT_EQ(after.source.type, storage::SourceType::Synthetic);
    EXPECT_EQ(after.source.path, "memory://test");
    EXPECT_EQ(after.source.size, card().size());
    EXPECT_EQ(after.source.sectorSize, 512U);
    EXPECT_EQ(after.source.fingerprint, before.source.fingerprint);
    EXPECT_GE(after.source.fingerprint.bytes, SourceFingerprint::kBlock);
    // The configuration.
    EXPECT_EQ(after.configuration.mode, ScanMode::Deep);
    EXPECT_TRUE(after.configuration.carving && after.configuration.mp4 && after.configuration.fragments);
    EXPECT_FALSE(after.playability);
    // The progress: the state, its history, the stage, the metrics.
    EXPECT_EQ(after.scan.state, SessionState::Completed);
    EXPECT_FALSE(after.scan.interrupted);
    ASSERT_EQ(after.scan.history.size(), 2U);
    EXPECT_EQ(after.scan.history[0].state, SessionState::Started);
    EXPECT_EQ(after.scan.history[1].state, SessionState::Completed);
    EXPECT_EQ(after.scan.history[0].engineVersion, kEngineVersion);
    EXPECT_LE(after.scan.history[0].time, after.scan.history[1].time);
    EXPECT_EQ(after.scan.history[1].time, before.scan.history[1].time);
    EXPECT_EQ(after.scan.stage, scan::ScanStage::Completed);
    EXPECT_EQ(after.scan.updates, before.scan.updates);
    EXPECT_EQ(after.scan.metrics.candidates, expectedDeep().size());
    EXPECT_EQ(after.scan.metrics.bytesScanned, card().size());
    EXPECT_EQ(after.scan.metrics.elapsed, before.scan.metrics.elapsed);
    EXPECT_FALSE(after.scan.runnable);
    EXPECT_NE(after.scan.notRunnable.find("complete"), std::string::npos);
    // The filesystem.
    EXPECT_EQ(after.partitionScheme, before.partitionScheme);
    ASSERT_EQ(after.volumes.size(), 1U);
    EXPECT_EQ(after.volumes[0].filesystem, filesystem::FilesystemType::Fat32);
    EXPECT_TRUE(after.volumes[0].scanned);
    EXPECT_GE(after.volumes[0].files, 7U);
    EXPECT_EQ(after.volumes[0].serialNumber, before.volumes[0].serialNumber);
    // The candidates.
    EXPECT_EQ(session->candidateCount(), expectedDeep().size());
    expectExpectedCandidates(*session);
    EXPECT_EQ(lines(*session).front(), scan::test::describe(*session->candidate(evaluation::EvaluatedCandidateId{1})));
    EXPECT_EQ(session->candidates(2, 3).size(), 3U);
    EXPECT_TRUE(session->candidates(1000).empty());
    EXPECT_FALSE(session->candidate(evaluation::EvaluatedCandidateId{0}).has_value());
    // No errors and nothing unreadable on a sound card; nothing repaired.
    EXPECT_TRUE(session->errors().empty());
    EXPECT_TRUE(session->unreadableRegions().empty());
    EXPECT_TRUE(after.damage.empty());
    EXPECT_EQ(after.tornBytesDropped, 0U);

    // A complete scan does not run again.
    const Result<scan::ScanSummary> again = runScan(*session, source);
    ASSERT_FALSE(again.ok());
    EXPECT_EQ(again.error().code, ErrorCode::InvalidInput);
}

TEST(RecoverySessionTest, RecoveredFilesAndTheirReportsAreKept) {
    TempDir dir;
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    const std::filesystem::path folder = dir / "session";
    test::writeJournal(folder, scanned().journal);
    const std::filesystem::path destination = dir / "out";
    std::size_t candidates = 0;
    {
        const std::unique_ptr<RecoverySession> session = opened(folder);
        ASSERT_NE(session, nullptr);
        candidates = session->candidateCount();
        ASSERT_GT(candidates, 0U);
        const Result<std::uint32_t> job = session->addRecoveryJob(destination);
        RECOVERY_ASSERT_OK(job);
        EXPECT_EQ(*job, 1U);
        scan::RecoveryJobOptions options;
        options.workerThreads = 3;
        const Result<scan::RecoveryJobSummary> summary = session->runRecovery(*job, source, options);
        RECOVERY_ASSERT_OK(summary);
        EXPECT_EQ(summary->outcome, scan::RecoveryJobOutcome::Completed);
        // A second job: two candidates, in the order given.
        const Result<std::uint32_t> second = session->addRecoveryJob(
            dir / "chosen", {evaluation::EvaluatedCandidateId{3}, evaluation::EvaluatedCandidateId{1}});
        RECOVERY_ASSERT_OK(second);
        RECOVERY_ASSERT_OK(session->runRecovery(*second, source));
    }

    const std::unique_ptr<RecoverySession> session = opened(folder);
    ASSERT_NE(session, nullptr);
    const SessionInfo info = session->info();
    ASSERT_EQ(info.jobs.size(), 2U);
    const RecoveryJobStatus& job = info.jobs[0];
    EXPECT_EQ(job.id, 1U);
    EXPECT_EQ(job.destination, toUtf8(std::filesystem::absolute(destination).lexically_normal()));
    EXPECT_EQ(job.candidates.size(), candidates);
    EXPECT_EQ(job.state, SessionState::Completed);
    EXPECT_EQ(job.done, candidates);
    EXPECT_EQ(job.recovered + job.failed, candidates);
    EXPECT_GT(job.recovered, 0U);
    EXPECT_EQ(job.metrics.recoveredFiles, job.recovered);
    EXPECT_EQ(job.filesInProgress, 0U);
    EXPECT_EQ(info.jobs[1].state, SessionState::Completed);
    EXPECT_EQ(info.jobs[1].candidates,
              (std::vector<evaluation::EvaluatedCandidateId>{evaluation::EvaluatedCandidateId{3},
                                                             evaluation::EvaluatedCandidateId{1}}));

    // Each file written: where it went, and its reconstruction report (L42).
    const std::vector<scan::RecoveredItem> items = session->recoveredItems(1);
    ASSERT_EQ(items.size(), candidates);
    std::size_t written = 0;
    for (const scan::RecoveredItem& item : items) {
        const std::optional<evaluation::EvaluatedCandidate> candidate = session->candidate(item.candidate);
        ASSERT_TRUE(candidate.has_value());
        if (!item.file.has_value()) {
            ASSERT_TRUE(item.error.has_value());
            continue;
        }
        ++written;
        const Bytes bytes = ::recovery::test::readFile(item.file->path);
        EXPECT_EQ(bytes.size(), item.file->report.outputSize);
        EXPECT_EQ(item.file->report.expectedSize, candidate->data.expectedSize);
        if (candidate->identity.sha256.has_value()) {
            EXPECT_EQ(sha256(bytes), *candidate->identity.sha256) << toUtf8(item.file->path);
        }
    }
    EXPECT_EQ(written, info.jobs[0].recovered);
    EXPECT_EQ(session->recoveredItems(2).size(), 2U);
    EXPECT_TRUE(session->recoveredItems(3).empty());

    // A complete job does not run again; jobs of nothing are refused.
    EXPECT_FALSE(session->runRecovery(1, source).ok());
    EXPECT_FALSE(session->runRecovery(7, source).ok());
    EXPECT_FALSE(session->addRecoveryJob(dir / "x", {evaluation::EvaluatedCandidateId{999}}).ok());
    EXPECT_FALSE(
        session->addRecoveryJob(dir / "x", {evaluation::EvaluatedCandidateId{1}, evaluation::EvaluatedCandidateId{1}})
            .ok());
    EXPECT_FALSE(session->addRecoveryJob({}).ok());
}

// ===========================================================================
// Pause and resume
// ===========================================================================

// Pauses the session once the source pass has examined `bytes`.
void pauseInThePass(scan::ScanRunOptions& options, RecoverySession& session, std::uint64_t bytes,
                    std::atomic<bool>& paused) {
    options.onProgress = [&session, &paused, bytes](const scan::ScanProgress& progress) {
        if (!paused.load() && progress.stage == scan::ScanStage::SourcePass && progress.stageDone >= bytes) {
            paused.store(true);
            EXPECT_TRUE(session.pause().ok());
        }
    };
}

// P19: the update hooks of SessionOptions are told each update of the scan
// and of a job once the session has recorded it, in order, and nothing
// when a journal is replayed.
TEST(RecoverySessionTest, TheUpdateHooksAreToldEachUpdateOnceItIsRecorded) {
    TempDir dir;
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    const RecoverySession* live = nullptr;
    std::uint64_t scanUpdates = 0;
    std::uint64_t lastSequence = 0;
    std::size_t delivered = 0;
    std::map<std::uint32_t, std::size_t> jobItems;
    bool recorded = true;
    SessionOptions options;
    options.onScanUpdate = [&](const scan::ScanUpdate& update) {
        ++scanUpdates;
        recorded = recorded && update.sequence == lastSequence + 1;
        lastSequence = update.sequence;
        delivered += update.candidates.size();
        // Recorded: the accessors already return what it added.
        recorded = recorded && live->candidateCount() == delivered && live->info().scan.updates == update.sequence;
    };
    options.onJobUpdate = [&](std::uint32_t job, const scan::RecoveryJobUpdate& update) {
        jobItems[job] += update.items.size();
        recorded = recorded && live->recoveredItems(job).size() == jobItems[job];
    };
    Result<std::unique_ptr<RecoverySession>> made = RecoverySession::create(dir.path(), source, {}, options);
    RECOVERY_ASSERT_OK(made);
    live = made->get();
    const std::filesystem::path folder = (*made)->folder();
    RECOVERY_ASSERT_OK(runScan(**made, source));
    EXPECT_TRUE(recorded);
    EXPECT_GT(scanUpdates, 1U);
    EXPECT_EQ(scanUpdates, (*made)->info().scan.updates);
    EXPECT_EQ(delivered, (*made)->candidateCount());
    const Result<std::uint32_t> job = (*made)->addRecoveryJob(dir / "out");
    RECOVERY_ASSERT_OK(job);
    RECOVERY_ASSERT_OK((*made)->runRecovery(*job, source));
    EXPECT_TRUE(recorded);
    EXPECT_EQ(jobItems[*job], (*made)->candidateCount());
    made->reset();

    scanUpdates = 0;
    jobItems.clear();
    const Result<std::unique_ptr<RecoverySession>> reopened = RecoverySession::open(folder, options);
    RECOVERY_ASSERT_OK(reopened);
    EXPECT_EQ(scanUpdates, 0U);
    EXPECT_TRUE(jobItems.empty());
}

TEST(RecoverySessionTest, APausedScanIsRecordedAndResumes) {
    TempDir dir;
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    std::unique_ptr<RecoverySession> session = created(dir.path(), source);
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(session->pause().ok());  // nothing runs

    std::atomic<bool> paused{false};
    scan::ScanRunOptions options = scan::test::testRunOptions();
    pauseInThePass(options, *session, 512 * kKiB, paused);
    Result<scan::ScanSummary> summary = makeError(ErrorCode::InternalError, "not run");
    std::thread scanning([&] { summary = runScan(*session, source, options); });
    ASSERT_TRUE(waitFor([&] {
        const SessionProgress progress = session->progress();
        return progress.paused && progress.scan.has_value() && progress.scan->paused;
    }));
    // Recorded: in the session, and in its journal (read while in use).
    EXPECT_EQ(session->info().scan.state, SessionState::Paused);
    EXPECT_FALSE(session->info().scan.interrupted);
    const Result<SessionSummary> onDisk = readSessionSummary(session->folder());
    RECOVERY_ASSERT_OK(onDisk);
    EXPECT_EQ(onDisk->state, SessionState::Paused);
    EXPECT_EQ(onDisk->stage, scan::ScanStage::SourcePass);
    // While it runs, nothing else does.
    EXPECT_EQ(session->progress().operation, SessionOperation::Scan);
    const Result<scan::ScanSummary> busy = runScan(*session, source);
    ASSERT_FALSE(busy.ok());
    EXPECT_NE(busy.error().message.find("busy"), std::string::npos);

    RECOVERY_ASSERT_OK(session->resume());
    scanning.join();
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, scan::ScanOutcome::Completed);
    const SessionInfo info = session->info();
    std::vector<SessionState> states;
    for (const StateChange& change : info.scan.history) {
        states.push_back(change.state);
    }
    EXPECT_EQ(states, (std::vector<SessionState>{SessionState::Started, SessionState::Paused,
                                                 SessionState::Started, SessionState::Completed}));
    expectExpectedCandidates(*session);
    EXPECT_EQ(session->progress().operation, SessionOperation::None);
}

// Progress, information and results are read from other threads all through
// a scan that is paused and resumed meanwhile: every read sees a consistent
// session (candidates numbered without gaps, a state from the history), and
// the scan delivers what an undisturbed one does.
TEST(RecoverySessionTest, ProgressAndResultsAreReadFromOtherThreadsWhileTheScanRuns) {
    TempDir dir;
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    std::unique_ptr<RecoverySession> session = created(dir.path(), source);
    ASSERT_NE(session, nullptr);
    std::atomic<bool> running{true};
    std::atomic<std::size_t> reads{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&, r] {
            while (running.load()) {
                const SessionProgress progress = session->progress();
                const SessionInfo info = session->info();
                const std::vector<evaluation::EvaluatedCandidate> candidates = session->candidates();
                for (std::size_t i = 0; i < candidates.size(); ++i) {
                    EXPECT_EQ(candidates[i].id.value(), i + 1);
                }
                EXPECT_LE(candidates.size(), session->candidateCount());
                if (info.scan.state.has_value()) {
                    EXPECT_EQ(*info.scan.state, info.scan.history.back().state);
                }
                (void)session->errors();
                (void)session->unreadableRegions();
                (void)progress;
                if (r == 0 && session->pause().ok()) {
                    // Pausing and resuming at whatever point the scan is
                    // (it may end in between: nothing is left to resume).
                    std::this_thread::sleep_for(1ms);
                    (void)session->resume();
                }
                ++reads;
            }
        });
    }
    const Result<scan::ScanSummary> summary = runScan(*session, source, scan::test::testRunOptions(4));
    running.store(false);
    for (std::thread& reader : readers) {
        reader.join();
    }
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, scan::ScanOutcome::Completed);
    expectExpectedCandidates(*session);
    EXPECT_GT(reads.load(), 3U);
    // The pauses are on record, each followed by a resume or by the end.
    const std::vector<StateChange> history = session->info().scan.history;
    ASSERT_GE(history.size(), 2U);
    EXPECT_EQ(history.front().state, SessionState::Started);
    EXPECT_EQ(history.back().state, SessionState::Completed);
    for (std::size_t i = 1; i + 1 < history.size(); ++i) {
        if (history[i].state == SessionState::Paused) {
            EXPECT_TRUE(history[i + 1].state == SessionState::Started ||
                        history[i + 1].state == SessionState::Completed);
        }
    }
}

// The process ends while the scan is paused (the journal's last record is
// the pause): opened again, the scan is paused, not interrupted, and resumes
// from where it was paused.
TEST(RecoverySessionTest, AScanPausedWhenItsProcessEndedResumesAfterARestart) {
    TempDir dir;
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    std::filesystem::path folder;
    {
        std::unique_ptr<RecoverySession> session = created(dir / "first", source);
        ASSERT_NE(session, nullptr);
        folder = session->folder();
        std::atomic<bool> paused{false};
        scan::ScanRunOptions options = scan::test::testRunOptions();
        pauseInThePass(options, *session, 1 * kMiB, paused);
        Result<scan::ScanSummary> summary = makeError(ErrorCode::InternalError, "not run");
        std::thread scanning([&] { summary = runScan(*session, source, options); });
        ASSERT_TRUE(waitFor([&] {
            const SessionProgress progress = session->progress();
            return progress.scan.has_value() && progress.scan->paused;
        }));
        session->cancel();
        scanning.join();
        RECOVERY_ASSERT_OK(summary);
        EXPECT_EQ(summary->outcome, scan::ScanOutcome::Cancelled);
    }
    // Without the record of the cancellation: what the process ending while
    // paused leaves.
    const std::vector<JournalRecord> records = test::readRecords(folder);
    ASSERT_GE(records.size(), 3U);
    ASSERT_EQ(std::get<ScanStateRecord>(test::decoded(records.back())).state, SessionState::Cancelled);
    const std::filesystem::path restarted = dir / "restarted";
    test::writeJournal(restarted, prefix(test::journalBytes(folder), records.back().offset));

    const std::unique_ptr<RecoverySession> session = opened(restarted);
    ASSERT_NE(session, nullptr);
    const SessionInfo info = session->info();
    EXPECT_EQ(info.scan.state, SessionState::Paused);
    EXPECT_FALSE(info.scan.interrupted);
    EXPECT_TRUE(info.scan.runnable);
    // Where it was paused: in the pass, past its start.
    EXPECT_EQ(info.scan.stage, scan::ScanStage::SourcePass);
    ASSERT_TRUE(session->checkpoint().pass().has_value());
    const std::uint64_t position = session->checkpoint().pass()->position;
    EXPECT_GT(position, 0U);
    const std::uint64_t sequence = session->checkpoint().sequence();

    const Result<scan::ScanSummary> summary = runScan(*session, source);
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, scan::ScanOutcome::Completed);
    expectExpectedCandidates(*session);
    // The resumed run went on from the pause: its first update follows the
    // last recorded, in the pass, from where the pass was.
    bool first = true;
    for (const JournalRecord& record : test::readRecords(restarted)) {
        const std::optional<scan::ScanUpdate> update = scanUpdateOf(record);
        if (!update.has_value() || update->sequence <= sequence) {
            continue;
        }
        if (first) {
            EXPECT_EQ(update->sequence, sequence + 1);
            EXPECT_EQ(update->stage, scan::ScanStage::SourcePass);
            ASSERT_TRUE(update->pass.has_value());
            EXPECT_GE(update->pass->position, position);
            first = false;
        }
        EXPECT_GE(update->stage, scan::ScanStage::SourcePass);
    }
    EXPECT_FALSE(first);
}

// ===========================================================================
// Crash simulation
// ===========================================================================

// The journal of a complete scan cut after every one of its records (as a
// crash leaves it: whole records, then maybe part of the next), opened again
// and resumed: the candidates are those of an uninterrupted scan, each once,
// and no update of a stage the cut journal had finished is handed out again.
TEST(RecoverySessionTest, AScanCutOffAfterAnyRecordResumesAfterARestart) {
    const Scanned& full = scanned();
    ASSERT_GE(full.records.size(), 10U);
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    TempDir dir;
    std::size_t runs = 0;
    for (std::size_t k = 1; k < full.records.size(); ++k) {
        for (const bool torn : {false, true}) {
            if (torn && k % 4 != 1) {
                continue;
            }
            SCOPED_TRACE(std::to_string(k) + (torn ? " torn" : ""));
            const JournalRecord& next = full.records[k];
            Bytes cut = prefix(full.journal, next.offset + (torn ? next.size / 2 : 0));
            const std::filesystem::path folder = dir / ("cut-" + std::to_string(k) + (torn ? "-torn" : ""));
            test::writeJournal(folder, cut);

            const std::unique_ptr<RecoverySession> session = opened(folder);
            ASSERT_NE(session, nullptr);
            const SessionInfo info = session->info();
            EXPECT_EQ(info.tornBytesDropped, torn ? next.size / 2 : 0);
            EXPECT_TRUE(info.damage.empty());
            const scan::ScanStage stage = session->checkpoint().stage();
            const std::uint64_t sequence = session->checkpoint().sequence();
            if (!info.scan.runnable) {
                // Every update was recorded: complete, its end recorded at open.
                EXPECT_EQ(info.scan.state, SessionState::Completed);
                expectExpectedCandidates(*session);
                continue;
            }
            EXPECT_EQ(info.scan.interrupted, info.scan.state == SessionState::Started);
            const Result<scan::ScanSummary> summary = runScan(*session, source);
            ++runs;
            RECOVERY_ASSERT_OK(summary);
            EXPECT_EQ(summary->outcome, scan::ScanOutcome::Completed);
            expectExpectedCandidates(*session);
            const std::vector<evaluation::EvaluatedCandidate> candidates = session->candidates();
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                EXPECT_EQ(candidates[i].id.value(), i + 1);
            }
            // Nothing recorded was done again.
            bool first = true;
            for (const JournalRecord& record : test::readRecords(folder)) {
                const std::optional<scan::ScanUpdate> update = scanUpdateOf(record);
                if (!update.has_value() || update->sequence <= sequence) {
                    continue;
                }
                if (first) {
                    EXPECT_EQ(update->sequence, sequence + 1);
                    first = false;
                }
                EXPECT_GE(update->stage, stage);
            }
        }
    }
    EXPECT_GE(runs, 10U);
}

// A recovery job written one file at a time, cut after every record of its
// files (the file begun, or its update), with the destination as a crash
// leaves it: the files done before, and the file begun empty, half written
// or whole but not recorded. Resumed after a restart, the job removes what
// the crash left and writes the files with the names an uninterrupted job
// gives them, each once.
TEST(RecoverySessionTest, ARecoveryJobCutOffAnywhereResumesWithTheSameNames) {
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    TempDir dir;
    const std::filesystem::path destination = dir / "out";
    scan::RecoveryJobOptions oneByOne;
    oneByOne.workerThreads = 1;
    oneByOne.window = 1;
    oneByOne.checkpointItems = 1;

    // The uninterrupted job.
    const std::filesystem::path reference = dir / "reference";
    test::writeJournal(reference, scanned().journal);
    {
        const std::unique_ptr<RecoverySession> session = opened(reference);
        ASSERT_NE(session, nullptr);
        RECOVERY_ASSERT_OK(session->addRecoveryJob(destination));
        RECOVERY_ASSERT_OK(session->runRecovery(1, source, oneByOne));
    }
    const std::map<std::string, Bytes> expected = filesBelow(destination);
    ASSERT_GE(expected.size(), 5U);
    const Bytes journal = test::journalBytes(reference);
    const std::vector<JournalRecord> records = test::readRecords(reference);

    // Where each candidate's file went.
    std::map<std::uint64_t, std::string> pathOf;
    const auto relative = [&](const std::string& path) {
        return toUtf8(std::filesystem::relative(std::filesystem::path(std::u8string(path.begin(), path.end())),
                                                destination));
    };
    for (const JournalRecord& record : records) {
        if (record.type == static_cast<std::uint16_t>(RecordType::FileStarted)) {
            const auto started = std::get<FileStartedRecord>(test::decoded(record));
            pathOf[started.candidate.value()] = relative(started.path);
        }
    }

    std::size_t cuts = 0;
    std::set<std::uint64_t> done;
    for (std::size_t k = 0; k < records.size(); ++k) {
        const JournalRecord& record = records[k];
        std::optional<std::uint64_t> begun;
        if (record.type == static_cast<std::uint16_t>(RecordType::FileStarted)) {
            begun = std::get<FileStartedRecord>(test::decoded(record)).candidate.value();
        } else if (record.type == static_cast<std::uint16_t>(RecordType::JobUpdate)) {
            const RecordPayload update = test::decoded(record);
            for (const scan::RecoveredItem& item : std::get<JobUpdateRecord>(update).update.items) {
                done.insert(item.candidate.value());
            }
        } else {
            continue;
        }
        SCOPED_TRACE(k);
        // The destination as the crash leaves it.
        std::filesystem::remove_all(destination);
        std::filesystem::create_directories(destination);
        for (const std::uint64_t candidate : done) {
            if (pathOf.contains(candidate)) {
                const std::filesystem::path file = destination / pathOf[candidate];
                std::filesystem::create_directories(file.parent_path());
                ::recovery::test::writeFile(file, expected.at(pathOf[candidate]));
            }
        }
        if (begun.has_value()) {
            const Bytes& whole = expected.at(pathOf.at(*begun));
            const std::size_t kept = cuts % 3 == 0 ? 0 : (cuts % 3 == 1 ? whole.size() / 2 : whole.size());
            const std::filesystem::path file = destination / pathOf.at(*begun);
            std::filesystem::create_directories(file.parent_path());
            ::recovery::test::writeFile(file, Bytes(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(kept)));
        }
        const std::filesystem::path folder = dir / ("cut-" + std::to_string(k));
        test::writeJournal(folder, prefix(journal, record.offset + record.size));
        ++cuts;

        const std::unique_ptr<RecoverySession> session = opened(folder);
        ASSERT_NE(session, nullptr);
        const SessionInfo info = session->info();
        ASSERT_EQ(info.jobs.size(), 1U);
        if (info.jobs[0].state == SessionState::Completed) {
            continue;
        }
        EXPECT_TRUE(info.jobs[0].interrupted);
        EXPECT_EQ(info.jobs[0].filesInProgress, begun.has_value() ? 1U : 0U);
        const Result<scan::RecoveryJobSummary> summary = session->runRecovery(1, source, oneByOne);
        RECOVERY_ASSERT_OK(summary);
        EXPECT_EQ(summary->outcome, scan::RecoveryJobOutcome::Completed);
        const std::map<std::string, Bytes> files = filesBelow(destination);
        ASSERT_EQ(files.size(), expected.size()) << [&] {
            std::string names;
            for (const auto& [name, bytes] : files) {
                names += name + " (" + std::to_string(bytes.size()) + ") ";
            }
            names += "| expected ";
            for (const auto& [name, bytes] : expected) {
                names += name + " (" + std::to_string(bytes.size()) + ") ";
            }
            return names + "| record " + std::string(toString(static_cast<RecordType>(record.type)));
        }();
        for (const auto& [name, bytes] : files) {
            ASSERT_TRUE(expected.contains(name)) << name;
            EXPECT_EQ(bytes, expected.at(name)) << name;
        }
        EXPECT_EQ(session->info().jobs[0].state, SessionState::Completed);
    }
    EXPECT_GE(cuts, 10U);
}

// A run whose end was not recorded (a crash between its last update and its
// end): opened again, its end is recorded.
TEST(RecoverySessionTest, ARunCompleteButNotRecordedSoIsCompletedWhenOpened) {
    const Scanned& full = scanned();
    TempDir dir;
    const std::filesystem::path folder = dir / "session";
    test::writeJournal(folder, prefix(full.journal, full.records.back().offset));
    const std::unique_ptr<RecoverySession> session = opened(folder);
    ASSERT_NE(session, nullptr);
    const SessionInfo info = session->info();
    EXPECT_EQ(info.scan.state, SessionState::Completed);
    EXPECT_FALSE(info.scan.interrupted);
    expectExpectedCandidates(*session);
}

// ===========================================================================
// Corrupt session data
// ===========================================================================

TEST(RecoverySessionTest, ADamagedJournalOpensAsItWasBeforeTheDamage) {
    const Scanned& full = scanned();
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    TempDir dir;
    // A record in the middle, in its header and in its payload.
    const std::size_t k = full.records.size() / 2;
    const JournalRecord& victim = full.records[k];
    for (const std::uint64_t at : {std::uint64_t{18}, victim.size - 1}) {
        SCOPED_TRACE(at);
        Bytes damaged = full.journal;
        damaged[victim.offset + at] ^= std::byte{0x10};
        const std::filesystem::path folder = dir / ("damaged-" + std::to_string(at));
        test::writeJournal(folder, damaged);

        {
            const std::unique_ptr<RecoverySession> session = opened(folder);
            ASSERT_NE(session, nullptr);
            const SessionInfo info = session->info();
            ASSERT_EQ(info.damage.size(), 1U);
            const DamageRecord& damage = info.damage[0].damage;
            EXPECT_EQ(damage.offset, victim.offset);
            EXPECT_EQ(damage.bytesDropped, full.journal.size() - victim.offset);
            EXPECT_EQ(damage.recordsDropped, full.records.size() - k - 1);
            EXPECT_FALSE(damage.reason.empty());
            // The damaged journal is kept as it was.
            EXPECT_EQ(::recovery::test::readFile(folder / damage.backup), damaged);
            EXPECT_TRUE(info.scan.interrupted);
            EXPECT_TRUE(info.scan.runnable);
            const Result<scan::ScanSummary> summary = runScan(*session, source);
            RECOVERY_ASSERT_OK(summary);
            expectExpectedCandidates(*session);
        }
        // The damage stays on record.
        const std::unique_ptr<RecoverySession> again = opened(folder);
        ASSERT_NE(again, nullptr);
        EXPECT_EQ(again->info().damage.size(), 1U);
        EXPECT_EQ(again->info().scan.state, SessionState::Completed);
    }
}

TEST(RecoverySessionTest, DamagedHeadersAndSessionRecordsAreRefusedUntouched) {
    const Scanned& full = scanned();
    TempDir dir;
    const auto refused = [&](const Bytes& bytes, std::string_view why) {
        const std::filesystem::path folder = dir / ("refused-" + std::to_string(bytes.size()) + std::string(why));
        test::writeJournal(folder, bytes);
        const Result<std::unique_ptr<RecoverySession>> session = RecoverySession::open(folder);
        ASSERT_FALSE(session.ok()) << why;
        EXPECT_EQ(session.error().code, ErrorCode::InvalidFormat);
        EXPECT_NE(session.error().message.find(why), std::string::npos) << session.error().message;
        EXPECT_EQ(test::journalBytes(folder), bytes);
    };
    Bytes header = full.journal;
    header[3] ^= std::byte{1};
    refused(header, "not a session journal");
    Bytes version = full.journal;
    version[12] ^= std::byte{1};
    refused(version, "damaged");
    // The session's own record.
    Bytes first = full.journal;
    first[full.records[0].offset + kRecordHeaderSize + 2] ^= std::byte{1};
    refused(first, "no session record");
    refused(prefix(full.journal, kJournalHeaderSize), "no session record");
    refused(prefix(full.journal, full.records[0].offset + 10), "no session record");

    // No journal at all.
    const Result<std::unique_ptr<RecoverySession>> none = RecoverySession::open(dir / "nothing");
    ASSERT_FALSE(none.ok());
    EXPECT_EQ(none.error().code, ErrorCode::InvalidInput);
}

// Records whose bytes check but that cannot be used: one that does not
// decode, an update that does not follow, an update of a job that does not
// exist. Each is damage there.
TEST(RecoverySessionTest, RecordsThatCheckButCannotBeUsedAreDamage) {
    const Scanned& full = scanned();
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    TempDir dir;
    std::size_t firstUpdate = 0;
    while (!scanUpdateOf(full.records[firstUpdate]).has_value()) {
        ++firstUpdate;
    }
    const std::size_t k = full.records.size() / 2;
    JournalRecord garbage;
    garbage.type = static_cast<std::uint16_t>(RecordType::ScanUpdate);
    garbage.payload = ::recovery::test::makePattern(300, 77);
    JobUpdateRecord orphan;
    orphan.job = 4;
    struct Case {
        std::string name;
        JournalRecord record;
    };
    const std::vector<Case> cases = {
        {"undecodable", garbage},
        {"out of step", full.records[firstUpdate]},
        {"orphan job update", test::recordOf(RecordPayload{orphan})},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        std::vector<JournalRecord> records(full.records.begin(), full.records.begin() + static_cast<std::ptrdiff_t>(k));
        records.push_back(c.record);
        records.insert(records.end(), full.records.begin() + static_cast<std::ptrdiff_t>(k), full.records.end());
        const std::filesystem::path folder = dir / c.name;
        test::writeJournal(folder, test::buildJournal(records));
        const std::unique_ptr<RecoverySession> session = opened(folder);
        ASSERT_NE(session, nullptr);
        const SessionInfo info = session->info();
        ASSERT_EQ(info.damage.size(), 1U);
        EXPECT_EQ(info.damage[0].damage.recordsDropped, full.records.size() - k + 1);
        EXPECT_NE(info.damage[0].damage.reason.find("cannot be used"), std::string::npos)
            << info.damage[0].damage.reason;
        RECOVERY_ASSERT_OK(runScan(*session, source));
        expectExpectedCandidates(*session);
    }
}

// Random damage: flipped bytes and cuts anywhere. The session opens (as it
// was before the damage) or is refused cleanly; what it opens with is a
// beginning of what the scan delivers, and it resumes to the rest.
TEST(RecoverySessionTest, RandomDamageOpensAsItWasBeforeOrIsRefused) {
    const Scanned& full = scanned();
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    TempDir dir;
    std::mt19937_64 random(0xDA3A6E);
    std::size_t openedCount = 0;
    std::size_t refusedCount = 0;
    for (int round = 0; round < 40; ++round) {
        SCOPED_TRACE(round);
        Bytes damaged = full.journal;
        if (round % 4 == 3) {
            damaged.resize(kJournalHeaderSize + random() % (damaged.size() - kJournalHeaderSize));
        } else {
            for (int flip = 0; flip < 1 + round % 3; ++flip) {
                damaged[random() % damaged.size()] ^= static_cast<std::byte>(1 + random() % 255);
            }
        }
        const std::filesystem::path folder = dir / ("random-" + std::to_string(round));
        test::writeJournal(folder, damaged);
        Result<std::unique_ptr<RecoverySession>> session = RecoverySession::open(folder);
        if (!session.ok()) {
            ++refusedCount;
            EXPECT_EQ(session.error().code, ErrorCode::InvalidFormat) << describe(session.error());
            continue;
        }
        ++openedCount;
        const std::vector<std::string> got = lines(**session);
        const std::vector<std::string>& expected = expectedDeep();
        ASSERT_LE(got.size(), expected.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            EXPECT_EQ(got[i], expected[i]);
        }
        if (round % 5 == 0 && (*session)->info().scan.runnable) {
            RECOVERY_ASSERT_OK(runScan(**session, source));
            expectExpectedCandidates(**session);
        }
    }
    EXPECT_GT(openedCount, 0U);
}

// ===========================================================================
// Version compatibility
// ===========================================================================

TEST(RecoverySessionTest, SessionsOfANewerFormatAreRefusedUntouched) {
    const Scanned& full = scanned();
    TempDir dir;
    const auto refusedAsNewer = [&](const std::string& name, const Bytes& bytes) {
        SCOPED_TRACE(name);
        const std::filesystem::path folder = dir / name;
        test::writeJournal(folder, bytes);
        const Result<std::unique_ptr<RecoverySession>> session = RecoverySession::open(folder);
        ASSERT_FALSE(session.ok());
        EXPECT_EQ(session.error().code, ErrorCode::InvalidFormat);
        EXPECT_NE(session.error().message.find("newer"), std::string::npos) << session.error().message;
        EXPECT_EQ(test::journalBytes(folder), bytes);
        const Result<SessionSummary> summary = readSessionSummary(folder);
        RECOVERY_ASSERT_OK(summary);
        ASSERT_TRUE(summary->error.has_value());
        EXPECT_NE(summary->error->message.find("newer"), std::string::npos);
    };
    // A newer journal format.
    Bytes newer = encodeJournalHeader(kJournalFormatVersion + 1);
    newer.insert(newer.end(), full.journal.begin() + kJournalHeaderSize, full.journal.end());
    refusedAsNewer("format", newer);
    // A record type this engine does not know, that it may not skip.
    std::vector<JournalRecord> records = full.records;
    JournalRecord unknown;
    unknown.type = 99;
    unknown.payload = ::recovery::test::makePattern(20, 99);
    records.insert(records.begin() + 3, unknown);
    refusedAsNewer("type", test::buildJournal(records));
    // A flag this engine does not know.
    records = full.records;
    records[2].flags = 0x0004;
    refusedAsNewer("flag", test::buildJournal(records));
    // Listed with what is wrong.
    const Result<std::vector<SessionSummary>> listed = listSessions(dir.path());
    RECOVERY_ASSERT_OK(listed);
    ASSERT_EQ(listed->size(), 3U);
    for (const SessionSummary& summary : *listed) {
        EXPECT_TRUE(summary.error.has_value()) << toUtf8(summary.folder);
    }
}

TEST(RecoverySessionTest, RecordsAnOlderReaderMaySkipAreSkipped) {
    const Scanned& full = scanned();
    TempDir dir;
    std::vector<JournalRecord> records = full.records;
    JournalRecord extra;
    extra.type = 200;
    extra.flags = kRecordOptional;
    extra.payload = ::recovery::test::makePattern(40, 200);
    records.insert(records.begin() + 5, extra);
    records.push_back(extra);
    const std::filesystem::path folder = dir / "session";
    test::writeJournal(folder, test::buildJournal(records));
    const std::unique_ptr<RecoverySession> session = opened(folder);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->info().recordsSkipped, 2U);
    EXPECT_TRUE(session->info().damage.empty());
    expectExpectedCandidates(*session);
}

// The journal of `records` as engine `engine` writes it: the session's
// record, its states and the scan's identity name that engine.
Bytes asEngine(std::vector<JournalRecord> records, const std::string& engine) {
    for (JournalRecord& record : records) {
        RecordPayload payload = test::decoded(record);
        if (auto* session = std::get_if<SessionCreatedRecord>(&payload)) {
            session->engineVersion = engine;
        } else if (auto* state = std::get_if<ScanStateRecord>(&payload)) {
            state->engineVersion = engine;
        } else if (auto* update = std::get_if<scan::ScanUpdate>(&payload)) {
            if (update->identity.has_value()) {
                update->identity->engineVersion = engine;
            }
        }
        record = test::recordOf(payload, record.time);
    }
    return test::buildJournal(records);
}

TEST(RecoverySessionTest, ASessionOfAnotherEngineOpensButItsScanDoesNotResume) {
    const Scanned& full = scanned();
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    TempDir dir;
    // Cut where some candidates were delivered: in the evaluation.
    std::size_t cut = full.records.size();
    for (std::size_t k = 0; k < full.records.size(); ++k) {
        const std::optional<scan::ScanUpdate> update = scanUpdateOf(full.records[k]);
        if (update.has_value() && !update->candidates.empty()) {
            cut = k + 1;
            break;
        }
    }
    ASSERT_LT(cut, full.records.size());
    const std::vector<JournalRecord> records(full.records.begin(),
                                             full.records.begin() + static_cast<std::ptrdiff_t>(cut));
    const std::filesystem::path folder = dir / "older";
    test::writeJournal(folder, asEngine(records, "0.0.1"));
    {
        const std::unique_ptr<RecoverySession> session = opened(folder);
        ASSERT_NE(session, nullptr);
        const SessionInfo info = session->info();
        EXPECT_EQ(info.engineVersion, "0.0.1");
        EXPECT_FALSE(info.scan.runnable);
        EXPECT_NE(info.scan.notRunnable.find("0.0.1"), std::string::npos) << info.scan.notRunnable;
        EXPECT_NE(info.scan.notRunnable.find(std::string(kEngineVersion)), std::string::npos);
        const Bytes before = test::journalBytes(folder);
        const Result<scan::ScanSummary> refused = runScan(*session, source);
        ASSERT_FALSE(refused.ok());
        EXPECT_EQ(refused.error().code, ErrorCode::InvalidInput);
        EXPECT_EQ(test::journalBytes(folder), before);
        // Its candidates can be read and recovered.
        ASSERT_GT(session->candidateCount(), 0U);
        const std::vector<std::string> got = lines(*session);
        for (std::size_t i = 0; i < got.size(); ++i) {
            EXPECT_EQ(got[i], expectedDeep()[i]);
        }
        const Result<std::uint32_t> job = session->addRecoveryJob(dir / "out");
        RECOVERY_ASSERT_OK(job);
        const Result<scan::RecoveryJobSummary> recovered = session->runRecovery(*job, source);
        RECOVERY_ASSERT_OK(recovered);
        EXPECT_GT(recovered->metrics.recoveredFiles, 0U);
    }

    // A session another engine created whose scan never ran: this engine runs it.
    const std::filesystem::path fresh = dir / "fresh";
    test::writeJournal(fresh, asEngine({full.records.front()}, "0.0.1"));
    const std::unique_ptr<RecoverySession> session = opened(fresh);
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->info().scan.runnable);
    RECOVERY_ASSERT_OK(runScan(*session, source));
    expectExpectedCandidates(*session);
    EXPECT_EQ(session->info().scan.history.front().engineVersion, kEngineVersion);
    EXPECT_EQ(session->info().engineVersion, "0.0.1");
}

// ===========================================================================
// The source, the session, the operations
// ===========================================================================

TEST(RecoverySessionTest, OnlyTheSessionsSourceIsScannedOrRecovered) {
    TempDir dir;
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    const std::unique_ptr<RecoverySession> session = created(dir.path(), source);
    ASSERT_NE(session, nullptr);
    const auto refusedWith = [&](const Bytes& bytes, std::string_view why) {
        MemoryStorageSource other(bytes);
        RECOVERY_ASSERT_OK(other.open());
        const Result<scan::ScanSummary> summary = runScan(*session, other);
        ASSERT_FALSE(summary.ok()) << why;
        EXPECT_EQ(summary.error().code, ErrorCode::InvalidInput);
        EXPECT_NE(summary.error().message.find(why), std::string::npos) << summary.error().message;
    };
    // Another card of the same size.
    refusedWith(scan::test::makeCard(4096, 0xBEEF), "not the one the session was made for");
    // The same card, written to since (a byte of its boot region).
    Bytes written = card();
    written[512 + 488] ^= std::byte{1};
    refusedWith(written, "not the one the session was made for");
    // Another size.
    refusedWith(prefix(card(), card().size() - 512), "sectors of");
    // Nothing was recorded.
    EXPECT_FALSE(session->info().scan.state.has_value());
    EXPECT_EQ(session->info().scan.updates, 0U);
    // The session's own source scans.
    RECOVERY_ASSERT_OK(runScan(*session, source));
    // And recovers; another does not.
    const Result<std::uint32_t> job = session->addRecoveryJob(dir / "out");
    RECOVERY_ASSERT_OK(job);
    MemoryStorageSource other(scan::test::makeCard(4096, 0xBEEF));
    RECOVERY_ASSERT_OK(other.open());
    EXPECT_FALSE(session->runRecovery(*job, other).ok());
    EXPECT_FALSE(session->info().jobs[0].state.has_value());
}

// A scan goes on only with the formats it began with: a run with others is
// refused before anything is recorded.
TEST(RecoverySessionTest, AScanGoesOnOnlyWithWhatItBeganWith) {
    const Scanned& full = scanned();
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    TempDir dir;
    const std::filesystem::path folder = dir / "session";
    test::writeJournal(folder, prefix(full.journal, full.records[full.records.size() / 2].offset));
    const std::unique_ptr<RecoverySession> session = opened(folder);
    ASSERT_NE(session, nullptr);
    const Bytes before = test::journalBytes(folder);
    carving::FormatRegistry images;
    RECOVERY_ASSERT_OK(formats::registerImageFormats(images));
    const Result<scan::ScanSummary> other =
        session->runScan(source, images, scan::test::allMedia(), scan::test::testRunOptions());
    ASSERT_FALSE(other.ok());
    EXPECT_EQ(other.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(other.error().message.find("the carving formats"), std::string::npos) << other.error().message;
    EXPECT_EQ(test::journalBytes(folder), before);
    EXPECT_EQ(session->info().scan.state, SessionState::Started);
    RECOVERY_ASSERT_OK(runScan(*session, source));
    expectExpectedCandidates(*session);
}

TEST(RecoverySessionTest, ASessionIsOpenedByOneAtATime) {
    TempDir dir;
    const std::filesystem::path folder = dir / "session";
    test::writeJournal(folder, scanned().journal);
    {
        const std::unique_ptr<RecoverySession> first = opened(folder);
        ASSERT_NE(first, nullptr);
        const Result<std::unique_ptr<RecoverySession>> second = RecoverySession::open(folder);
        ASSERT_FALSE(second.ok());
        EXPECT_EQ(second.error().code, ErrorCode::DestinationError);
        EXPECT_NE(second.error().message.find("in use"), std::string::npos) << second.error().message;
        // Read without opening it.
        const Result<SessionSummary> summary = readSessionSummary(folder);
        RECOVERY_ASSERT_OK(summary);
        EXPECT_FALSE(summary->error.has_value());
        EXPECT_EQ(summary->id, first->id());
    }
    EXPECT_NE(opened(folder), nullptr);
}

TEST(RecoverySessionTest, DestroyingASessionCancelsItsScanAndWaits) {
    TempDir dir;
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    std::unique_ptr<RecoverySession> session = created(dir.path(), source);
    ASSERT_NE(session, nullptr);
    const std::filesystem::path folder = session->folder();
    std::atomic<bool> paused{false};
    scan::ScanRunOptions options = scan::test::testRunOptions();
    pauseInThePass(options, *session, 256 * kKiB, paused);
    Result<scan::ScanSummary> summary = makeError(ErrorCode::InternalError, "not run");
    RecoverySession* raw = session.get();
    std::thread scanning([&summary, raw, &source, &options] { summary = runScan(*raw, source, options); });
    ASSERT_TRUE(waitFor([&] { return paused.load() && raw->progress().paused; }));
    session.reset();
    scanning.join();
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, scan::ScanOutcome::Cancelled);
    const std::unique_ptr<RecoverySession> again = opened(folder);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->info().scan.state, SessionState::Cancelled);
    EXPECT_TRUE(again->info().scan.runnable);
}

class NoPlayback final : public validation::IPlayabilityChecker {
public:
    Result<validation::LevelResult> check(carving::IContentReader&, std::string_view, std::string_view) override {
        validation::LevelResult result;
        result.status = validation::LevelStatus::Unsupported;
        result.checker = "none";
        return result;
    }
};

TEST(RecoverySessionTest, ThePlayabilityCheckerIsGivenToEachRun) {
    TempDir dir;
    MemoryStorageSource source(card());
    RECOVERY_ASSERT_OK(source.open());
    NoPlayback checker;
    scan::ScanConfiguration configuration;
    configuration.mode = ScanMode::Quick;
    configuration.playability = &checker;
    const std::unique_ptr<RecoverySession> session = created(dir.path(), source, configuration);
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->info().playability);
    EXPECT_EQ(session->info().configuration.playability, nullptr);
    const Result<scan::ScanSummary> without = runScan(*session, source);
    ASSERT_FALSE(without.ok());
    EXPECT_NE(without.error().message.find("needs a checker"), std::string::npos);
    const Result<scan::ScanSummary> with =
        session->runScan(source, scan::test::allFormats(), scan::test::allMedia(), scan::test::testRunOptions(),
                         &checker);
    RECOVERY_ASSERT_OK(with);
    EXPECT_EQ(with->outcome, scan::ScanOutcome::Completed);
    for (const evaluation::EvaluatedCandidate& candidate : session->candidates()) {
        if (candidate.validation.playability.status != validation::LevelStatus::NotRun) {
            EXPECT_EQ(candidate.validation.playability.checker, "none");
        }
    }
}

// ===========================================================================
// Errors, unreadable regions, listing
// ===========================================================================

TEST(RecoverySessionTest, ErrorsAndUnreadableRegionsAreKept) {
    TempDir dir;
    const std::uint64_t sectors = card().size() / 512;
    MemoryStorageSource failing(card());
    // Unreadable sectors in the data area, and one whose read fails for good
    // (not an I/O error): the scan stops there.
    failing.addBadSector(sectors / 2);
    failing.addBadSector(sectors / 2 + 1);
    failing.addFatalSector(sectors * 3 / 4);
    RECOVERY_ASSERT_OK(failing.open());
    std::filesystem::path folder;
    {
        const std::unique_ptr<RecoverySession> session = created(dir / "sessions", failing);
        ASSERT_NE(session, nullptr);
        folder = session->folder();
        const Result<scan::ScanSummary> summary = runScan(*session, failing);
        ASSERT_FALSE(summary.ok());
        EXPECT_EQ(session->info().scan.state, SessionState::Failed);
    }
    {
        const std::unique_ptr<RecoverySession> session = opened(folder);
        ASSERT_NE(session, nullptr);
        const SessionInfo info = session->info();
        EXPECT_EQ(info.scan.state, SessionState::Failed);
        ASSERT_TRUE(info.scan.history.back().error.has_value());
        const std::vector<SessionError> errors = session->errors();
        ASSERT_FALSE(errors.empty());
        EXPECT_EQ(errors.back().context, "scan");
        EXPECT_EQ(errors.back().time, info.scan.history.back().time);
        EXPECT_TRUE(info.scan.runnable);
    }
    // Resumed once the source reads again (the bad sectors stay bad).
    MemoryStorageSource healthier(card());
    healthier.addBadSector(sectors / 2);
    healthier.addBadSector(sectors / 2 + 1);
    RECOVERY_ASSERT_OK(healthier.open());
    {
        const std::unique_ptr<RecoverySession> session = opened(folder);
        ASSERT_NE(session, nullptr);
        RECOVERY_ASSERT_OK(runScan(*session, healthier));
        EXPECT_EQ(session->info().scan.state, SessionState::Completed);
        // A job whose destination cannot be used: a file is in its place.
        ::recovery::test::writeFile(dir / "blocked", Bytes(10, std::byte{1}));
        const Result<std::uint32_t> job = session->addRecoveryJob(dir / "blocked");
        RECOVERY_ASSERT_OK(job);
        EXPECT_FALSE(session->runRecovery(*job, healthier).ok());
        EXPECT_EQ(session->info().jobs[0].state, SessionState::Failed);
    }
    const std::unique_ptr<RecoverySession> session = opened(folder);
    ASSERT_NE(session, nullptr);
    const std::vector<storage::BadRegion> unreadable = session->unreadableRegions();
    ASSERT_EQ(unreadable.size(), 1U);
    EXPECT_EQ(unreadable[0].offset, sectors / 2 * 512);
    EXPECT_EQ(unreadable[0].length, 1024U);
    EXPECT_EQ(session->info().scan.metrics.unreadableBytes, 1024U);
    std::set<std::string> contexts;
    for (const SessionError& error : session->errors()) {
        contexts.insert(error.context);
    }
    EXPECT_TRUE(contexts.contains("scan"));
    EXPECT_TRUE(contexts.contains("recovery job 1"));
}

TEST(RecoverySessionTest, SessionsAreListedWithoutOpeningThem) {
    const Scanned& full = scanned();
    TempDir dir;
    test::writeJournal(dir / "a-complete", full.journal);
    test::writeJournal(dir / "b-cut", prefix(full.journal, full.records[full.records.size() / 2].offset));
    Bytes newer = encodeJournalHeader(kJournalFormatVersion + 1);
    test::writeJournal(dir / "c-newer", newer);
    std::filesystem::create_directories(dir / "d-empty");
    ::recovery::test::writeFile(dir / "e-file", Bytes(3, std::byte{0}));

    const Result<std::vector<SessionSummary>> listed = listSessions(dir.path());
    RECOVERY_ASSERT_OK(listed);
    ASSERT_EQ(listed->size(), 3U);
    const SessionSummary& complete = (*listed)[0];
    EXPECT_EQ(toUtf8(complete.folder.filename()), "a-complete");
    EXPECT_FALSE(complete.error.has_value());
    EXPECT_EQ(complete.state, SessionState::Completed);
    EXPECT_EQ(complete.stage, scan::ScanStage::Completed);
    EXPECT_EQ(complete.mode, ScanMode::Deep);
    EXPECT_EQ(complete.sourcePath, "memory://test");
    EXPECT_EQ(complete.sourceSize, card().size());
    EXPECT_EQ(complete.engineVersion, kEngineVersion);
    EXPECT_EQ(complete.formatVersion, kJournalFormatVersion);
    EXPECT_LE(complete.created, complete.updated);
    EXPECT_EQ(complete.metrics.candidates, expectedDeep().size());
    const SessionSummary& cut = (*listed)[1];
    EXPECT_FALSE(cut.error.has_value());
    EXPECT_EQ(cut.state, SessionState::Started);
    EXPECT_EQ(cut.id, complete.id);
    EXPECT_TRUE((*listed)[2].error.has_value());

    const Result<std::vector<SessionSummary>> none = listSessions(dir / "missing");
    RECOVERY_ASSERT_OK(none);
    EXPECT_TRUE(none->empty());
}

}  // namespace
}  // namespace recovery::session
