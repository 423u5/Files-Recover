// NTFS metadata corruption: damaged FILE records and run lists, cross-links,
// broken directory structure, damaged system files, unreadable sectors and
// hostile boot sectors. Every traversal must terminate, every reported
// extent must stay inside the volume, and every inconsistency must be named.

#include "filesystem/ntfs/ntfs_filesystem.hpp"

#include "recovery/byte_order.hpp"
#include "support/fat32_builder.hpp"  // readExtents
#include "support/memory_source.hpp"
#include "support/ntfs_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <random>

namespace recovery::filesystem::ntfs {
namespace {

using test::NtfsImageBuilder;

constexpr std::uint64_t kRoot = NtfsImageBuilder::root();

struct Mounted {
    std::unique_ptr<test::MemoryStorageSource> source;
    std::unique_ptr<NtfsFilesystem> fs;
};

Mounted mount(std::vector<std::byte> image) {
    Mounted m;
    m.source = std::make_unique<test::MemoryStorageSource>(std::move(image), 512);
    EXPECT_TRUE(m.source->open().ok());
    Result<std::unique_ptr<NtfsFilesystem>> fs = NtfsFilesystem::open(*m.source);
    EXPECT_TRUE(fs.ok()) << (fs.ok() ? "" : describe(fs.error()));
    if (fs.ok()) {
        m.fs = std::move(fs).value();
    }
    return m;
}

FileScan scanOf(const Mounted& m, ScanLimits limits = {}) {
    Result<FileScan> scan = m.fs->scan(limits, {});
    EXPECT_TRUE(scan.ok()) << (scan.ok() ? "" : describe(scan.error()));
    return scan.ok() ? std::move(scan).value() : FileScan{};
}

const FileRecord* find(const FileScan& scan, std::string_view path) {
    for (const FileRecord& record : scan.records) {
        if (record.path == path) {
            return &record;
        }
    }
    return nullptr;
}

bool hasIssue(const FileScan& scan, ScanIssueKind kind, std::string_view text = {}) {
    return std::any_of(scan.issues.begin(), scan.issues.end(), [&](const ScanIssue& i) {
        return i.kind == kind && i.detail.find(text) != std::string::npos;
    });
}

bool hasWarning(const FilesystemInfo& info, std::string_view text) {
    return std::any_of(info.warnings.begin(), info.warnings.end(),
                       [&](const std::string& w) { return w.find(text) != std::string::npos; });
}

// The mapping pairs of a record's unnamed $DATA attribute (unprotected record).
std::span<std::byte> runBytes(NtfsImageBuilder& builder, std::uint64_t record) {
    const std::size_t attribute = builder.attributeOffset(record, kAttrData);
    const std::span<std::byte> plain = builder.record(record);
    const std::size_t runs = loadLe16(plain, attribute + 32);
    const std::size_t length = loadLe32(plain, attribute + 4);
    return plain.subspan(attribute + runs, length - runs);
}

// Writes `pairs` as the mapping pairs, zero-filling the rest of the attribute.
void setRuns(NtfsImageBuilder& builder, std::uint64_t record, std::initializer_list<int> pairs) {
    const std::span<std::byte> runs = runBytes(builder, record);
    ASSERT_LE(pairs.size(), runs.size());
    std::fill(runs.begin(), runs.end(), std::byte{0});
    std::size_t i = 0;
    for (const int value : pairs) {
        runs[i++] = static_cast<std::byte>(value);
    }
}

// ---------------------------------------------------------------------------
// Damaged FILE records.
// ---------------------------------------------------------------------------

class NtfsRecordCorruptionTest : public ::testing::Test {
protected:
    void SetUp() override {
        victim_ = builder_.addFile(kRoot, "victim.bin", test::makePattern(3000, 1));
        bystander_ = builder_.addFile(kRoot, "bystander.txt", test::makePattern(100, 2));
    }

    FileScan scanImage(std::vector<std::byte> image) {
        mounted_ = mount(std::move(image));
        return scanOf(mounted_);
    }

    NtfsImageBuilder builder_;
    NtfsImageBuilder::Entry victim_;
    NtfsImageBuilder::Entry bystander_;
    Mounted mounted_;
};

TEST_F(NtfsRecordCorruptionTest, TornRecordIsReportedAndSkipped) {
    std::vector<std::byte> image = builder_.build();
    storeLe16(image, builder_.recordOffset(victim_.record) + 1022, 0x4242);
    const FileScan scan = scanImage(std::move(image));
    EXPECT_EQ(find(scan, "/victim.bin"), nullptr);
    EXPECT_NE(find(scan, "/bystander.txt"), nullptr);
    EXPECT_TRUE(hasIssue(scan, ScanIssueKind::RecordInvalid, "MFT record 64: update sequence Mismatch"));
}

TEST_F(NtfsRecordCorruptionTest, BadOrMissingSignature) {
    for (const char* signature : {"BAAD", "INDX", "\0\0\0\1"}) {
        std::vector<std::byte> image = builder_.build();
        std::memcpy(image.data() + builder_.recordOffset(victim_.record), signature, 4);
        const FileScan scan = scanImage(std::move(image));
        EXPECT_EQ(find(scan, "/victim.bin"), nullptr) << signature;
        EXPECT_NE(find(scan, "/bystander.txt"), nullptr) << signature;
        EXPECT_TRUE(hasIssue(scan, ScanIssueKind::RecordInvalid, "MFT record 64")) << signature;
    }
}

TEST_F(NtfsRecordCorruptionTest, MisplacedRecordIsRejected) {
    std::vector<std::byte> image = builder_.build();
    // A copy of record 64 in slot 70 still says it is record 64.
    std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(builder_.recordOffset(64)), 1024,
                image.begin() + static_cast<std::ptrdiff_t>(builder_.recordOffset(70)));
    const FileScan scan = scanImage(std::move(image));
    EXPECT_TRUE(hasIssue(scan, ScanIssueKind::RecordInvalid, "MFT record 70: record claims to be number 64"));
    EXPECT_EQ(std::count_if(scan.records.begin(), scan.records.end(),
                            [](const FileRecord& r) { return r.path == "/victim.bin"; }),
              1);
}

TEST_F(NtfsRecordCorruptionTest, DamagedAttributeChainKeepsTheName) {
    storeLe32(builder_.record(victim_.record), builder_.attributeOffset(victim_.record, kAttrData) + 4, 3);
    const FileScan scan = scanImage(builder_.build());
    const FileRecord* record = find(scan, "/victim.bin");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->entry.hasIssue(EntryIssue::DamagedRecord));
    EXPECT_TRUE(record->allocation.hasIssue(AllocationIssue::DataAttributeMissing));
    EXPECT_EQ(record->allocation.method, AllocationMethod::None);
    EXPECT_TRUE(hasIssue(scan, ScanIssueKind::RecordInvalid, "MFT record 64: attribute 0x80"));
}

TEST_F(NtfsRecordCorruptionTest, MissingStandardInformation) {
    // Turn $STANDARD_INFORMATION into an attribute the engine does not interpret.
    storeLe32(builder_.record(victim_.record), builder_.attributeOffset(victim_.record, kAttrStandardInformation),
              0x40);
    const FileScan scan = scanImage(builder_.build());
    const FileRecord* record = find(scan, "/victim.bin");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->entry.hasIssue(EntryIssue::MetadataIncomplete));
    EXPECT_FALSE(record->entry.created.has_value());
    EXPECT_FALSE(record->entry.attributes.archive);
    EXPECT_EQ(mounted_.fs->resolveAllocation(record->entry)->extents, record->allocation.extents);
}

TEST_F(NtfsRecordCorruptionTest, InvalidTimestamp) {
    const std::size_t si = builder_.attributeOffset(victim_.record, kAttrStandardInformation);
    storeLe64(builder_.record(victim_.record), si + 24, 0x8000000000000000ULL);  // created
    const FileScan scan = scanImage(builder_.build());
    const FileRecord* record = find(scan, "/victim.bin");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->entry.hasIssue(EntryIssue::InvalidTimestamp));
    EXPECT_FALSE(record->entry.created.has_value());
    EXPECT_TRUE(record->entry.modified.has_value());
}

TEST_F(NtfsRecordCorruptionTest, InvalidNames) {
    const std::size_t fn = builder_.attributeOffset(victim_.record, kAttrFileName) + 24;
    storeLe16(builder_.record(victim_.record), fn + 0x42 + 2 * 2, u'/');  // "vi/tim.bin"
    const FileScan scan = scanImage(builder_.build());
    const auto slashed = std::find_if(scan.records.begin(), scan.records.end(),
                                      [](const FileRecord& r) { return r.entry.name == "vi/tim.bin"; });
    ASSERT_NE(slashed, scan.records.end());
    EXPECT_TRUE(slashed->entry.hasIssue(EntryIssue::InvalidName));
    EXPECT_FALSE(find(scan, "/bystander.txt")->entry.hasIssue(EntryIssue::InvalidName));
}

TEST_F(NtfsRecordCorruptionTest, AttributeListIsReportedNotFollowed) {
    // The data now "lives in extension records": the record has an attribute list and no $DATA.
    storeLe32(builder_.record(victim_.record), builder_.attributeOffset(victim_.record, kAttrData),
              kAttrAttributeList);
    const FileScan scan = scanImage(builder_.build());
    const FileRecord* record = find(scan, "/victim.bin");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->allocation.hasIssue(AllocationIssue::AttributeListNotFollowed));
    EXPECT_FALSE(record->allocation.hasIssue(AllocationIssue::DataAttributeMissing));
    EXPECT_EQ(record->allocation.method, AllocationMethod::None);
}

TEST_F(NtfsRecordCorruptionTest, ExtensionRecordsAreNotEntries) {
    storeLe64(builder_.record(victim_.record), 0x20, bystander_.record | (1ULL << 48));
    const FileScan scan = scanImage(builder_.build());
    EXPECT_EQ(find(scan, "/victim.bin"), nullptr);
    // Its clusters still belong to an in-use record.
    const Result<ClusterUsage> usage = mounted_.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(scan.referencedClusters, usage->allocated);
}

TEST(NtfsRecordLimitTest, ManyInvalidRecordsAreSummarized) {
    test::NtfsBuilderOptions options;
    options.mftRecords = 320;
    NtfsImageBuilder builder(options);
    std::vector<std::byte> image = builder.build();
    for (std::uint64_t n = 100; n < 250; ++n) {
        std::memcpy(image.data() + builder.recordOffset(n), "JUNK", 4);
    }
    const Mounted m = mount(std::move(image));
    const FileScan scan = scanOf(m);
    const auto invalid = std::count_if(scan.issues.begin(), scan.issues.end(),
                                       [](const ScanIssue& i) { return i.kind == ScanIssueKind::RecordInvalid; });
    EXPECT_EQ(invalid, 101);
    EXPECT_TRUE(hasIssue(scan, ScanIssueKind::RecordInvalid, "50 more invalid MFT records not listed"));
}

// ---------------------------------------------------------------------------
// Invalid run lists and sizes.
// ---------------------------------------------------------------------------

class NtfsRunCorruptionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Three runs, so the attribute has room for the edited run lists below.
        file_ = builder_.addFileInClusters(kRoot, "file.bin", data_, {3000, 3001, 3002, 3100, 3101, 3200});
    }

    const FileRecord& scanned() {
        mounted_ = mount(builder_.build());
        scan_ = scanOf(mounted_);
        const FileRecord* record = find(scan_, "/file.bin");
        if (record == nullptr) {
            throw std::runtime_error("file.bin not found");
        }
        checkInsideVolume(*record);
        return *record;
    }

    void checkInsideVolume(const FileRecord& record) const {
        const std::uint64_t end = builder_.clusterCount() * builder_.clusterSize();
        for (const Extent& extent : record.allocation.extents) {
            EXPECT_LE(extent.length, end - std::min(end, extent.offset));
            EXPECT_LT(extent.offset, end);
        }
    }

    NtfsImageBuilder builder_;
    std::vector<std::byte> data_ = test::makePattern(6 * 512 - 10, 3);
    NtfsImageBuilder::Entry file_;
    Mounted mounted_;
    FileScan scan_;
};

TEST_F(NtfsRunCorruptionTest, IntactRunsHaveNoIssues) {
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.issues.empty());
    EXPECT_EQ(test::readExtents(*mounted_.source, r.allocation.extents), data_);
}

TEST_F(NtfsRunCorruptionTest, ZeroLengthRun) {
    setRuns(builder_, file_.record, {0x21, 0x00, 0xB8, 0x0B, 0x00});
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::InvalidRunList));
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    EXPECT_TRUE(r.allocation.extents.empty());
}

TEST_F(NtfsRunCorruptionTest, OversizedFields) {
    setRuns(builder_, file_.record, {0x9F, 0x01, 0x00});
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::InvalidRunList));
    EXPECT_TRUE(r.allocation.extents.empty());
}

TEST_F(NtfsRunCorruptionTest, MissingTerminator) {
    const std::span<std::byte> runs = runBytes(builder_, file_.record);
    std::fill(runs.begin(), runs.end(), std::byte{0x01});  // one-cluster holes to the end of the attribute
    runs[0] = std::byte{0x21};
    runs[1] = std::byte{0x03};
    runs[2] = std::byte{0xB8};
    runs[3] = std::byte{0x0B};
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::InvalidRunList));
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::SparseRuns));
    ASSERT_FALSE(r.allocation.extents.empty());
    EXPECT_EQ(r.allocation.extents[0], (Extent{builder_.clusterOffset(3000), 3 * 512}));
}

TEST_F(NtfsRunCorruptionTest, RunOutsideTheVolume) {
    setRuns(builder_, file_.record, {0x21, 0x03, 0xB8, 0x0B, 0x31, 0x02, 0x00, 0x00, 0x10, 0x00});
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::InvalidClusterInChain));
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    ASSERT_EQ(r.allocation.fragmentCount(), 1u);  // the part before it is kept
    EXPECT_EQ(r.allocation.clusterCount, 3u);
}

TEST_F(NtfsRunCorruptionTest, RunCrossingTheEndOfTheVolumeIsClipped) {
    setRuns(builder_, file_.record, {0x21, 0x05, 0xFE, 0x1F, 0x00});  // clusters 8190-8194 of 8192
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::BeyondVolume));
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    ASSERT_EQ(r.allocation.fragmentCount(), 1u);
    EXPECT_EQ(r.allocation.extents[0], (Extent{builder_.clusterOffset(8190), 2 * 512}));
}

TEST_F(NtfsRunCorruptionTest, RunBeforeClusterZero) {
    setRuns(builder_, file_.record, {0x21, 0x03, 0xB8, 0x0B, 0x21, 0x02, 0x00, 0xE0, 0x00});  // 3000 - 8192
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::InvalidRunList));
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    EXPECT_EQ(r.allocation.clusterCount, 3u);
}

TEST_F(NtfsRunCorruptionTest, RunsDisagreeWithTheVcnRange) {
    const std::size_t attribute = builder_.attributeOffset(file_.record, kAttrData);
    storeLe64(builder_.record(file_.record), attribute + 24, 99);  // last VCN
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::InvalidRunList));
    EXPECT_EQ(test::readExtents(*mounted_.source, r.allocation.extents), data_);  // the runs themselves are fine
}

TEST_F(NtfsRunCorruptionTest, RunsShorterThanTheSize) {
    const std::size_t attribute = builder_.attributeOffset(file_.record, kAttrData);
    storeLe64(builder_.record(file_.record), attribute + 48, 20 * 512);  // real size
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    EXPECT_EQ(r.allocation.dataBytes(), 6u * 512u);
    EXPECT_EQ(r.entry.size, 20u * 512u);
    EXPECT_FALSE(r.entry.hasIssue(EntryIssue::ValidDataLengthExceedsSize));
}

TEST_F(NtfsRunCorruptionTest, SizeLargerThanTheVolume) {
    const std::size_t attribute = builder_.attributeOffset(file_.record, kAttrData);
    storeLe64(builder_.record(file_.record), attribute + 48, 1ULL << 40);
    storeLe64(builder_.record(file_.record), attribute + 56, 1ULL << 40);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::SizeExceedsVolume));
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    EXPECT_EQ(r.allocation.clusterCount, 6u);
}

TEST_F(NtfsRunCorruptionTest, InitializedSizeBeyondTheSize) {
    const std::size_t attribute = builder_.attributeOffset(file_.record, kAttrData);
    storeLe64(builder_.record(file_.record), attribute + 56, 100000);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.entry.hasIssue(EntryIssue::ValidDataLengthExceedsSize));
    EXPECT_EQ(r.entry.validDataLength, 100000u);
}

TEST_F(NtfsRunCorruptionTest, ActiveFileWithClustersMarkedFree) {
    builder_.setAllocated(3101, false);
    EXPECT_TRUE(scanned().allocation.hasIssue(AllocationIssue::ClustersMarkedFree));
}

TEST(NtfsCrossLinkTest, OverlappingFilesAreCrossLinked) {
    NtfsImageBuilder builder;
    (void)builder.addFileInClusters(kRoot, "a.bin", test::makePattern(3 * 512, 1), {3000, 3001, 3002});
    const auto b = builder.addFileInClusters(kRoot, "b.bin", test::makePattern(3 * 512, 2), {3100, 3101, 3102});
    (void)builder.addFileInClusters(kRoot, "c.bin", test::makePattern(512, 3), {3200});
    setRuns(builder, b.record, {0x21, 0x03, 0xB9, 0x0B, 0x00});  // b now claims 3001-3003
    builder.setAllocated(3003, true);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_EQ(scan.crossLinkedClusters, 2u);
    EXPECT_TRUE(find(scan, "/a.bin")->allocation.hasIssue(AllocationIssue::CrossLinked));
    EXPECT_TRUE(find(scan, "/b.bin")->allocation.hasIssue(AllocationIssue::CrossLinked));
    EXPECT_FALSE(find(scan, "/c.bin")->allocation.hasIssue(AllocationIssue::CrossLinked));
    // b's original clusters are allocated but no longer referenced.
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->allocated - scan.referencedClusters, 3u);
}

// ---------------------------------------------------------------------------
// Directory structure.
// ---------------------------------------------------------------------------

// Rewrites the parent reference of a record's first $FILE_NAME.
void setParent(NtfsImageBuilder& builder, std::uint64_t record, std::uint64_t parentRecord, std::uint16_t sequence) {
    const std::size_t value = builder.attributeOffset(record, kAttrFileName) + 24;
    storeLe64(builder.record(record), value, parentRecord | (static_cast<std::uint64_t>(sequence) << 48));
}

TEST(NtfsDirectoryTest, ParentCycleTerminates) {
    NtfsImageBuilder builder;
    const auto a = builder.addDirectory(kRoot, "A");
    const auto b = builder.addDirectory(a.record, "B");
    (void)builder.addFile(b.record, "inside.txt", test::makePattern(10, 1));
    setParent(builder, a.record, b.record, builder.sequence(b.record));  // A in B, B in A
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_EQ(find(scan, "/A"), nullptr);
    const FileRecord* orphan = find(scan, "/$OrphanFiles/A");
    ASSERT_NE(orphan, nullptr);
    EXPECT_TRUE(orphan->entry.hasIssue(EntryIssue::ParentMissing));
    EXPECT_NE(find(scan, "/$OrphanFiles/A/B/inside.txt"), nullptr);
    EXPECT_TRUE(hasIssue(scan, ScanIssueKind::DirectoryLoop));
}

TEST(NtfsDirectoryTest, DirectoryWithTwoNamesIsEnteredOnce) {
    NtfsImageBuilder builder;
    const auto one = builder.addDirectory(kRoot, "one");
    const auto shared = builder.addDirectory(one.record, "shared");
    (void)builder.addFile(shared.record, "f.txt", test::makePattern(10, 1));
    builder.addHardLink(shared, kRoot, "alias");  // directories cannot have hard links
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_TRUE(hasIssue(scan, ScanIssueKind::DirectoryLoop));
    EXPECT_EQ(std::count_if(scan.records.begin(), scan.records.end(),
                            [](const FileRecord& r) { return r.entry.name == "f.txt"; }),
              1);
}

TEST(NtfsDirectoryTest, SubdirectoryPointingToRootIsNotFollowedTwice) {
    NtfsImageBuilder builder;
    const auto directory = builder.addDirectory(kRoot, "dir");
    builder.addHardLink(NtfsImageBuilder::Entry{kRoot, kRoot, {}, true}, directory.record, "root again");
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_TRUE(scan.complete);
    EXPECT_NE(find(scan, "/dir/root again"), nullptr);
    EXPECT_TRUE(hasIssue(scan, ScanIssueKind::DirectoryLoop));
}

TEST(NtfsDirectoryTest, ParentThatIsAFileOrMissing) {
    NtfsImageBuilder builder;
    const auto file = builder.addFile(kRoot, "plain.txt", test::makePattern(10, 1));
    const auto child = builder.addFile(kRoot, "child.txt", test::makePattern(10, 2));
    const auto lost = builder.addFile(kRoot, "lost.txt", test::makePattern(10, 3));
    setParent(builder, child.record, file.record, builder.sequence(file.record));
    setParent(builder, lost.record, 0x0000FFFFFFFFFFFFULL, 1);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    for (const std::string path : {"/$OrphanFiles/child.txt", "/$OrphanFiles/lost.txt"}) {
        const FileRecord* record = find(scan, path);
        ASSERT_NE(record, nullptr) << path;
        EXPECT_EQ(record->entry.state, EntryState::Active);  // corruption, not deletion
        EXPECT_TRUE(record->entry.hasIssue(EntryIssue::ParentMissing));
    }
}

TEST(NtfsDirectoryTest, DamagedRootRecord) {
    NtfsImageBuilder builder;
    (void)builder.addFile(kRoot, "top.txt", test::makePattern(10, 1));
    std::vector<std::byte> image = builder.build();
    std::memcpy(image.data() + builder.recordOffset(kRootRecord), "JUNK", 4);
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_TRUE(hasWarning(m.fs->info(), "root directory record is unusable"));
    const FileScan scan = scanOf(m);
    EXPECT_TRUE(hasIssue(scan, ScanIssueKind::DirectoryInvalid, "root directory"));
    EXPECT_NE(find(scan, "/$OrphanFiles/top.txt"), nullptr);
    EXPECT_NE(find(scan, "/$OrphanFiles/$MFT"), nullptr);
    RECOVERY_EXPECT_ERROR(m.fs->readDirectory(m.fs->rootDirectory()), ErrorCode::CorruptedFilesystem);
}

// ---------------------------------------------------------------------------
// System files and unreadable sectors.
// ---------------------------------------------------------------------------

TEST(NtfsSystemFileTest, MftRecordAndMirrorBothDamaged) {
    NtfsImageBuilder builder;
    std::vector<std::byte> image = builder.build();
    std::memcpy(image.data() + builder.recordOffset(0), "JUNK", 4);
    std::memcpy(image.data() + builder.clusterOffset(builder.mftMirrorCluster()), "JUNK", 4);
    test::MemoryStorageSource source(image, 512);
    RECOVERY_ASSERT_OK(source.open());
    RECOVERY_EXPECT_ERROR(NtfsFilesystem::open(source), ErrorCode::CorruptedFilesystem);
}

TEST(NtfsSystemFileTest, MftDataThatDoesNotStartAtTheMftIsRejected) {
    NtfsImageBuilder builder;
    // Record 0 (both copies) claims the MFT starts one cluster later than the
    // boot sector says: 256 clusters (two length bytes) at mftCluster + 1.
    setRuns(builder, kMftRecord, {0x12, 0x00, 0x01, static_cast<int>(builder.mftCluster() + 1), 0x00});
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    RECOVERY_EXPECT_ERROR(NtfsFilesystem::open(source), ErrorCode::CorruptedFilesystem);
}

TEST(NtfsSystemFileTest, MissingClusterBitmap) {
    NtfsImageBuilder builder;
    const auto file = builder.addFileInClusters(kRoot, "f.bin", test::makePattern(512, 1), {3000});
    std::vector<std::byte> image = builder.build();
    std::memcpy(image.data() + builder.recordOffset(kBitmapRecord), "JUNK", 4);
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_FALSE(m.fs->hasClusterBitmap());
    EXPECT_TRUE(hasWarning(m.fs->info(), "cluster allocation is unknown"));
    EXPECT_EQ(m.fs->clusterState(ClusterNumber{3000}).value(), ClusterState::Unreadable);
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->unreadable, usage->total);
    const FileScan scan = scanOf(m);
    const FileRecord* record = find(scan, "/f.bin");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->allocation.hasIssue(AllocationIssue::UnreadableAllocationTable));
    EXPECT_EQ(test::readExtents(*m.source, record->allocation.extents), test::makePattern(512, 1));
    EXPECT_EQ(file.clusters.size(), 1u);
}

TEST(NtfsSystemFileTest, DamagedVolumeRecord) {
    NtfsImageBuilder builder;
    std::vector<std::byte> image = builder.build();
    std::memcpy(image.data() + builder.recordOffset(kVolumeRecord), "JUNK", 4);
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_TRUE(hasWarning(m.fs->info(), "$Volume record is unusable"));
    EXPECT_EQ(m.fs->info().label, "");
    EXPECT_EQ(m.fs->majorVersion(), 0u);
}

TEST(NtfsSystemFileTest, DirtyVolumeAndOtherVersionsAreWarnings) {
    NtfsImageBuilder builder;
    const std::size_t information = builder.attributeOffset(kVolumeRecord, kAttrVolumeInformation) + 24;
    std::span<std::byte> plain = builder.record(kVolumeRecord);
    plain[information + 8] = std::byte{1};
    plain[information + 9] = std::byte{2};
    storeLe16(plain, information + 10, 0x0001);
    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    EXPECT_TRUE(hasWarning(m.fs->info(), "dirty"));
    EXPECT_TRUE(hasWarning(m.fs->info(), "NTFS version 1.2"));
}

TEST(NtfsSystemFileTest, UnreadableMftSectors) {
    NtfsImageBuilder builder;
    const auto first = builder.addFile(kRoot, "first.txt", test::makePattern(10, 1));
    const auto second = builder.addFile(kRoot, "second.txt", test::makePattern(10, 2));
    const std::vector<std::byte> image = builder.build();
    test::MemoryStorageSource source(image, 512);
    source.addBadSector(builder.recordOffset(first.record) / 512 + 1);  // the record's second sector
    RECOVERY_ASSERT_OK(source.open());
    Result<std::unique_ptr<NtfsFilesystem>> fs = NtfsFilesystem::open(source);
    RECOVERY_ASSERT_OK(fs);
    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    EXPECT_EQ(find(*scan, "/first.txt"), nullptr);
    EXPECT_NE(find(*scan, "/second.txt"), nullptr);
    EXPECT_TRUE(hasIssue(*scan, ScanIssueKind::RecordUnreadable, "1 unreadable MFT record(s), the first is record 64"));
    RECOVERY_EXPECT_ERROR(fs.value()->readRecord(first.record), ErrorCode::IoError);
    EXPECT_EQ(second.record, first.record + 1);
}

TEST(NtfsSystemFileTest, UnreadableBitmapSector) {
    NtfsImageBuilder builder;
    (void)builder.addFileInClusters(kRoot, "f.bin", test::makePattern(512, 1), {4200});  // bitmap byte 525
    const std::vector<std::byte> image = builder.build();
    test::MemoryStorageSource source(image, 512);
    const Mounted probe = mount(image);
    const std::uint64_t bitmapCluster = probe.fs->readRecord(kBitmapRecord)->find(kAttrData)->runs.runs[0].lcn.value();
    source.addBadSector(bitmapCluster + 1);  // bitmap bytes 512-1023: clusters 4096-8191
    RECOVERY_ASSERT_OK(source.open());
    Result<std::unique_ptr<NtfsFilesystem>> fs = NtfsFilesystem::open(source);
    RECOVERY_ASSERT_OK(fs);
    EXPECT_EQ(fs.value()->clusterState(ClusterNumber{4200}).value(), ClusterState::Unreadable);
    EXPECT_EQ(fs.value()->clusterState(ClusterNumber{10}).value(), ClusterState::Allocated);
    const Result<ClusterUsage> usage = fs.value()->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->unreadable, 4096u);
    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    EXPECT_TRUE(find(*scan, "/f.bin")->allocation.hasIssue(AllocationIssue::UnreadableAllocationTable));
}

TEST(NtfsSystemFileTest, TruncatedImage) {
    NtfsImageBuilder builder;
    const auto early = builder.addFile(kRoot, "early.txt", test::makePattern(10, 1));
    std::vector<std::byte> image = builder.build();
    // Cut the image inside the MFT, after record 80.
    image.resize(static_cast<std::size_t>(builder.recordOffset(80)));
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_TRUE(hasWarning(m.fs->info(), "truncated"));
    const FileScan scan = scanOf(m);
    EXPECT_NE(find(scan, "/early.txt"), nullptr);
    EXPECT_TRUE(hasIssue(scan, ScanIssueKind::RecordUnreadable, "48 unreadable MFT record(s), the first is record 80"));
    RECOVERY_EXPECT_OK(m.fs->analyzeClusters({}));
    EXPECT_EQ(early.record, 64u);
}

// ---------------------------------------------------------------------------
// Hostile boot sectors and MFT sizes.
// ---------------------------------------------------------------------------

TEST(NtfsHostileTest, HugeClaimedVolumeAndMftOnATinyImage) {
    NtfsImageBuilder builder;
    (void)builder.addFile(kRoot, "a.bin", test::makePattern(1000, 1));
    // $MFT claims 2^40 bytes (2^30 records) in one run of 2^31 clusters.
    const std::size_t data = builder.attributeOffset(kMftRecord, kAttrData);
    std::span<std::byte> plain = builder.record(kMftRecord);
    storeLe64(plain, data + 24, (1ULL << 31) - 1);
    storeLe64(plain, data + 40, 1ULL << 40);
    storeLe64(plain, data + 48, 1ULL << 40);
    storeLe64(plain, data + 56, 1ULL << 40);
    const std::span<std::byte> runs = runBytes(builder, kMftRecord);
    std::fill(runs.begin(), runs.end(), std::byte{0});
    runs[0] = std::byte{0x15};  // 5-byte length, 1-byte offset
    storeLe32(runs, 1, 0);
    runs[5] = std::byte{0};
    runs[4] = std::byte{0x80};  // length 2^31
    runs[6] = static_cast<std::byte>(builder.mftCluster());
    std::vector<std::byte> image = builder.build();
    storeLe64(image, 40, 1ULL << 42);  // total sectors: 2^42 clusters of one sector
    test::MemoryStorageSource source(image, 512);
    RECOVERY_ASSERT_OK(source.open());
    Result<std::unique_ptr<NtfsFilesystem>> fs = NtfsFilesystem::open(source);
    RECOVERY_ASSERT_OK(fs);
    EXPECT_EQ(fs.value()->recordCount(), NtfsOptions{}.maxRecords);
    EXPECT_TRUE(hasWarning(fs.value()->info(), "only the first"));
    // Whatever is found, every operation is bounded by what the image holds.
    RECOVERY_EXPECT_OK(fs.value()->scan({}, {}));
    RECOVERY_EXPECT_OK(fs.value()->analyzeClusters({}));
    EXPECT_LT(source.readCount(), 20000u);
}

TEST(NtfsHostileTest, MaxRecordsBoundsTheMft) {
    NtfsImageBuilder builder;
    (void)builder.addFile(kRoot, "a.bin", test::makePattern(10, 1));
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    NtfsOptions options;
    options.maxRecords = 20;
    Result<std::unique_ptr<NtfsFilesystem>> fs = NtfsFilesystem::open(source, options);
    RECOVERY_ASSERT_OK(fs);
    EXPECT_EQ(fs.value()->recordCount(), 20u);
    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    EXPECT_EQ(find(*scan, "/a.bin"), nullptr);  // record 64 is beyond the limit
}

// ---------------------------------------------------------------------------
// Randomised corruption of the boot sector, MFT, mirror and bitmap.
// ---------------------------------------------------------------------------

std::vector<std::byte> richImage(NtfsImageBuilder& builder) {
    const auto dcim = builder.addDirectory(kRoot, "DCIM", 2);
    const auto camera = builder.addDirectory(dcim.record, "100 Camera");
    for (int i = 0; i < 12; ++i) {
        const auto file = builder.addFile(i % 2 == 0 ? camera.record : kRoot,
                                          "Picture number " + std::to_string(i) + ".jpg",
                                          test::makePattern(400 * static_cast<std::size_t>(i + 1), i));
        if (i % 3 == 0) {
            builder.deleteEntry(file);
        }
    }
    (void)builder.addFileInClusters(kRoot, "frag.bin", test::makePattern(4 * 512, 9), {5000, 5001, 5050, 5020});
    const auto old = builder.addDirectory(kRoot, "Old stuff");
    const auto note = builder.addFile(old.record, "note.txt", test::makePattern(900, 5));
    builder.deleteEntry(note);
    builder.deleteEntry(old);
    return builder.build();
}

void checkInvariants(const FileScan& scan, const FilesystemInfo& info, std::uint32_t recordSize) {
    const std::uint64_t volumeEnd = info.clusterCount * info.clusterSize;
    for (const FileRecord& r : scan.records) {
        for (const Extent& extent : r.allocation.extents) {
            ASSERT_LT(extent.offset, volumeEnd) << r.path;
            ASSERT_LE(extent.length, volumeEnd - extent.offset) << r.path;
        }
        ASSERT_LE(r.allocation.clusterCount, info.clusterCount) << r.path;
        ASSERT_LE(r.allocation.residentData.size(), recordSize) << r.path;
    }
}

TEST(NtfsFuzzTest, RandomCorruptionNeverEscapesTheVolume) {
    NtfsImageBuilder builder;
    const std::vector<std::byte> pristine = richImage(builder);
    const std::uint64_t mftStart = builder.recordOffset(0);
    const std::uint64_t userRecords = builder.recordOffset(64);
    const std::uint64_t mirror = builder.clusterOffset(builder.mftMirrorCluster());
    const std::uint64_t backup = builder.clusterCount() * builder.clusterSize();

    std::mt19937_64 random(0x4E7F5);
    int opened = 0;
    for (int iteration = 0; iteration < 400; ++iteration) {
        std::vector<std::byte> image = pristine;
        const int mutations = 1 + static_cast<int>(random() % 24);
        for (int i = 0; i < mutations; ++i) {
            std::uint64_t at = 0;
            switch (random() % 6) {
            case 0:
                at = random() % 512;  // boot sector
                break;
            case 1:
                at = mftStart + random() % (16 * 1024);  // system records
                break;
            case 2:
                at = mirror + random() % 4096;
                break;
            case 3:
                at = backup + random() % 512;
                break;
            default:
                at = userRecords + random() % (32 * 1024);  // user records
                break;
            }
            image[at] = static_cast<std::byte>(random());
        }

        test::MemoryStorageSource source(image, 512);
        ASSERT_TRUE(source.open().ok());
        Result<std::unique_ptr<NtfsFilesystem>> fs = NtfsFilesystem::open(source);
        if (!fs.ok()) {
            ASSERT_TRUE(fs.error().code == ErrorCode::UnsupportedFilesystem ||
                        fs.error().code == ErrorCode::CorruptedFilesystem)
                << describe(fs.error());
            continue;
        }
        ++opened;
        const Result<FileScan> scan = fs.value()->scan({}, {});
        ASSERT_TRUE(scan.ok()) << describe(scan.error());
        checkInvariants(scan.value(), fs.value()->info(), fs.value()->bootSector().recordSize);
        ASSERT_TRUE(fs.value()->analyzeClusters({}).ok());
        for (const FileRecord& record : scan->records) {
            // Re-reading a record must agree with, or fail as safely as, the scan.
            const Result<FileAllocation> resolved = fs.value()->resolveAllocation(record.entry);
            if (resolved.ok()) {
                ASSERT_LE(resolved->extents.size(), 4096u);
            }
        }
        if (HasFatalFailure()) {
            FAIL() << "iteration " << iteration;
        }
    }
    EXPECT_GT(opened, 250) << "most mutations should leave the volume mountable (the backups help)";
}

}  // namespace
}  // namespace recovery::filesystem::ntfs
