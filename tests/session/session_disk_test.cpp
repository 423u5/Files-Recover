// A session's physical disk under another number (L147): Windows numbers
// disks as they are attached, so a card reader plugged in again, or found
// after a restart, may be another \\.\PhysicalDriveN. The session knows its
// disk by its device (the serial number, else vendor and product; the user's
// choice of 2026-10-16: the device must be the same, not only the content)
// and its content (the fingerprint), finds it under its new number, records
// that, and resumes its scan and recovers from it there. Files written while
// the disk is not opened stay off every disk that may be it.
//
// The disks are simulated (support/disk_rack.hpp): no real device is opened.

#include "session/recovery_session.hpp"
#include "session/session_types.hpp"

#include "scan/scan_test_support.hpp"
#include "session_test_support.hpp"
#include "storage/disk_image_source.hpp"
#include "storage/physical_disk_source.hpp"
#include "support/disk_rack.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace recovery::session {
namespace {

using test::Bytes;
using ::recovery::test::DiskRack;
using ::recovery::test::FakeDeviceConfig;
using ::recovery::test::TempDir;

const Bytes& card() {
    static const Bytes bytes = scan::test::makeCard();
    return bytes;
}

// Another card of the card's size.
const Bytes& otherCard() {
    static const Bytes bytes = scan::test::makeCard(4096, 0xBEEF);
    return bytes;
}

const std::vector<std::string>& expectedDeep() {
    static const std::vector<std::string> expected = [] {
        ::recovery::test::MemoryStorageSource source(card());
        const bool opened = source.open().ok();
        return opened ? scan::test::describe(scan::test::referenceScan(source, ScanMode::Deep))
                      : std::vector<std::string>{};
    }();
    return expected;
}

// Every destination is on disk 0, never one of the rack's.
storage::DiskResolver systemDisk() {
    return [](const std::filesystem::path&) -> Result<std::vector<std::uint32_t>> {
        return std::vector<std::uint32_t>{0};
    };
}

SessionOptions sessionOptions() {
    SessionOptions options;
    options.diskResolver = systemDisk();
    return options;
}

std::string path(std::uint32_t disk) {
    return "\\\\.\\PhysicalDrive" + std::to_string(disk);
}

// Disk `number` of `rack`, open.
std::unique_ptr<storage::PhysicalDiskSource> openDisk(const std::shared_ptr<DiskRack>& rack, std::uint32_t number) {
    auto disk = std::make_unique<storage::PhysicalDiskSource>(number, rack);
    EXPECT_TRUE(disk->open().ok()) << "disk " << number;
    return disk;
}

// What a session created on disk `number` of `rack` records of it.
SessionSource recordedOf(const std::shared_ptr<DiskRack>& rack, std::uint32_t number) {
    const std::unique_ptr<storage::PhysicalDiskSource> disk = openDisk(rack, number);
    Result<SessionSource> described = describeSource(*disk);
    EXPECT_TRUE(described.ok());
    return described.ok() ? std::move(described).value() : SessionSource{};
}

SessionDiskOpener openerOf(const std::shared_ptr<DiskRack>& rack) {
    return [rack](std::uint32_t number) -> Result<std::unique_ptr<storage::IStorageSource>> {
        auto disk = std::make_unique<storage::PhysicalDiskSource>(number, rack);
        if (Status opened = disk->open(); !opened.ok()) {
            return opened.error();
        }
        return std::unique_ptr<storage::IStorageSource>(std::move(disk));
    };
}

// A copy of `config` (another device holding the same, or the same device
// written to) with `serial`.
std::shared_ptr<FakeDeviceConfig> copyOf(const std::shared_ptr<FakeDeviceConfig>& config, std::string serial) {
    auto copy = std::make_shared<FakeDeviceConfig>(*config);
    copy->description.serialNumber = std::move(serial);
    return copy;
}

Result<scan::ScanSummary> runScan(RecoverySession& session, storage::IStorageSource& source,
                                  scan::ScanRunOptions options = scan::test::testRunOptions()) {
    return session.runScan(source, scan::test::allFormats(), scan::test::allMedia(), std::move(options));
}

std::vector<std::uint32_t> numbers(const std::vector<storage::SourceInfo>& sources) {
    std::vector<std::uint32_t> found;
    for (const storage::SourceInfo& source : sources) {
        found.push_back(source.diskNumber.value_or(9999));
    }
    std::sort(found.begin(), found.end());
    return found;
}

// ===========================================================================
// The same disk
// ===========================================================================

TEST(SessionDiskTest, TheSameDeviceWithTheSameContentIsTheSessionsDiskUnderAnyNumber) {
    auto rack = std::make_shared<DiskRack>();
    const std::shared_ptr<FakeDeviceConfig> config = rack->add(7, card(), {}, "SER-A");
    const SessionSource recorded = recordedOf(rack, 7);
    EXPECT_EQ(recorded.path, path(7));
    EXPECT_EQ(recorded.diskNumber, 7U);
    EXPECT_EQ(recorded.serialNumber, "SER-A");

    // Under its number, and under another.
    RECOVERY_EXPECT_OK(checkSameSource(recorded, *openDisk(rack, 7)));
    rack->renumber(7, 9);
    RECOVERY_EXPECT_OK(checkSameSource(recorded, *openDisk(rack, 9)));

    const auto refused = [&](const std::shared_ptr<FakeDeviceConfig>& disk, const SessionSource& session,
                             std::string_view why) {
        rack->attach(11, disk);
        const Status same = checkSameSource(session, *openDisk(rack, 11));
        ASSERT_FALSE(same.ok()) << why;
        EXPECT_EQ(same.error().code, ErrorCode::InvalidInput);
        EXPECT_NE(same.error().message.find(why), std::string::npos) << same.error().message;
    };
    // The same content in another device (a card in another reader, a copy).
    refused(copyOf(config, "SER-B"), recorded,
            "another device than the session's: its serial number is 'SER-B', the session's disk's 'SER-A'");
    refused(copyOf(config, ""), recorded, "it reports no serial number, the session's disk 'SER-A'");
    // The same device, written to since.
    auto written = std::make_shared<FakeDeviceConfig>(*config);
    written->data[512 + 488] ^= std::byte{1};
    refused(written, recorded, "not the one the session was made for, or it has changed since");

    // A session that recorded no serial number (an older one, or a device
    // that reports none): the vendor and product say which device.
    SessionSource older = recorded;
    older.serialNumber.clear();
    RECOVERY_EXPECT_OK(checkSameSource(older, *openDisk(rack, 9)));
    auto otherModel = copyOf(config, "SER-A");
    otherModel->description.product = "Other Reader";
    refused(otherModel, older, "it is a 'Simulated Other Reader', the session's disk a 'Simulated Card Reader'");

    // An image file is known by its path.
    SessionSource image = recorded;
    image.type = storage::SourceType::DiskImage;
    image.path = "E:\\card.img";
    TempDir dir;
    const std::filesystem::path file = dir / "card.img";
    ::recovery::test::writeFile(file, card());
    storage::DiskImageSource moved(file);
    RECOVERY_ASSERT_OK(moved.open());
    const Status other = checkSameSource(image, moved);
    ASSERT_FALSE(other.ok());
    EXPECT_NE(other.error().message.find("the session's is 'E:\\card.img'"), std::string::npos);
}

TEST(SessionDiskTest, TheSessionsDiskIsFoundUnderItsNumberOrAnother) {
    auto rack = std::make_shared<DiskRack>();
    const std::shared_ptr<FakeDeviceConfig> config = rack->add(7, card(), {}, "SER-A");
    const SessionSource recorded = recordedOf(rack, 7);
    const SessionDiskOpener open = openerOf(rack);

    // Under its own number: nothing else is opened.
    {
        const std::size_t before = rack->opened().size();
        Result<FoundSessionDisk> found = findSessionDisk(recorded, open, rack->lister());
        RECOVERY_ASSERT_OK(found);
        EXPECT_EQ(found->number, 7U);
        EXPECT_FALSE(found->renumbered);
        EXPECT_EQ(rack->opened().size(), before + 1);
    }

    // Plugged in again: another card took its number, and a reader of
    // another serial number holds a card of the same size.
    rack->renumber(7, 9);
    rack->add(7, otherCard(), {}, "SER-C");
    rack->add(8, otherCard(), {}, "SER-D");
    {
        Result<FoundSessionDisk> found = findSessionDisk(recorded, open, rack->lister());
        RECOVERY_ASSERT_OK(found);
        EXPECT_EQ(found->number, 9U);
        EXPECT_TRUE(found->renumbered);
        EXPECT_EQ(found->source->getInfo().path, std::filesystem::path(path(9)));
        // Disk 8 is another device: it is not opened.
        const std::vector<std::uint32_t> opened = rack->opened();
        EXPECT_EQ(std::count(opened.begin(), opened.end(), 8U), 0);
    }

    // Not attached: every disk tried says why it is not the session's.
    rack->detach(9);
    {
        const Result<FoundSessionDisk> found = findSessionDisk(recorded, open, rack->lister());
        ASSERT_FALSE(found.ok());
        EXPECT_EQ(found.error().code, ErrorCode::InvalidInput);
        const std::string& message = found.error().message;
        EXPECT_NE(message.find("the session's disk, disk 7 when last used (Simulated Card Reader, serial SER-A, " +
                               std::to_string(card().size()) + " bytes), is not attached, or it has changed"),
                  std::string::npos)
            << message;
        EXPECT_NE(message.find("disk 7: the disk is another device than the session's: its serial number is "
                               "'SER-C'"),
                  std::string::npos)
            << message;
        EXPECT_NE(message.find("disk 8: another device: its serial number is 'SER-D'"), std::string::npos)
            << message;
    }

    // Twice the same device with the same content: which is which cannot be told.
    rack->attach(9, config);
    rack->attach(12, config);
    {
        const Result<FoundSessionDisk> found = findSessionDisk(recorded, open, rack->lister());
        ASSERT_FALSE(found.ok());
        EXPECT_NE(found.error().message.find("is attached 2 times: disks 9, 12"), std::string::npos)
            << found.error().message;
    }
    rack->detach(12);

    // Without administrator rights, no disk can be opened: said at once.
    rack->failOpen(7, makeError(ErrorCode::IoError, "access denied", 5));
    {
        const Result<FoundSessionDisk> found = findSessionDisk(recorded, open, rack->lister());
        ASSERT_FALSE(found.ok());
        EXPECT_EQ(found.error().systemErrorCode, 5U);
        EXPECT_EQ(found.error().message, "access denied");
    }

    // The disks cannot be listed: the recorded number alone is tried.
    rack->failOpen(7, makeError(ErrorCode::IoError, "no such disk", 2));
    const storage::DiskLister broken = []() -> Result<std::vector<storage::AttachedDisk>> {
        return makeError(ErrorCode::IoError, "the system cannot be asked");
    };
    {
        const Result<FoundSessionDisk> found = findSessionDisk(recorded, open, broken);
        ASSERT_FALSE(found.ok());
        EXPECT_NE(found.error().message.find("the disks attached cannot be listed to look for it under another "
                                             "number: the system cannot be asked"),
                  std::string::npos)
            << found.error().message;
    }
    // With the list, it is found again.
    RECOVERY_ASSERT_OK(findSessionDisk(recorded, open, rack->lister()));
}

TEST(SessionDiskTest, WhatIsWrittenWithoutTheDiskStaysOffEveryDiskThatMayBeIt) {
    auto rack = std::make_shared<DiskRack>();
    const std::shared_ptr<FakeDeviceConfig> config = rack->add(7, card(), {}, "SER-A");
    const SessionSource recorded = recordedOf(rack, 7);

    // Disk 7 is another device now; the session's may be 9 (its device, its
    // size) or 10 (its device, its size not told); 8 is another device.
    rack->renumber(7, 9);
    rack->add(7, card(), {}, "SER-C");
    rack->add(8, card(), {}, "SER-D");
    auto unsized = copyOf(config, "SER-A");
    unsized->geometry.sizeBytes = 0;
    rack->attach(10, unsized);
    EXPECT_EQ(numbers(possibleSessionSources(recorded, rack->lister())), (std::vector<std::uint32_t>{9, 10}));

    // The recorded number is not listed: it is kept too.
    rack->detach(7);
    EXPECT_EQ(numbers(possibleSessionSources(recorded, rack->lister())), (std::vector<std::uint32_t>{7, 9, 10}));
    // The disks cannot be listed: the recorded disk alone.
    const storage::DiskLister broken = []() -> Result<std::vector<storage::AttachedDisk>> {
        return makeError(ErrorCode::IoError, "the system cannot be asked");
    };
    EXPECT_EQ(numbers(possibleSessionSources(recorded, broken)), (std::vector<std::uint32_t>{7}));

    // An image file: itself.
    SessionSource image;
    image.type = storage::SourceType::DiskImage;
    image.path = "E:\\card.img";
    const std::vector<storage::SourceInfo> sources = possibleSessionSources(image, rack->lister());
    ASSERT_EQ(sources.size(), 1U);
    EXPECT_EQ(sources[0].type, storage::SourceType::DiskImage);
    EXPECT_EQ(sources[0].path, std::filesystem::path("E:\\card.img"));
}

// ===========================================================================
// A session on the disk under another number
// ===========================================================================

TEST(SessionDiskTest, AScanResumesAndRecoversOnTheDiskUnderItsNewNumber) {
    auto rack = std::make_shared<DiskRack>();
    const std::shared_ptr<FakeDeviceConfig> config = rack->add(7, card(), {}, "SER-A");
    TempDir dir;
    std::filesystem::path folder;
    {
        const std::unique_ptr<storage::PhysicalDiskSource> disk = openDisk(rack, 7);
        Result<std::unique_ptr<RecoverySession>> created =
            RecoverySession::create(dir / "sessions", *disk, {}, sessionOptions());
        RECOVERY_ASSERT_OK(created);
        RecoverySession& session = **created;
        folder = session.folder();
        // Stopped in the source pass.
        scan::ScanRunOptions options = scan::test::testRunOptions();
        options.onProgress = [&session](const scan::ScanProgress& progress) {
            if (progress.stage == scan::ScanStage::SourcePass) {
                session.cancel();
            }
        };
        const Result<scan::ScanSummary> stopped = runScan(session, *disk, std::move(options));
        RECOVERY_ASSERT_OK(stopped);
        ASSERT_EQ(stopped->outcome, scan::ScanOutcome::Cancelled);
    }

    // Plugged in again as disk 9; disk 7 is another card now.
    rack->renumber(7, 9);
    rack->add(7, otherCard(), {}, "SER-C");
    {
        Result<std::unique_ptr<RecoverySession>> reopened = RecoverySession::open(folder, sessionOptions());
        RECOVERY_ASSERT_OK(reopened);
        RecoverySession& session = **reopened;
        EXPECT_EQ(session.info().source.path, path(7));
        // Disk 7 is not the session's: refused, nothing recorded.
        const Bytes before = test::journalBytes(folder);
        const Result<scan::ScanSummary> refused = runScan(session, *openDisk(rack, 7));
        ASSERT_FALSE(refused.ok());
        EXPECT_EQ(refused.error().code, ErrorCode::InvalidInput);
        EXPECT_EQ(test::journalBytes(folder), before);

        const std::unique_ptr<storage::PhysicalDiskSource> disk = openDisk(rack, 9);
        const Result<scan::ScanSummary> resumed = runScan(session, *disk);
        RECOVERY_ASSERT_OK(resumed);
        EXPECT_EQ(resumed->outcome, scan::ScanOutcome::Completed);
        EXPECT_EQ(scan::test::describe(session.candidates()), expectedDeep());
        const SessionInfo info = session.info();
        EXPECT_EQ(info.source.path, path(9));
        EXPECT_EQ(info.source.diskNumber, 9U);
        EXPECT_EQ(info.source.serialNumber, "SER-A");

        // Recovered from it there.
        const Result<std::uint32_t> job = session.addRecoveryJob(dir / "out");
        RECOVERY_ASSERT_OK(job);
        scan::RecoveryJobOptions jobOptions;
        jobOptions.writer.diskResolver = systemDisk();
        const Result<scan::RecoveryJobSummary> recovered = session.runRecovery(*job, *disk, std::move(jobOptions));
        RECOVERY_ASSERT_OK(recovered);
        EXPECT_GT(recovered->metrics.recoveredFiles, 0U);
    }

    // One record says so, which an older engine may skip.
    std::size_t found = 0;
    for (const JournalRecord& record : test::readRecords(folder)) {
        if (record.type == static_cast<std::uint16_t>(RecordType::SourceFound)) {
            ++found;
            EXPECT_TRUE(record.optional());
            const RecordPayload decoded = test::decoded(record);
            const SourceFoundRecord& payload = std::get<SourceFoundRecord>(decoded);
            EXPECT_EQ(payload.path, path(9));
            EXPECT_EQ(payload.diskNumber, 9U);
        }
    }
    EXPECT_EQ(found, 1U);
    // As it is when the session is opened again, and listed.
    Result<std::unique_ptr<RecoverySession>> again = RecoverySession::open(folder, sessionOptions());
    RECOVERY_ASSERT_OK(again);
    EXPECT_EQ((*again)->info().source.path, path(9));
    EXPECT_EQ((*again)->info().scan.state, SessionState::Completed);
    again->reset();
    Result<std::vector<SessionSummary>> listed = listSessions(dir / "sessions");
    RECOVERY_ASSERT_OK(listed);
    ASSERT_EQ(listed->size(), 1U);
    EXPECT_EQ((*listed)[0].sourcePath, path(9));
    EXPECT_EQ((*listed)[0].source.diskNumber, 9U);
    EXPECT_EQ((*listed)[0].source.serialNumber, "SER-A");
}

// A record of a disk found again that a session cannot use (its path is not
// its number's; the session's source is an image file) is damage: the
// session opens as it was before it, and its source is as recorded.
TEST(SessionDiskTest, ADiskFoundAgainThatCannotBeUsedIsDamage) {
    auto rack = std::make_shared<DiskRack>();
    (void)rack->add(7, card(), {}, "SER-A");
    TempDir dir;
    std::filesystem::path made;
    {
        const std::unique_ptr<storage::PhysicalDiskSource> disk = openDisk(rack, 7);
        Result<std::unique_ptr<RecoverySession>> created =
            RecoverySession::create(dir / "made", *disk, {}, sessionOptions());
        RECOVERY_ASSERT_OK(created);
        made = (*created)->folder();
    }
    const std::vector<JournalRecord> disk = test::readRecords(made);
    ASSERT_EQ(disk.size(), 1U);
    SessionCreatedRecord ofImage = std::get<SessionCreatedRecord>(test::decoded(disk[0]));
    ofImage.source.type = storage::SourceType::DiskImage;
    ofImage.source.path = "E:\\card.img";
    ofImage.source.diskNumber.reset();

    SourceFoundRecord mismatched;
    mismatched.path = path(8);
    mismatched.diskNumber = 9;
    SourceFoundRecord found;
    found.path = path(9);
    found.diskNumber = 9;
    struct Case {
        std::string name;
        JournalRecord created;
        SourceFoundRecord found;
    };
    const std::vector<Case> cases = {
        {"path of another number", disk[0], mismatched},
        {"a session of an image file", test::recordOf(RecordPayload{ofImage}), found},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const std::filesystem::path folder = dir / c.name;
        JournalRecord record = test::recordOf(RecordPayload{c.found});
        record.flags = kRecordOptional;
        test::writeJournal(folder, test::buildJournal({c.created, record}));
        Result<std::unique_ptr<RecoverySession>> session = RecoverySession::open(folder, sessionOptions());
        RECOVERY_ASSERT_OK(session);
        const SessionInfo info = (*session)->info();
        ASSERT_EQ(info.damage.size(), 1U);
        EXPECT_NE(info.damage[0].damage.reason.find("cannot be used"), std::string::npos)
            << info.damage[0].damage.reason;
        EXPECT_NE(info.source.path, path(9));
        EXPECT_NE(info.source.path, path(8));
    }
}

// A session of an older format did not record the serial number: the device
// is told by its vendor and product until the session finds its disk, then
// by the serial number it records (in the journal's own format).
TEST(SessionDiskTest, AnOlderSessionLearnsItsDisksSerialNumber) {
    auto rack = std::make_shared<DiskRack>();
    const std::shared_ptr<FakeDeviceConfig> config = rack->add(7, card(), {}, "SER-A");
    TempDir dir;
    std::filesystem::path made;
    {
        const std::unique_ptr<storage::PhysicalDiskSource> disk = openDisk(rack, 7);
        Result<std::unique_ptr<RecoverySession>> created =
            RecoverySession::create(dir / "made", *disk, {}, sessionOptions());
        RECOVERY_ASSERT_OK(created);
        made = (*created)->folder();
    }
    // The same session in format 2.
    std::vector<JournalRecord> records = test::readRecords(made);
    ASSERT_EQ(records.size(), 1U);
    records[0].payload = encodePayload(test::decoded(records[0]), 2);
    const std::filesystem::path folder = dir / "older";
    test::writeJournal(folder, test::buildJournal(records, 2));

    Result<std::unique_ptr<RecoverySession>> opened = RecoverySession::open(folder, sessionOptions());
    RECOVERY_ASSERT_OK(opened);
    RecoverySession& session = **opened;
    EXPECT_EQ(session.info().formatVersion, 2U);
    EXPECT_TRUE(session.info().source.serialNumber.empty());
    // Another reader model is another device; the same model is the session's.
    auto otherModel = copyOf(config, "SER-A");
    otherModel->description.product = "Other Reader";
    rack->attach(8, otherModel);
    EXPECT_FALSE(runScan(session, *openDisk(rack, 8)).ok());
    rack->renumber(7, 9);
    const Result<scan::ScanSummary> scanned = runScan(session, *openDisk(rack, 9));
    RECOVERY_ASSERT_OK(scanned);
    EXPECT_EQ(scanned->outcome, scan::ScanOutcome::Completed);
    EXPECT_EQ(session.info().source.serialNumber, "SER-A");
    EXPECT_EQ(session.info().source.path, path(9));
    opened->reset();

    // The journal kept its format; the serial number is known from then on.
    for (const JournalRecord& record : test::readRecords(folder)) {
        EXPECT_TRUE(decodePayload(static_cast<RecordType>(record.type), record.payload, 2).ok())
            << toString(static_cast<RecordType>(record.type));
    }
    Result<std::unique_ptr<RecoverySession>> again = RecoverySession::open(folder, sessionOptions());
    RECOVERY_ASSERT_OK(again);
    EXPECT_EQ((*again)->info().formatVersion, 2U);
    EXPECT_EQ((*again)->info().source.serialNumber, "SER-A");
    rack->attach(11, copyOf(config, "SER-B"));
    const Status other = checkSameSource((*again)->info().source, *openDisk(rack, 11));
    ASSERT_FALSE(other.ok());
    EXPECT_NE(other.error().message.find("'SER-B'"), std::string::npos) << other.error().message;
}

}  // namespace
}  // namespace recovery::session
