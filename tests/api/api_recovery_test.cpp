// Recovery through the API (P19): every file written and told once, byte
// for byte; recovery converging on its folder (nothing twice unless asked);
// single candidates and filters; requests refused before anything is
// recorded; and the recovery state the list shows agrees with the engine's
// own index.

#include "api_test_support.hpp"

#include "metadata/recovery_status.hpp"
#include "session/recovery_session.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <set>

namespace recovery::api::test {
namespace {

class ApiRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        id_ = world_.startCardScan();
        ASSERT_EQ(world_.waitIdle(id_).state, OperationState::Completed);
    }

    [[nodiscard]] RecoveryOptions to(std::string_view folder) const {
        RecoveryOptions options;
        options.destination = world_.folder() / folder;
        return options;
    }

    ApiWorld world_;
    std::string id_;
};

TEST_F(ApiRecoveryTest, RecoverAllWritesEveryFileAndTellsEachOnce) {
    const Result<RecoveryStart> start = world_.api().recoverAll(id_, to("out"));
    RECOVERY_ASSERT_OK(start);
    EXPECT_EQ(start->requested, expectedCard().size());
    EXPECT_EQ(start->alreadyRecovered, 0u);
    ASSERT_TRUE(start->newJob.has_value());
    EXPECT_EQ(start->jobs, std::vector<std::uint32_t>{*start->newJob});
    const std::optional<Event> finished = world_.log().waitForFinished(id_, 2);
    ASSERT_TRUE(finished.has_value());
    EXPECT_EQ(finished->progress.operation, OperationKind::Recovery);
    EXPECT_EQ(finished->progress.state, OperationState::Completed);
    ASSERT_TRUE(finished->progress.recovery.has_value());
    EXPECT_EQ(finished->progress.recovery->fraction, 1.0);
    EXPECT_EQ(finished->progress.recovery->metrics.recovered, expectedCard().size());

    // Every candidate is told once, with the file written.
    std::set<std::uint64_t> told;
    for (const Event& event : world_.log().eventsOf(id_)) {
        for (const RecoveredFile& file : event.files) {
            EXPECT_TRUE(told.insert(file.candidate.value).second) << "candidate " << file.candidate.value;
            EXPECT_EQ(file.job, *start->newJob);
            ASSERT_TRUE(file.file.has_value()) << (file.error ? describe(*file.error) : "");
            EXPECT_TRUE(std::filesystem::exists(*file.file));
            EXPECT_EQ(std::filesystem::file_size(*file.file), file.size);
        }
    }
    EXPECT_EQ(told.size(), expectedCard().size());

    for (const CandidateInfo& candidate : allCandidates(world_.api(), id_)) {
        EXPECT_EQ(candidate.recovery, RecoveryState::Recovered) << candidate.name;
        EXPECT_TRUE(std::filesystem::exists(candidate.recoveredFile)) << candidate.name;
        EXPECT_EQ(candidate.recoveredComplete, candidate.unreadableBytes == 0) << candidate.name;
    }
    const std::map<std::string, Bytes> files = ::recovery::test::filesBelow(world_.folder() / "out");
    for (const auto& [name, bytes] : ::recovery::test::cardOriginals()) {
        ASSERT_TRUE(files.contains(name)) << name;
        EXPECT_EQ(files.at(name), bytes) << name;
    }
}

TEST_F(ApiRecoveryTest, RecoveringToTheSameFolderAgainWritesNothingTwice) {
    RECOVERY_ASSERT_OK(world_.api().recoverAll(id_, to("out")));
    ASSERT_EQ(world_.waitIdle(id_).state, OperationState::Completed);

    const Result<RecoveryStart> again = world_.api().recoverAll(id_, to("out"));
    RECOVERY_ASSERT_OK(again);
    EXPECT_TRUE(again->jobs.empty()) << "nothing to do: no operation";
    EXPECT_EQ(again->alreadyRecovered, expectedCard().size());
    const Result<Progress> idle = world_.api().getProgress(id_);
    RECOVERY_ASSERT_OK(idle);
    EXPECT_EQ(idle->state, OperationState::Completed);

    // The same folder spelt otherwise is the same folder.
    RecoveryOptions spelt = to("out");
    spelt.destination = world_.folder() / "OUT" / "." / "";
    const Result<RecoveryStart> same = world_.api().recoverAll(id_, spelt);
    RECOVERY_ASSERT_OK(same);
    EXPECT_TRUE(same->jobs.empty());

    // Asked to, it writes them again under new names.
    RecoveryOptions twice = to("out");
    twice.again = true;
    const Result<RecoveryStart> copies = world_.api().recoverAll(id_, twice);
    RECOVERY_ASSERT_OK(copies);
    ASSERT_TRUE(copies->newJob.has_value());
    ASSERT_EQ(world_.waitIdle(id_).state, OperationState::Completed);
    const std::map<std::string, Bytes> files = ::recovery::test::filesBelow(world_.folder() / "out");
    EXPECT_EQ(files.size(), 2 * expectedCard().size());
    EXPECT_TRUE(files.contains("PHOTO (1).JPG"));

    // Another folder gets every file.
    const Result<RecoveryStart> elsewhere = world_.api().recoverAll(id_, to("elsewhere"));
    RECOVERY_ASSERT_OK(elsewhere);
    EXPECT_EQ(elsewhere->alreadyRecovered, 0u);
    ASSERT_EQ(world_.waitIdle(id_).state, OperationState::Completed);
    EXPECT_EQ(::recovery::test::filesBelow(world_.folder() / "elsewhere").size(), expectedCard().size());
}

TEST_F(ApiRecoveryTest, OneCandidateOrAFilteredSelection) {
    const CandidateInfo photo = candidateNamed(world_.api(), id_, "PHOTO.JPG");
    const Result<RecoveryStart> one = world_.api().recoverCandidate(id_, photo.id, to("one"));
    RECOVERY_ASSERT_OK(one);
    EXPECT_EQ(one->requested, 1u);
    ASSERT_EQ(world_.waitIdle(id_).state, OperationState::Completed);
    const std::map<std::string, Bytes> single = ::recovery::test::filesBelow(world_.folder() / "one");
    ASSERT_EQ(single.size(), 1u);
    EXPECT_EQ(single.begin()->second, ::recovery::test::cardOriginals().at("PHOTO.JPG"));

    // Images without duplicates: COPY.JPG and the carved copy are left out.
    CandidateFilter filter;
    filter.kinds = {MediaKind::Image};
    filter.skipDuplicates = true;
    const Result<RecoveryStart> images = world_.api().recoverAll(id_, to("images"), filter);
    RECOVERY_ASSERT_OK(images);
    ASSERT_EQ(world_.waitIdle(id_).state, OperationState::Completed);
    std::uint64_t expected = 0;
    for (const CandidateInfo& candidate : allCandidates(world_.api(), id_)) {
        expected += candidate.kind == MediaKind::Image && !candidate.duplicateOf.has_value() ? 1 : 0;
    }
    EXPECT_EQ(images->requested, expected);
    const std::map<std::string, Bytes> written = ::recovery::test::filesBelow(world_.folder() / "images");
    EXPECT_EQ(written.size(), expected);
    EXPECT_FALSE(written.contains("COPY.JPG"));
    EXPECT_TRUE(written.contains("PHOTO.JPG"));

    // The list filters the same way.
    CandidateQuery query;
    query.filter.recovery = {RecoveryState::Recovered};
    const Result<CandidatePage> recovered = world_.api().getCandidates(id_, query);
    RECOVERY_ASSERT_OK(recovered);
    EXPECT_EQ(recovered->matching, expected);
    query.filter = CandidateFilter{};
    query.filter.deleted = true;
    const Result<CandidatePage> deleted = world_.api().getCandidates(id_, query);
    RECOVERY_ASSERT_OK(deleted);
    EXPECT_GE(deleted->matching, 2u);
    for (const CandidateInfo& candidate : deleted->candidates) {
        EXPECT_TRUE(candidate.deleted);
    }
    query.first = deleted->matching;
    const Result<CandidatePage> beyond = world_.api().getCandidates(id_, query);
    RECOVERY_ASSERT_OK(beyond);
    EXPECT_TRUE(beyond->candidates.empty());
}

TEST_F(ApiRecoveryTest, RequestsThatCannotBeDoneAreRefusedBeforeAnythingIsRecorded) {
    RECOVERY_EXPECT_ERROR(world_.api().recoverCandidate(id_, CandidateId{999}, to("out")), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world_.api().recoverCandidate(id_, CandidateId{0}, to("out")), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world_.api().recoverCandidates(id_, {}, to("out")), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world_.api().recoverAll(id_, RecoveryOptions{}), ErrorCode::InvalidInput);
    ::recovery::test::writeFile(world_.folder() / "a-file", Bytes(4));
    RECOVERY_EXPECT_ERROR(world_.api().recoverAll(id_, to("a-file")), ErrorCode::DestinationError);
    RECOVERY_EXPECT_ERROR(world_.api().recoverAll("no-such-session", to("out")), ErrorCode::InvalidInput);
    // The source must be the session's: here it changed.
    {
        std::vector<std::byte> bytes = ::recovery::test::readFile(world_.cardImage());
        bytes[0] = std::byte{0x00};
        bytes[1] ^= std::byte{0xFF};
        ::recovery::test::writeFile(world_.cardImage(), bytes);
    }
    const Result<RecoveryStart> changed = world_.api().recoverAll(id_, to("out"));
    ASSERT_FALSE(changed.ok());
    EXPECT_NE(changed.error().message.find("must be attached and unchanged"), std::string::npos)
        << describe(changed.error());

    const Result<SessionDetails> details = world_.api().getSession(id_);
    RECOVERY_ASSERT_OK(details);
    EXPECT_TRUE(details->jobs.empty());
    const Result<Progress> progress = world_.api().getProgress(id_);
    RECOVERY_ASSERT_OK(progress);
    EXPECT_EQ(progress->operation, OperationKind::Scan) << "a refused request leaves the last operation's state";
    EXPECT_FALSE(std::filesystem::exists(world_.folder() / "out"));
}

TEST_F(ApiRecoveryTest, TheListsRecoveryStateAgreesWithTheEnginesIndex) {
    // A job of two files, and a job of every file cancelled at once.
    const CandidateInfo photo = candidateNamed(world_.api(), id_, "PHOTO.JPG");
    const CandidateInfo picture = candidateNamed(world_.api(), id_, "PICTURE.PNG");
    const CandidateId pair[] = {photo.id, picture.id};
    RECOVERY_ASSERT_OK(world_.api().recoverCandidates(id_, pair, to("a")));
    ASSERT_EQ(world_.waitIdle(id_).state, OperationState::Completed);
    RECOVERY_ASSERT_OK(world_.api().recoverAll(id_, to("b")));
    RECOVERY_ASSERT_OK(world_.api().cancelRecovery(id_));
    (void)world_.waitIdle(id_);
    const std::vector<CandidateInfo> listed = allCandidates(world_.api(), id_);
    world_.restart();

    Result<std::unique_ptr<session::RecoverySession>> session =
        session::RecoverySession::open(world_.sessions() / id_);
    RECOVERY_ASSERT_OK(session);
    const metadata::RecoveryJobIndex index = metadata::RecoveryJobIndex::fromSession(**session);
    ASSERT_EQ(listed.size(), (*session)->candidateCount());
    for (const CandidateInfo& candidate : listed) {
        const metadata::CandidateRecovery engine =
            index.recoveryOf(evaluation::EvaluatedCandidateId{candidate.id.value});
        const RecoveryState expected = engine.state == metadata::RecoveryState::Recovered ? RecoveryState::Recovered
                                       : engine.state == metadata::RecoveryState::Pending ? RecoveryState::Pending
                                       : engine.state == metadata::RecoveryState::Failed  ? RecoveryState::Failed
                                                                                          : RecoveryState::NotRecovered;
        EXPECT_EQ(candidate.recovery, expected) << candidate.name;
        EXPECT_EQ(candidate.recoveredComplete, engine.complete) << candidate.name;
    }
}

TEST(ApiRecoveryRefusalTest, AScanThatDeliveredNothingYetHasNothingToRecover) {
    std::atomic<bool> cancelled{false};
    RecoveryApi* api = nullptr;
    ApiWorld world;
    world.hooks().onOperationProgress = [&](std::string_view session, const Progress& progress) {
        if (progress.scan.has_value() && !cancelled.exchange(true)) {
            EXPECT_TRUE(api->cancelScan(session).ok());
        }
    };
    api = &world.api();
    const std::string id = world.startCardScan();
    ASSERT_EQ(world.waitIdle(id).state, OperationState::Cancelled);
    RecoveryOptions options;
    options.destination = world.folder() / "out";
    const Result<RecoveryStart> nothing = api->recoverAll(id, options);
    RECOVERY_EXPECT_ERROR(nothing, ErrorCode::InvalidInput);
}

TEST(ApiRecoveryRefusalTest, NothingIsWrittenOnTheSourceDisk) {
    ApiWorld world;
    const std::uint32_t disk = world.addDisk(::recovery::test::readFile(world.cardImage()));
    // A folder of a volume on the disk scanned.
    std::filesystem::create_directories(world.folder() / "on-the-disk");
    world.placeOn(world.folder() / "on-the-disk", disk);
    const Result<std::string> id = world.api().startScan(SourceRef::physicalDisk(disk));
    RECOVERY_ASSERT_OK(id);
    ASSERT_EQ(world.waitIdle(*id).state, OperationState::Completed);
    RecoveryOptions options;
    options.destination = world.folder() / "on-the-disk" / "out";
    RECOVERY_EXPECT_ERROR(world.api().recoverAll(*id, options), ErrorCode::DestinationError);
    EXPECT_FALSE(std::filesystem::exists(options.destination));
    const Result<SessionDetails> details = world.api().getSession(*id);
    RECOVERY_ASSERT_OK(details);
    EXPECT_TRUE(details->jobs.empty());
}

}  // namespace
}  // namespace recovery::api::test
