// The encoding of session records (P16): every update a deep scan hands out
// and every kind of value comes back as it was (decoding and encoding again
// gives the same bytes; the candidates explain themselves the same); bytes
// cut short, values out of range, numbers not in their shortest form,
// lengths beyond the data and candidates that break their invariants are
// refused, and random damage never crashes the decoder.

#include "session/session_journal.hpp"

#include "partition/guid.hpp"
#include "scan/scan_test_support.hpp"
#include "support/memory_source.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace recovery::session {
namespace {

using Bytes = std::vector<std::byte>;

struct DeepScan {
    bool ok = false;
    std::vector<scan::ScanUpdate> updates;
};

// Every update of a deep scan of the scan tests' card (kept once).
const DeepScan& deepScan() {
    static const DeepScan scan = [] {
        DeepScan result;
        ::recovery::test::MemoryStorageSource source(scan::test::makeCard());
        if (!source.open().ok()) {
            return result;
        }
        scan::ScanCoordinator coordinator(source, scan::test::allFormats(), scan::test::allMedia(), {},
                                          scan::test::testRunOptions());
        const Result<scan::ScanSummary> summary = coordinator.run([&](const scan::ScanUpdate& update) {
            result.updates.push_back(update);
            return success();
        });
        result.ok = summary.ok() && summary->outcome == scan::ScanOutcome::Completed;
        return result;
    }();
    return scan;
}

std::vector<std::string> explained(const std::vector<evaluation::EvaluatedCandidate>& candidates) {
    std::vector<std::string> lines;
    for (const evaluation::EvaluatedCandidate& candidate : candidates) {
        lines.push_back(scan::test::describe(candidate));
        for (const std::string& line : evaluation::explain(candidate)) {
            lines.push_back("  " + line);
        }
    }
    return lines;
}

template <class T>
T roundTrip(const T& value, RecordType type) {
    const Bytes bytes = encodePayload(value);
    Result<RecordPayload> decoded = decodePayload(type, bytes);
    EXPECT_TRUE(decoded.ok()) << (decoded.ok() ? std::string() : describe(decoded.error()));
    if (!decoded.ok()) {
        return T{};
    }
    const T* back = std::get_if<T>(&*decoded);
    EXPECT_NE(back, nullptr);
    if (back == nullptr) {
        return T{};
    }
    // Encoding what was decoded gives the same bytes: nothing was lost.
    EXPECT_EQ(encodePayload(*back), bytes);
    return *back;
}

// ===========================================================================
// What comes back
// ===========================================================================

TEST(SessionCodecTest, EveryUpdateOfADeepScanComesBackAsItWas) {
    const DeepScan& scan = deepScan();
    ASSERT_TRUE(scan.ok);
    ASSERT_GE(scan.updates.size(), 10U);
    std::set<scan::ScanStage> stages;
    std::size_t candidates = 0;
    for (const scan::ScanUpdate& update : scan.updates) {
        SCOPED_TRACE(update.sequence);
        stages.insert(update.stage);
        const scan::ScanUpdate back = roundTrip(update, RecordType::ScanUpdate);
        EXPECT_EQ(back.sequence, update.sequence);
        EXPECT_EQ(back.stage, update.stage);
        EXPECT_EQ(explained(back.candidates), explained(update.candidates));
        candidates += update.candidates.size();
    }
    // Every stage handed out updates, and the evaluation its candidates.
    EXPECT_EQ(stages.size(), 7U);
    EXPECT_GE(candidates, 7U);
}

// A checkpoint made of the decoded updates is the checkpoint of the updates.
TEST(SessionCodecTest, DecodedUpdatesMakeTheSameCheckpoint) {
    const DeepScan& scan = deepScan();
    ASSERT_TRUE(scan.ok);
    scan::ScanCheckpoint original;
    scan::ScanCheckpoint decoded;
    for (const scan::ScanUpdate& update : scan.updates) {
        RECOVERY_ASSERT_OK(original.apply(update));
        Result<RecordPayload> back = decodePayload(RecordType::ScanUpdate, encodePayload(update));
        RECOVERY_ASSERT_OK(back);
        RECOVERY_ASSERT_OK(decoded.apply(std::get<scan::ScanUpdate>(*back)));
    }
    EXPECT_TRUE(decoded.completed());
    EXPECT_EQ(decoded.sequence(), original.sequence());
    EXPECT_EQ(*decoded.identity(), *original.identity());
    EXPECT_EQ(decoded.carves().size(), original.carves().size());
    EXPECT_EQ(decoded.mp4Candidates().size(), original.mp4Candidates().size());
    EXPECT_EQ(decoded.fragmentCandidates().size(), original.fragmentCandidates().size());
    EXPECT_EQ(decoded.evaluated().size(), original.evaluated().size());
    for (std::size_t i = 0; i < decoded.evaluated().size(); ++i) {
        EXPECT_EQ(decoded.evaluated()[i].identity.sha256, original.evaluated()[i].identity.sha256);
    }
    EXPECT_EQ(decoded.metrics().candidates, original.metrics().candidates);
    EXPECT_EQ(decoded.metrics().elapsed, original.metrics().elapsed);
}

// Values a FAT32 card does not have: GPT partitions, NTFS data in the record,
// timestamps, issues of every kind, errors, MP4 tracks, reconstructions.
scan::ScanUpdate richUpdate() {
    const partition::Guid unique = *partition::Guid::parse("01234567-89AB-CDEF-0123-456789ABCDEF");
    const partition::Guid disk = *partition::Guid::parse("FEDCBA98-7654-3210-FEDC-BA9876543210");
    scan::ScanUpdate update;
    update.sequence = 1;
    update.stage = scan::ScanStage::Volumes;
    update.stageComplete = true;
    scan::ScanIdentity identity;
    identity.engineVersion = "9.8.7";
    identity.sourceType = storage::SourceType::PhysicalDisk;
    identity.sourcePath = "\\\\.\\PhysicalDrive3";
    identity.sourceSize = (std::uint64_t{1} << 40) + 512;
    identity.sectorSize = 4096;
    identity.mode = ScanMode::Quick;
    identity.configuration = "mode=quick\ncarving=no\n";
    identity.formats = {"jpeg", "png"};
    identity.mediaValidators = {"jpeg"};
    update.identity = identity;

    partition::Partition part;
    part.index = 3;
    part.firstLba = 2048;
    part.sectorCount = 999;
    part.offset = 2048ULL * 4096;
    part.size = 999ULL * 4096;
    part.truncated = true;
    part.mbrType = 0xEE;
    part.bootable = true;
    part.logical = true;
    part.typeGuid = partition::gpt_types::kMicrosoftBasicData;
    part.uniqueGuid = unique;
    part.attributes = 0x8000000000000001ULL;
    part.name = "Donn\xC3\xA9" "es";
    part.typeName = "Basic data";
    part.candidates = {true, false, true};
    partition::PartitionTable table;
    table.scheme = partition::PartitionScheme::Gpt;
    table.sectorSize = 4096;
    table.deviceSectors = 123456789;
    table.partitions = {part};
    table.gpt = partition::GptInfo{disk, 1, 34, 1000, 128, 128, true, false};
    table.issues = {partition::PartitionIssue{partition::PartitionIssueKind::GptBackupInvalid, 2, "backup"}};
    update.partitionTable = table;

    RecoveryCandidate file;
    file.id = CandidateId{42};
    file.method = RecoveryMethod::Fragmented;
    file.filename = "r\xC3\xA9sum\xC3\xA9.txt";
    file.extension = "txt";
    file.expectedSize = 300;
    file.sourceRegions = {SourceRegion{0, 100, RegionKind::Embedded, 4, false},
                          SourceRegion{100, 100, RegionKind::Stored, 1ULL << 33, true},
                          SourceRegion{200, 50, RegionKind::Zeros, 0, false},
                          SourceRegion{250, 50, RegionKind::Missing, 0, false}};
    file.embeddedData = Bytes(104, std::byte{0x5A});
    file.fragmentation = FragmentationInfo{3, false};
    FilesystemEvidence& evidence = file.filesystemEvidence;
    evidence.type = filesystem::FilesystemType::Ntfs;
    evidence.volumeOffset = part.offset;
    evidence.metadataOffset = part.offset + 0x4000;
    evidence.path = "/$OrphanFiles/r\xC3\xA9sum\xC3\xA9.txt";
    evidence.shortName = "RESUME~1.TXT";
    evidence.state = filesystem::EntryState::Deleted;
    evidence.parentDeleted = true;
    evidence.validDataLength = 250;
    evidence.created = filesystem::Timestamp{SessionTime{std::chrono::milliseconds{-1234}}, false};
    evidence.modified = filesystem::Timestamp{SessionTime{std::chrono::milliseconds{1'700'000'000'123}}, true};
    evidence.attributes = filesystem::EntryAttributes{true, false, true, false};
    evidence.entryIssues = {filesystem::EntryIssue::ParentMissing, filesystem::EntryIssue::DamagedRecord};
    evidence.allocation.method = filesystem::AllocationMethod::RunList;
    evidence.allocation.layout = LayoutEvidence::Recorded;
    evidence.allocation.firstCluster = filesystem::ClusterNumber{77};
    evidence.allocation.clusterCount = 9;
    evidence.allocation.clusterSize = 4096;
    evidence.allocation.issues = {filesystem::AllocationIssue::SparseRuns,
                                  filesystem::AllocationIssue::DataAttributeMissing};
    file.warnings = {CandidateWarning::DataMissing, CandidateWarning::MetadataDamaged};

    CandidateScan candidates;
    candidates.filesystemInfo.type = filesystem::FilesystemType::Ntfs;
    candidates.filesystemInfo.label = "DATA";
    candidates.filesystemInfo.serialNumber = 0xDEADBEEFCAFEF00DULL;
    candidates.filesystemInfo.bytesPerSector = 4096;
    candidates.filesystemInfo.clusterSize = 4096;
    candidates.filesystemInfo.clusterCount = 999;
    candidates.filesystemInfo.volumeSize = part.size;
    candidates.filesystemInfo.dataOffset = 0;
    candidates.filesystemInfo.warnings = {"primary boot sector invalid"};
    candidates.volumeOffset = part.offset;
    candidates.candidates = {file};
    candidates.issues = {filesystem::ScanIssue{filesystem::ScanIssueKind::RecordInvalid, "/x", "bad"}};
    candidates.directories = 5;
    candidates.systemFiles = 16;
    candidates.complete = false;

    scan::VolumeRecord done;
    done.offset = part.offset;
    done.size = part.size;
    done.partition = part;
    done.scanned = true;
    done.filesystem = filesystem::FilesystemType::Ntfs;
    done.candidates = candidates;
    scan::VolumeRecord failed;
    failed.offset = 0;
    failed.size = 4096;
    failed.scanned = true;
    failed.error = makeError(ErrorCode::UnsupportedFilesystem, "no filesystem", 1005);
    update.volumePlan = std::vector<scan::VolumeRecord>{done, failed};
    update.volumes = {{0, done}, {1, failed}};
    update.unreadable = {storage::BadRegion{512, 4096, 23}};
    update.metrics.sourceSize = identity.sourceSize;
    update.metrics.bytesRead = 1ULL << 41;
    update.metrics.elapsed = std::chrono::milliseconds{-5};
    return update;
}

TEST(SessionCodecTest, EveryKindOfValueComesBackAsItWas) {
    const scan::ScanUpdate update = richUpdate();
    const scan::ScanUpdate back = roundTrip(update, RecordType::ScanUpdate);
    ASSERT_TRUE(back.partitionTable.has_value());
    const partition::Partition& part = back.partitionTable->partitions.at(0);
    EXPECT_EQ(part.uniqueGuid, update.partitionTable->partitions[0].uniqueGuid);
    EXPECT_EQ(part.typeGuid, partition::gpt_types::kMicrosoftBasicData);
    EXPECT_EQ(part.name, "Donn\xC3\xA9" "es");
    EXPECT_EQ(part.attributes, 0x8000000000000001ULL);
    EXPECT_EQ(part.mbrType, 0xEE);
    EXPECT_EQ(back.partitionTable->gpt->diskGuid, update.partitionTable->gpt->diskGuid);
    ASSERT_EQ(back.volumes.size(), 2U);
    const RecoveryCandidate& file = back.volumes[0].second.candidates->candidates.at(0);
    EXPECT_EQ(file.embeddedData, Bytes(104, std::byte{0x5A}));
    EXPECT_EQ(file.sourceRegions, update.volumes[0].second.candidates->candidates[0].sourceRegions);
    EXPECT_EQ(file.filesystemEvidence.created->time.time_since_epoch().count(), -1234);
    EXPECT_FALSE(file.filesystemEvidence.created->local);
    EXPECT_FALSE(file.filesystemEvidence.accessed.has_value());
    EXPECT_EQ(file.filesystemEvidence.allocation.firstCluster.value(), 77U);
    EXPECT_EQ(back.volumes[1].second.error->code, ErrorCode::UnsupportedFilesystem);
    EXPECT_EQ(back.volumes[1].second.error->systemErrorCode, 1005U);
    EXPECT_EQ(back.metrics.elapsed.count(), -5);
    EXPECT_EQ(back.identity->sourceSize, (std::uint64_t{1} << 40) + 512);
}

TEST(SessionCodecTest, EverySessionRecordComesBackAsItWas) {
    SessionCreatedRecord created;
    created.id = "20261006-101530-3fa94c2e";
    created.engineVersion = "0.1.0";
    created.source.type = storage::SourceType::DiskImage;
    created.source.path = "D:\\images\\card \xE2\x82\xAC.img";
    created.source.size = 1 << 30;
    created.source.sectorSize = 512;
    created.source.physicalSectorSize = 4096;
    created.source.diskNumber = 4;
    created.source.vendor = "Vendor";
    created.source.product = "Card reader";
    created.source.removable = true;
    created.source.fingerprint.digest = sha256(Bytes(10, std::byte{1}));
    created.source.fingerprint.bytes = 131072;
    created.source.fingerprint.unreadableBytes = 512;
    created.configuration.mode = ScanMode::Quick;
    created.configuration.alignment = 512;
    created.configuration.knownBadRegions = {storage::BadRegion{4096, 512, 23}};
    created.configuration.sha256 = false;
    created.playability = true;
    const SessionCreatedRecord session = roundTrip(created, RecordType::SessionCreated);
    EXPECT_EQ(session.source.path, created.source.path);
    EXPECT_EQ(session.source.fingerprint, created.source.fingerprint);
    EXPECT_EQ(session.source.diskNumber, 4U);
    EXPECT_EQ(session.configuration.knownBadRegions, created.configuration.knownBadRegions);
    EXPECT_EQ(session.configuration.playability, nullptr);
    EXPECT_TRUE(session.playability);

    ScanStateRecord state;
    state.state = SessionState::Failed;
    state.engineVersion = "0.1.0";
    state.error = makeError(ErrorCode::IoError, "the device went away", 1167);
    state.stage = scan::ScanStage::SourcePass;
    state.metrics.bytesScanned = 12345;
    EXPECT_EQ(roundTrip(state, RecordType::ScanState).error->systemErrorCode, 1167U);

    JobCreatedRecord job;
    job.job = 2;
    job.destination = "E:\\Recovered";
    job.candidates = {evaluation::EvaluatedCandidateId{3}, evaluation::EvaluatedCandidateId{1}};
    EXPECT_EQ(roundTrip(job, RecordType::JobCreated).candidates, job.candidates);

    JobUpdateRecord jobUpdate;
    jobUpdate.job = 2;
    jobUpdate.update.sequence = 1;
    jobUpdate.update.destination = "E:\\Recovered";
    scan::RecoveredItem written;
    written.candidate = evaluation::EvaluatedCandidateId{3};
    written.file = RecoveredFile{CandidateId{8}, std::filesystem::path(L"E:\\Recovered\\caf\u00E9.jpg"), {}};
    written.file->report.expectedSize = 1000;
    written.file->report.outputSize = 900;
    written.file->report.missingBytes = 100;
    written.file->report.unreadableRegions = {storage::BadRegion{1024, 512, 23}};
    scan::RecoveredItem failedItem;
    failedItem.candidate = evaluation::EvaluatedCandidateId{1};
    failedItem.error = makeError(ErrorCode::DestinationError, "disk full", 112);
    jobUpdate.update.items = {written, failedItem};
    jobUpdate.update.complete = true;
    jobUpdate.update.metrics.recoveredFiles = 1;
    const JobUpdateRecord jobBack = roundTrip(jobUpdate, RecordType::JobUpdate);
    ASSERT_EQ(jobBack.update.items.size(), 2U);
    EXPECT_EQ(jobBack.update.items[0].file->path, written.file->path);
    EXPECT_EQ(jobBack.update.items[0].file->report.missingBytes, 100U);
    EXPECT_EQ(jobBack.update.items[1].error->message, "disk full");

    JobStateRecord jobState;
    jobState.job = 2;
    jobState.state = SessionState::Paused;
    jobState.metrics.failedFiles = 1;
    EXPECT_EQ(roundTrip(jobState, RecordType::JobState).metrics.failedFiles, 1U);

    FileStartedRecord started;
    started.job = 2;
    started.candidate = evaluation::EvaluatedCandidateId{3};
    started.path = "E:\\Recovered\\caf\xC3\xA9.jpg";
    EXPECT_EQ(roundTrip(started, RecordType::FileStarted).path, started.path);

    DamageRecord damage;
    damage.offset = 4096;
    damage.bytesDropped = 100;
    damage.recordsDropped = 3;
    damage.reason = "a record whose payload does not check";
    damage.backup = "session.journal.damaged-1";
    EXPECT_EQ(roundTrip(damage, RecordType::Damage).recordsDropped, 3U);
}

// ===========================================================================
// What is refused
// ===========================================================================

TEST(SessionCodecTest, APayloadCutShortOrWithBytesLeftOverIsRefused) {
    const DeepScan& scan = deepScan();
    ASSERT_TRUE(scan.ok);
    // The largest update (the evaluation's candidates, or a volume's).
    const scan::ScanUpdate* largest = &scan.updates.front();
    for (const scan::ScanUpdate& update : scan.updates) {
        if (encodePayload(update).size() > encodePayload(*largest).size()) {
            largest = &update;
        }
    }
    const Bytes bytes = encodePayload(*largest);
    ASSERT_GT(bytes.size(), 1000U);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        const Result<RecordPayload> cut = decodePayload(RecordType::ScanUpdate, std::span(bytes).first(size));
        ASSERT_FALSE(cut.ok()) << size;
        EXPECT_EQ(cut.error().code, ErrorCode::InvalidFormat);
    }
    Bytes longer = bytes;
    longer.push_back(std::byte{0});
    const Result<RecordPayload> extra = decodePayload(RecordType::ScanUpdate, longer);
    ASSERT_FALSE(extra.ok());
    EXPECT_NE(extra.error().message.find("left over"), std::string::npos) << extra.error().message;
}

void expectRefused(RecordType type, const Bytes& bytes, std::string_view why) {
    const Result<RecordPayload> decoded = decodePayload(type, bytes);
    ASSERT_FALSE(decoded.ok()) << why;
    EXPECT_EQ(decoded.error().code, ErrorCode::InvalidFormat);
    EXPECT_NE(decoded.error().message.find(why), std::string::npos) << decoded.error().message;
}

TEST(SessionCodecTest, ValuesOutOfTheirRangeAreRefused) {
    ScanStateRecord state;
    const Bytes stateBytes = encodePayload(state);
    // The state comes first: one byte, 0-4.
    Bytes beyond = stateBytes;
    beyond[0] = std::byte{5};
    expectRefused(RecordType::ScanState, beyond, "out of the range of its kind");
    // 0 written in two bytes.
    Bytes overlong = stateBytes;
    overlong[0] = std::byte{0x80};
    overlong.insert(overlong.begin() + 1, std::byte{0x00});
    expectRefused(RecordType::ScanState, overlong, "shortest form");

    // A flag that is neither 0 nor 1: SessionCreatedRecord ends with one.
    Bytes flag = encodePayload(SessionCreatedRecord{});
    flag.back() = std::byte{2};
    expectRefused(RecordType::SessionCreated, flag, "neither 0 nor 1");

    // The job number (32 bits) first: 2^32, then a number beyond 64 bits.
    JobCreatedRecord job;
    job.job = 1;
    Bytes wide = encodePayload(job);
    wide.erase(wide.begin());
    const Bytes twoTo32 = {std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x10}};
    Bytes tooLarge = wide;
    tooLarge.insert(tooLarge.begin(), twoTo32.begin(), twoTo32.end());
    expectRefused(RecordType::JobCreated, tooLarge, "too large for its field");
    Bytes huge = wide;
    huge.insert(huge.begin(), 10, std::byte{0xFF});
    expectRefused(RecordType::JobCreated, huge, "beyond 64 bits");

    // A string longer than the data: FileStartedRecord's path comes last.
    FileStartedRecord started;
    started.path = "abc";
    Bytes path = encodePayload(started);
    path[path.size() - 4] = std::byte{100};
    expectRefused(RecordType::FileStarted, path, "a length of 100");

    // An empty unreadable region.
    scan::ScanUpdate update;
    update.sequence = 1;
    update.unreadable = {storage::BadRegion{512, 0, 23}};
    expectRefused(RecordType::ScanUpdate, encodePayload(update), "empty or overflowing unreadable region");
}

TEST(SessionCodecTest, CandidatesThatBreakTheirInvariantsAreRefused) {
    scan::ScanUpdate update;
    update.sequence = 9;
    update.stage = scan::ScanStage::Evaluation;
    evaluation::EvaluatedCandidate candidate;
    candidate.id = evaluation::EvaluatedCandidateId{1};
    candidate.data.expectedSize = 100;
    // A gap between the regions.
    candidate.data.sourceRegions = {SourceRegion{0, 40, RegionKind::Stored, 0, false},
                                    SourceRegion{50, 50, RegionKind::Stored, 100, false}};
    update.candidates = {candidate};
    const Result<RecordPayload> gap = decodePayload(RecordType::ScanUpdate, encodePayload(update));
    ASSERT_FALSE(gap.ok());
    EXPECT_EQ(gap.error().code, ErrorCode::InvalidFormat);

    // A carve whose extents do not cover it.
    update.candidates.clear();
    update.stage = scan::ScanStage::SourcePass;
    carving::FileCandidate carve;
    carve.id = carving::FileCandidateId{1};
    carve.formatId = "jpeg";
    carve.length = 100;
    carve.extents = {carving::CarvedExtent{0, 0, 60}};
    update.carves = {carve};
    const Result<RecordPayload> short_ = decodePayload(RecordType::ScanUpdate, encodePayload(update));
    ASSERT_FALSE(short_.ok());
    EXPECT_EQ(short_.error().code, ErrorCode::InvalidFormat);
}

TEST(SessionCodecTest, APathThatIsNotUtf8IsRefused) {
    JobUpdateRecord record;
    record.job = 1;
    scan::RecoveredItem item;
    item.candidate = evaluation::EvaluatedCandidateId{1};
    item.file = RecoveredFile{CandidateId{1}, std::filesystem::path(L"Q"), {}};
    record.update.items = {item};
    Bytes bytes = encodePayload(record);
    const auto q = std::find(bytes.begin(), bytes.end(), std::byte{'Q'});
    ASSERT_NE(q, bytes.end());
    *q = std::byte{0xFF};
    expectRefused(RecordType::JobUpdate, bytes, "not UTF-8");
}

// Random damage to encoded updates: the decoder refuses or decodes, and what
// it decodes encodes again; it never reads out of bounds (ASan) or crashes.
TEST(SessionCodecTest, RandomDamageNeverCrashesTheDecoder) {
    const DeepScan& scan = deepScan();
    ASSERT_TRUE(scan.ok);
    std::mt19937_64 random(0x5E55);
    std::size_t refused = 0;
    for (const scan::ScanUpdate& update : scan.updates) {
        const Bytes bytes = encodePayload(update);
        for (int round = 0; round < 40; ++round) {
            Bytes damaged = bytes;
            const std::size_t flips = 1 + random() % 4;
            for (std::size_t i = 0; i < flips; ++i) {
                damaged[random() % damaged.size()] ^= static_cast<std::byte>(1 + random() % 255);
            }
            const Result<RecordPayload> decoded = decodePayload(RecordType::ScanUpdate, damaged);
            if (!decoded.ok()) {
                ++refused;
                EXPECT_EQ(decoded.error().code, ErrorCode::InvalidFormat);
                continue;
            }
            (void)encodePayload(*decoded);
        }
    }
    EXPECT_GT(refused, 0U);
}

}  // namespace
}  // namespace recovery::session
