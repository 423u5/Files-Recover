// FAT32 filesystem: normal, deleted, fragmented and Unicode-named files on
// generated images, read through the filesystem-independent interface.

#include "filesystem/fat32/fat32_filesystem.hpp"

#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/byte_order.hpp"
#include "support/fat32_builder.hpp"
#include "support/memory_source.hpp"
#include "support/partition_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>

namespace recovery::filesystem::fat32 {
namespace {

using test::Fat32ImageBuilder;

std::vector<std::byte> bytesOf(std::size_t size, std::uint64_t seed) {
    return test::makePattern(size, seed);
}

// Opens an image and keeps the source alive alongside the filesystem.
struct Mounted {
    std::unique_ptr<test::MemoryStorageSource> source;
    std::unique_ptr<Fat32Filesystem> fs;

    [[nodiscard]] FileScan scan(ScanLimits limits = {}) const {
        Result<FileScan> result = fs->scan(limits, {});
        EXPECT_TRUE(result.ok()) << (result.ok() ? "" : describe(result.error()));
        return result.ok() ? std::move(result).value() : FileScan{};
    }
};

Mounted mount(std::vector<std::byte> image, std::uint32_t sectorSize = 512) {
    Mounted m;
    m.source = std::make_unique<test::MemoryStorageSource>(std::move(image), sectorSize);
    EXPECT_TRUE(m.source->open().ok());
    Result<std::unique_ptr<Fat32Filesystem>> fs = Fat32Filesystem::open(*m.source);
    EXPECT_TRUE(fs.ok()) << (fs.ok() ? "" : describe(fs.error()));
    if (fs.ok()) {
        m.fs = std::move(fs).value();
    }
    return m;
}

const FileRecord* find(const FileScan& scan, std::string_view path) {
    for (const FileRecord& record : scan.records) {
        if (record.path == path) {
            return &record;
        }
    }
    return nullptr;
}

TEST(Fat32FilesystemTest, OpensAndReportsInfo) {
    Fat32ImageBuilder builder;
    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    const FilesystemInfo& info = m.fs->info();
    EXPECT_EQ(info.type, FilesystemType::Fat32);
    EXPECT_EQ(info.label, "TESTVOL");
    EXPECT_EQ(info.serialNumber, 0x1234ABCDu);
    EXPECT_EQ(info.bytesPerSector, 512u);
    EXPECT_EQ(info.clusterSize, 512u);
    EXPECT_EQ(info.clusterCount, 2048u);
    EXPECT_EQ(info.firstCluster, 2u);
    EXPECT_EQ(info.dataOffset, builder.clusterOffset(2));
    EXPECT_FALSE(info.warnings.empty());  // small, non-standard cluster count
}

TEST(Fat32FilesystemTest, FindsNormalFilesAndDirectories) {
    Fat32ImageBuilder builder;
    const auto small = bytesOf(100, 1);
    const auto multi = bytesOf(5000, 2);
    const auto nested = bytesOf(1234, 3);
    (void)builder.addFile(builder.root(), "SMALL.TXT", small);
    (void)builder.addFile(builder.root(), "MULTI.BIN", multi);
    const auto dcim = builder.addDirectory(builder.root(), "DCIM");
    const auto camera = builder.addDirectory(dcim.clusters[0], "100MEDIA");
    (void)builder.addFile(camera.clusters[0], "IMG_0001.JPG", nested);
    const Mounted m = mount(builder.build());

    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.complete);
    EXPECT_TRUE(scan.issues.empty()) << scan.issues.front().detail;
    EXPECT_EQ(scan.crossLinkedClusters, 0u);
    ASSERT_EQ(scan.records.size(), 5u);

    const std::map<std::string, const std::vector<std::byte>*> expected = {
        {"/SMALL.TXT", &small}, {"/MULTI.BIN", &multi}, {"/DCIM/100MEDIA/IMG_0001.JPG", &nested}};
    for (const auto& [path, data] : expected) {
        const FileRecord* record = find(scan, path);
        ASSERT_NE(record, nullptr) << path;
        EXPECT_EQ(record->entry.state, EntryState::Active);
        EXPECT_FALSE(record->entry.isDirectory);
        EXPECT_EQ(record->entry.size, data->size());
        EXPECT_EQ(record->allocation.method, AllocationMethod::ClusterChain);
        EXPECT_TRUE(record->allocation.issues.empty()) << path << ": " << toString(record->allocation.issues.front());
        EXPECT_EQ(test::readExtents(*m.source, record->allocation.extents), *data) << path;
    }

    const FileRecord* directory = find(scan, "/DCIM/100MEDIA");
    ASSERT_NE(directory, nullptr);
    EXPECT_TRUE(directory->entry.isDirectory);
    EXPECT_EQ(directory->entry.name, "100MEDIA");
    // Root + 2 directories + 1 + 10 + 3 data clusters.
    EXPECT_EQ(scan.referencedClusters, 1u + 2u + 1u + 10u + 3u);
}

TEST(Fat32FilesystemTest, ShortNamesWithoutLongNames) {
    Fat32ImageBuilder builder;
    const auto e = builder.addFile(builder.root(), "IMG_0001.JPG", bytesOf(10, 1));
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    ASSERT_EQ(scan.records.size(), 1u);
    EXPECT_EQ(scan.records[0].entry.name, "IMG_0001.JPG");
    EXPECT_EQ(scan.records[0].entry.shortName, "IMG_0001.JPG");
    EXPECT_EQ(scan.records[0].entry.metadataOffset, builder.shortEntryOffset(e));
    EXPECT_TRUE(scan.records[0].entry.issues.empty());
}

TEST(Fat32FilesystemTest, WindowsLowercaseFlags) {
    Fat32ImageBuilder builder;
    const auto e = builder.addFile(builder.root(), "README.TXT", bytesOf(10, 1));
    builder.shortEntry(e)[12] = std::byte{0x18};
    const Mounted m = mount(builder.build());
    EXPECT_EQ(m.scan().records.at(0).entry.name, "readme.txt");
}

TEST(Fat32FilesystemTest, LongAndUnicodeNames) {
    Fat32ImageBuilder builder;
    const std::string unicode = "\xC3\x9Cn\xC3\xAF" "c\xC3\xB8" "d\xC3\xA9 \xE5\x86\x99\xE7\x9C\x9F \xF0\x9F\x98\x80.jpeg";
    const std::string longAscii = "Holiday photos from the summer of 2024 - beach day number one (final edit).mp4";
    const std::string exactly13 = "abcdefghijklm";  // fills one long-name slot with no terminator
    (void)builder.addFile(builder.root(), unicode, bytesOf(700, 1));
    (void)builder.addFile(builder.root(), longAscii, bytesOf(10, 2));
    (void)builder.addFile(builder.root(), exactly13, bytesOf(10, 3));
    (void)builder.addFile(builder.root(), "mixedCase.Txt", bytesOf(10, 4));
    const Mounted m = mount(builder.build());

    const FileScan scan = m.scan();
    ASSERT_EQ(scan.records.size(), 4u);
    EXPECT_EQ(scan.records[0].entry.name, unicode);
    EXPECT_EQ(scan.records[0].path, "/" + unicode);
    EXPECT_NE(scan.records[0].entry.shortName.find("~1"), std::string::npos);
    EXPECT_EQ(scan.records[1].entry.name, longAscii);
    EXPECT_EQ(scan.records[1].entry.shortName, "HOLIDA~1.MP4");
    EXPECT_EQ(scan.records[2].entry.name, exactly13);
    EXPECT_EQ(scan.records[3].entry.name, "mixedCase.Txt");
    for (const FileRecord& record : scan.records) {
        EXPECT_TRUE(record.entry.issues.empty()) << record.entry.name;
    }
}

TEST(Fat32FilesystemTest, DecodesTimestampsAndAttributes) {
    Fat32ImageBuilder builder;
    const auto e = builder.addFile(builder.root(), "A.TXT", bytesOf(10, 1));
    builder.shortEntry(e)[11] = std::byte{0x20 | 0x01 | 0x02};
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const DirectoryEntry& entry = scanned.records.at(0).entry;

    using namespace std::chrono;
    const auto day = sys_days{year{2024} / 5 / 17};
    ASSERT_TRUE(entry.modified.has_value());
    EXPECT_EQ(entry.modified->time, day + hours{13} + minutes{45} + seconds{30});
    EXPECT_TRUE(entry.modified->local);
    ASSERT_TRUE(entry.created.has_value());
    EXPECT_EQ(entry.created->time, day + hours{13} + minutes{45} + seconds{30} + milliseconds{500});
    ASSERT_TRUE(entry.accessed.has_value());
    EXPECT_EQ(entry.accessed->time, time_point_cast<milliseconds>(day));
    EXPECT_TRUE(entry.attributes.readOnly);
    EXPECT_TRUE(entry.attributes.hidden);
    EXPECT_TRUE(entry.attributes.archive);
    EXPECT_FALSE(entry.attributes.system);
}

TEST(Fat32FilesystemTest, InvalidTimestampIsFlagged) {
    Fat32ImageBuilder builder;
    const auto e = builder.addFile(builder.root(), "A.TXT", bytesOf(10, 1));
    storeLe16(builder.shortEntry(e), 24, (44 << 9) | (13 << 5) | 1);  // month 13
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const DirectoryEntry& entry = scanned.records.at(0).entry;
    EXPECT_FALSE(entry.modified.has_value());
    EXPECT_TRUE(entry.hasIssue(EntryIssue::InvalidTimestamp));
}

TEST(Fat32FilesystemTest, EmptyFile) {
    Fat32ImageBuilder builder;
    (void)builder.addFile(builder.root(), "EMPTY.TXT", {});
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const FileRecord& record = scanned.records.at(0);
    EXPECT_EQ(record.entry.size, 0u);
    EXPECT_EQ(record.allocation.method, AllocationMethod::None);
    EXPECT_TRUE(record.allocation.extents.empty());
    EXPECT_TRUE(record.allocation.issues.empty());
}

TEST(Fat32FilesystemTest, FragmentedFileFollowsTheChain) {
    Fat32ImageBuilder builder;
    const auto data = bytesOf(6 * 512 - 100, 7);
    (void)builder.addFileInClusters(builder.root(), "FRAG.BIN", data, {40, 41, 42, 90, 91, 60});
    const Mounted m = mount(builder.build());

    const FileScan scanned = m.scan();

    const FileRecord& record = scanned.records.at(0);
    EXPECT_EQ(record.allocation.method, AllocationMethod::ClusterChain);
    EXPECT_TRUE(record.allocation.issues.empty());
    ASSERT_EQ(record.allocation.fragmentCount(), 3u);
    EXPECT_EQ(record.allocation.extents[0], (Extent{builder.clusterOffset(40), 3 * 512}));
    EXPECT_EQ(record.allocation.extents[1], (Extent{builder.clusterOffset(90), 2 * 512}));
    EXPECT_EQ(record.allocation.extents[2], (Extent{builder.clusterOffset(60), 512 - 100}));
    EXPECT_EQ(record.allocation.clusterCount, 6u);
    EXPECT_EQ(test::readExtents(*m.source, record.allocation.extents), data);
}

TEST(Fat32FilesystemTest, LargeDirectorySpanningClusters) {
    Fat32ImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "Many files");
    for (int i = 0; i < 60; ++i) {
        (void)builder.addFile(folder.clusters[0], "photo number " + std::to_string(i) + ".jpg", bytesOf(10, 1));
    }
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    EXPECT_EQ(scan.records.size(), 61u);
    EXPECT_NE(find(scan, "/Many files/photo number 59.jpg"), nullptr);
    const FileRecord* directory = find(scan, "/Many files");
    ASSERT_NE(directory, nullptr);
    EXPECT_GT(directory->allocation.clusterCount, 1u);
}

// ---------------------------------------------------------------------------
// Deleted entries
// ---------------------------------------------------------------------------

TEST(Fat32FilesystemTest, DeletedFileWithLongName) {
    Fat32ImageBuilder builder;
    const auto data = bytesOf(3000, 11);
    const auto e = builder.addFile(builder.root(), "Vacation photo.jpg", data);
    (void)builder.addFile(builder.root(), "KEEP.TXT", bytesOf(10, 12));
    builder.deleteEntry(e);
    const Mounted m = mount(builder.build());

    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/Vacation photo.jpg");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->entry.state, EntryState::Deleted);
    EXPECT_EQ(record->entry.shortName, "VACATI~1.JPG");  // first character recovered via the checksum
    EXPECT_TRUE(record->entry.issues.empty()) << toString(record->entry.issues.front());
    EXPECT_EQ(record->entry.size, data.size());
    EXPECT_EQ(record->allocation.method, AllocationMethod::ContiguousGuess);
    EXPECT_TRUE(record->allocation.issues.empty());
    EXPECT_EQ(test::readExtents(*m.source, record->allocation.extents), data);
    EXPECT_EQ(find(scan, "/KEEP.TXT")->entry.state, EntryState::Active);
}

TEST(Fat32FilesystemTest, DeletedShortNameLosesFirstCharacter) {
    Fat32ImageBuilder builder;
    const auto e = builder.addFile(builder.root(), "PHOTO.JPG", bytesOf(100, 1));
    builder.deleteEntry(e);
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const FileRecord& record = scanned.records.at(0);
    EXPECT_EQ(record.entry.name, "_HOTO.JPG");
    EXPECT_TRUE(record.entry.hasIssue(EntryIssue::NameReconstructed));
}

TEST(Fat32FilesystemTest, DeletedUnicodeName) {
    Fat32ImageBuilder builder;
    const std::string name = "\xC3\xA9t\xC3\xA9 \xF0\x9F\x8C\x9E.png";
    const auto e = builder.addFile(builder.root(), name, bytesOf(100, 1));
    builder.deleteEntry(e);
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const FileRecord& record = scanned.records.at(0);
    EXPECT_EQ(record.entry.name, name);
    EXPECT_EQ(record.entry.state, EntryState::Deleted);
    EXPECT_FALSE(record.entry.hasIssue(EntryIssue::LongNameUnverified));  // '_' basis verified
}

TEST(Fat32FilesystemTest, DeletedFileWhoseClustersWereReused) {
    Fat32ImageBuilder builder;
    const auto old = builder.addFile(builder.root(), "OLD.BIN", bytesOf(2048, 1));
    builder.deleteEntry(old);
    (void)builder.addFileInClusters(builder.root(), "NEW.BIN", bytesOf(1024, 2), {old.clusters[2], old.clusters[3]});
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/_LD.BIN");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->allocation.hasIssue(AllocationIssue::ClustersInUse));
    EXPECT_TRUE(find(scan, "/NEW.BIN")->allocation.issues.empty());
}

TEST(Fat32FilesystemTest, DeletedFragmentedFileIsOnlyAGuess) {
    Fat32ImageBuilder builder;
    const auto data = bytesOf(3 * 512, 5);
    const auto e = builder.addFileInClusters(builder.root(), "FRAG.BIN", data, {50, 51, 70});
    builder.deleteEntry(e);
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const FileRecord& record = scanned.records.at(0);
    // The chain is gone: the engine can only guess contiguity, which is wrong
    // here. Fragment reconstruction (P13) has to resolve such files.
    EXPECT_EQ(record.allocation.method, AllocationMethod::ContiguousGuess);
    ASSERT_EQ(record.allocation.extents.size(), 1u);
    EXPECT_EQ(record.allocation.extents[0], (Extent{builder.clusterOffset(50), 3 * 512}));
    EXPECT_NE(test::readExtents(*m.source, record.allocation.extents), data);
}

TEST(Fat32FilesystemTest, FilesInsideDeletedDirectory) {
    Fat32ImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "Old Photos");
    const auto data = bytesOf(1500, 21);
    const auto inner = builder.addFile(folder.clusters[0], "beach.jpg", data);
    builder.deleteEntry(inner);
    builder.deleteEntry(folder);
    const Mounted m = mount(builder.build());

    const FileScan scan = m.scan();
    const FileRecord* directory = find(scan, "/Old Photos");
    ASSERT_NE(directory, nullptr);
    EXPECT_EQ(directory->entry.state, EntryState::Deleted);
    const FileRecord* file = find(scan, "/Old Photos/beach.jpg");
    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(file->parentDeleted);
    EXPECT_EQ(file->entry.state, EntryState::Deleted);
    EXPECT_EQ(test::readExtents(*m.source, file->allocation.extents), data);

    ScanLimits noDeletedDirs;
    noDeletedDirs.recurseIntoDeletedDirectories = false;
    EXPECT_EQ(find(m.scan(noDeletedDirs), "/Old Photos/beach.jpg"), nullptr);
}

TEST(Fat32FilesystemTest, DeletedDirectoryWhoseClusterWasReused) {
    Fat32ImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "GONE");
    builder.deleteEntry(folder);
    (void)builder.addFileInClusters(builder.root(), "NEW.BIN", bytesOf(100, 1), {folder.clusters[0]});
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    ASSERT_EQ(scan.issues.size(), 1u);
    EXPECT_EQ(scan.issues[0].kind, ScanIssueKind::DirectoryInvalid);
    EXPECT_EQ(scan.issues[0].path, "/_ONE");
}

TEST(Fat32FilesystemTest, ExcludingDeletedEntries) {
    Fat32ImageBuilder builder;
    const auto e = builder.addFile(builder.root(), "GONE.TXT", bytesOf(10, 1));
    (void)builder.addFile(builder.root(), "HERE.TXT", bytesOf(10, 1));
    builder.deleteEntry(e);
    const Mounted m = mount(builder.build());
    ScanLimits limits;
    limits.includeDeleted = false;
    const FileScan scan = m.scan(limits);
    ASSERT_EQ(scan.records.size(), 1u);
    EXPECT_EQ(scan.records[0].path, "/HERE.TXT");
}

// ---------------------------------------------------------------------------
// Different geometries
// ---------------------------------------------------------------------------

struct Geometry {
    std::uint16_t bytesPerSector;
    std::uint8_t sectorsPerCluster;
    std::uint32_t clusterCount;
};

class Fat32GeometryTest : public ::testing::TestWithParam<Geometry> {};

TEST_P(Fat32GeometryTest, FilesRoundTrip) {
    const Geometry g = GetParam();
    test::Fat32BuilderOptions options;
    options.bytesPerSector = g.bytesPerSector;
    options.sectorsPerCluster = g.sectorsPerCluster;
    options.clusterCount = g.clusterCount;
    Fat32ImageBuilder builder(options);
    const std::uint32_t cs = builder.clusterSize();

    std::vector<std::vector<std::byte>> files;
    for (const std::size_t size : {std::size_t{1}, std::size_t{cs - 1}, std::size_t{cs}, std::size_t{cs + 1},
                                   std::size_t{3 * cs + 17}}) {
        files.push_back(bytesOf(size, size));
        (void)builder.addFile(builder.root(), "File of " + std::to_string(size) + " bytes.bin", files.back());
    }
    const Mounted m = mount(builder.build(), g.bytesPerSector);
    ASSERT_NE(m.fs, nullptr);
    EXPECT_EQ(m.fs->info().clusterSize, cs);

    const FileScan scan = m.scan();
    ASSERT_EQ(scan.records.size(), files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        EXPECT_TRUE(scan.records[i].allocation.issues.empty());
        EXPECT_EQ(test::readExtents(*m.source, scan.records[i].allocation.extents), files[i]) << i;
    }
}

INSTANTIATE_TEST_SUITE_P(ClusterSizes, Fat32GeometryTest,
                         ::testing::Values(Geometry{512, 1, 512}, Geometry{512, 2, 512}, Geometry{512, 8, 256},
                                           Geometry{512, 64, 64}, Geometry{4096, 1, 256}, Geometry{4096, 8, 64}),
                         [](const auto& info) {
                             return std::to_string(info.param.bytesPerSector) + "x" +
                                    std::to_string(info.param.sectorsPerCluster);
                         });

TEST(Fat32FilesystemTest, SpecCompliantVolume) {
    test::Fat32BuilderOptions options;
    options.clusterCount = kMinStandardClusters + 10;
    Fat32ImageBuilder builder(options);
    const auto data = bytesOf(10000, 99);
    (void)builder.addFile(builder.root(), "DATA.BIN", data);
    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    EXPECT_TRUE(m.fs->info().warnings.empty());
    const FileScan scan = m.scan();
    ASSERT_EQ(scan.records.size(), 1u);
    EXPECT_EQ(test::readExtents(*m.source, scan.records[0].allocation.extents), data);
}

// ---------------------------------------------------------------------------
// Allocation analysis
// ---------------------------------------------------------------------------

TEST(Fat32FilesystemTest, ClusterUsageMatchesImage) {
    Fat32ImageBuilder builder;
    (void)builder.addFile(builder.root(), "A.BIN", bytesOf(5000, 1));
    (void)builder.addDirectory(builder.root(), "DIR");
    builder.setFat(500, 0x0FFFFFF7);  // bad cluster
    const std::uint32_t used = builder.usedClusters();
    const Mounted m = mount(builder.build());

    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->total, 2048u);
    EXPECT_EQ(usage->bad, 1u);
    EXPECT_EQ(usage->allocated, used - 1u);
    EXPECT_EQ(usage->free, 2048u - used);
    EXPECT_EQ(usage->invalid, 0u);
    EXPECT_EQ(usage->unreadable, 0u);
    EXPECT_EQ(usage->mirrorMismatches, 0u);
    ASSERT_TRUE(usage->recordedFree.has_value());
    EXPECT_EQ(*usage->recordedFree, usage->free);

    EXPECT_EQ(m.fs->clusterState(ClusterNumber{2}).value(), ClusterState::Allocated);
    EXPECT_EQ(m.fs->clusterState(ClusterNumber{500}).value(), ClusterState::Bad);
    EXPECT_EQ(m.fs->clusterState(ClusterNumber{1000}).value(), ClusterState::Free);
    RECOVERY_EXPECT_ERROR(m.fs->clusterState(ClusterNumber{1}), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(m.fs->clusterState(ClusterNumber{2050}), ErrorCode::InvalidInput);
}

// ---------------------------------------------------------------------------
// Opening, damaged media and limits
// ---------------------------------------------------------------------------

TEST(Fat32FilesystemTest, FallsBackToBackupBootSector) {
    Fat32ImageBuilder builder;
    (void)builder.addFile(builder.root(), "A.TXT", bytesOf(10, 1));
    std::vector<std::byte> image = builder.build();
    std::fill_n(image.begin(), 512, std::byte{0});
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_NE(m.fs->info().warnings.front().find("backup boot sector"), std::string::npos);
    EXPECT_EQ(m.scan().records.size(), 1u);
}

TEST(Fat32FilesystemTest, RejectsNonFat32Volumes) {
    test::MemoryStorageSource zeros(std::vector<std::byte>(64 * 1024), 512);
    RECOVERY_ASSERT_OK(zeros.open());
    RECOVERY_EXPECT_ERROR(Fat32Filesystem::open(zeros), ErrorCode::UnsupportedFilesystem);

    test::MemoryStorageSource tiny(std::vector<std::byte>(100), 512);
    RECOVERY_ASSERT_OK(tiny.open());
    RECOVERY_EXPECT_ERROR(Fat32Filesystem::open(tiny), ErrorCode::UnsupportedFilesystem);

    test::MemoryStorageSource closed(std::vector<std::byte>(4096), 512);
    RECOVERY_EXPECT_ERROR(Fat32Filesystem::open(closed), ErrorCode::InvalidInput);
}

TEST(Fat32FilesystemTest, PrimaryFatSectorUnreadableUsesSecondCopy) {
    Fat32ImageBuilder builder;
    const auto data = bytesOf(4000, 3);
    (void)builder.addFile(builder.root(), "A.BIN", data);
    std::vector<std::byte> image = builder.build();
    auto source = std::make_unique<test::MemoryStorageSource>(image, 512);
    source->addBadSector(builder.fatOffset(0) / 512);  // FAT 1, sector 0
    RECOVERY_ASSERT_OK(source->open());
    Result<std::unique_ptr<Fat32Filesystem>> fs = Fat32Filesystem::open(*source);
    RECOVERY_ASSERT_OK(fs);

    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    ASSERT_EQ(scan->records.size(), 1u);
    EXPECT_TRUE(scan->records[0].allocation.issues.empty());
    EXPECT_EQ(test::readExtents(*source, scan->records[0].allocation.extents), data);

    const Result<ClusterUsage> usage = fs.value()->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->unreadable, 0u);
}

TEST(Fat32FilesystemTest, FatSectorUnreadableInEveryCopy) {
    Fat32ImageBuilder builder;
    (void)builder.addFile(builder.root(), "A.BIN", bytesOf(4000, 3));
    std::vector<std::byte> image = builder.build();
    auto source = std::make_unique<test::MemoryStorageSource>(image, 512);
    source->addBadSector(builder.fatOffset(0) / 512);
    source->addBadSector(builder.fatOffset(1) / 512);
    RECOVERY_ASSERT_OK(source->open());
    Result<std::unique_ptr<Fat32Filesystem>> fs = Fat32Filesystem::open(*source);
    RECOVERY_ASSERT_OK(fs);

    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    // The root directory's own chain entry is unreadable, but its first
    // cluster is still listed.
    ASSERT_EQ(scan->records.size(), 1u);
    EXPECT_TRUE(scan->records[0].allocation.hasIssue(AllocationIssue::UnreadableAllocationTable));

    const Result<ClusterUsage> usage = fs.value()->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->unreadable, 128u - 2u);  // one FAT sector minus the two reserved entries
}

TEST(Fat32FilesystemTest, UnreadableDirectoryClusterIsReported) {
    Fat32ImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "BROKEN");
    (void)builder.addFile(folder.clusters[0], "LOST.TXT", bytesOf(10, 1));
    (void)builder.addFile(builder.root(), "FINE.TXT", bytesOf(10, 1));
    std::vector<std::byte> image = builder.build();
    auto source = std::make_unique<test::MemoryStorageSource>(image, 512);
    source->addBadSector(builder.clusterOffset(folder.clusters[0]) / 512);
    RECOVERY_ASSERT_OK(source->open());
    Result<std::unique_ptr<Fat32Filesystem>> fs = Fat32Filesystem::open(*source);
    RECOVERY_ASSERT_OK(fs);

    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    EXPECT_EQ(scan->records.size(), 2u);  // BROKEN and FINE.TXT
    ASSERT_EQ(scan->issues.size(), 1u);
    EXPECT_EQ(scan->issues[0].kind, ScanIssueKind::DirectoryUnreadable);
    EXPECT_EQ(scan->issues[0].path, "/BROKEN");
}

TEST(Fat32FilesystemTest, DepthAndEntryLimits) {
    Fat32ImageBuilder builder;
    std::uint32_t parent = builder.root();
    for (int depth = 0; depth < 5; ++depth) {
        parent = builder.addDirectory(parent, "LEVEL" + std::to_string(depth)).clusters[0];
    }
    const Mounted m = mount(builder.build());

    ScanLimits shallow;
    shallow.maxDepth = 2;
    const FileScan limited = m.scan(shallow);
    EXPECT_FALSE(limited.complete);
    EXPECT_EQ(limited.records.size(), 3u);
    EXPECT_EQ(limited.issues.at(0).kind, ScanIssueKind::DepthLimit);

    ScanLimits few;
    few.maxEntries = 2;
    const FileScan cut = m.scan(few);
    EXPECT_FALSE(cut.complete);
    EXPECT_EQ(cut.records.size(), 2u);
    EXPECT_EQ(cut.issues.at(0).kind, ScanIssueKind::EntryLimit);
}

TEST(Fat32FilesystemTest, ScanCanBeCancelled) {
    Fat32ImageBuilder builder;
    (void)builder.addFile(builder.root(), "A.TXT", bytesOf(10, 1));
    const Mounted m = mount(builder.build());
    CancellationSource cancel;
    cancel.requestCancellation();
    RECOVERY_EXPECT_ERROR(m.fs->scan({}, cancel.token()), ErrorCode::Cancelled);
    RECOVERY_EXPECT_ERROR(m.fs->analyzeClusters(cancel.token()), ErrorCode::Cancelled);
}

TEST(Fat32FilesystemTest, ReadDirectoryDirectly) {
    Fat32ImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "Sub folder");
    (void)builder.addFile(folder.clusters[0], "X.TXT", bytesOf(10, 1));
    const Mounted m = mount(builder.build());

    const Result<DirectoryListing> root = m.fs->readDirectory(m.fs->rootDirectory());
    RECOVERY_ASSERT_OK(root);
    ASSERT_EQ(root->entries.size(), 1u);
    EXPECT_EQ(root->entries[0].name, "Sub folder");

    const Result<DirectoryListing> sub = m.fs->readDirectory(root->entries[0]);
    RECOVERY_ASSERT_OK(sub);
    ASSERT_EQ(sub->entries.size(), 1u);
    EXPECT_EQ(sub->entries[0].name, "X.TXT");

    RECOVERY_EXPECT_ERROR(m.fs->readDirectory(sub->entries[0]), ErrorCode::InvalidInput);
    const Result<FileAllocation> allocation = m.fs->resolveAllocation(sub->entries[0]);
    RECOVERY_ASSERT_OK(allocation);
    EXPECT_EQ(allocation->method, AllocationMethod::ClusterChain);
}

// ---------------------------------------------------------------------------
// FAT32 inside a partition table (P3 + P4)
// ---------------------------------------------------------------------------

TEST(Fat32FilesystemTest, OpensInsideMbrPartition) {
    Fat32ImageBuilder builder;
    const auto data = bytesOf(2500, 42);
    (void)builder.addFile(builder.root(), "IN PARTITION.JPG", data);
    const std::vector<std::byte> volume = builder.build();

    constexpr std::uint32_t kStart = 2048;
    std::vector<std::byte> disk((kStart + volume.size() / 512 + 100) * 512);
    std::copy(volume.begin(), volume.end(), disk.begin() + kStart * 512);
    test::writeMbrSector(disk, 512, 0, {{0x80, 0x0C, kStart, static_cast<std::uint32_t>(volume.size() / 512)}});

    test::MemoryStorageSource device(disk, 512);
    RECOVERY_ASSERT_OK(device.open());
    const Result<partition::PartitionTable> table = partition::readPartitionTable(device);
    RECOVERY_ASSERT_OK(table);
    ASSERT_EQ(table->partitions.size(), 1u);
    ASSERT_TRUE(table->partitions[0].candidates.fat);

    partition::PartitionSource part(device, table->partitions[0]);
    RECOVERY_ASSERT_OK(part.open());
    Result<std::unique_ptr<Fat32Filesystem>> fs = Fat32Filesystem::open(part);
    RECOVERY_ASSERT_OK(fs);
    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    ASSERT_EQ(scan->records.size(), 1u);
    EXPECT_EQ(scan->records[0].entry.name, "IN PARTITION.JPG");
    EXPECT_EQ(test::readExtents(part, scan->records[0].allocation.extents), data);
}

TEST(Fat32FilesystemTest, SuperfloppyVolume) {
    Fat32ImageBuilder builder;
    (void)builder.addFile(builder.root(), "A.TXT", bytesOf(10, 1));
    test::MemoryStorageSource device(builder.build(), 512);
    RECOVERY_ASSERT_OK(device.open());
    const Result<partition::PartitionTable> table = partition::readPartitionTable(device);
    RECOVERY_ASSERT_OK(table);
    EXPECT_EQ(table->scheme, partition::PartitionScheme::Unpartitioned);
    ASSERT_EQ(table->partitions.size(), 1u);
    EXPECT_TRUE(table->partitions[0].candidates.fat);
}

TEST(Fat32FilesystemTest, LogsOpenAndScan) {
    Fat32ImageBuilder builder;
    auto sink = std::make_shared<diagnostics::MemorySink>();
    diagnostics::Logger logger(diagnostics::LogLevel::Debug);
    logger.addSink(sink);
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    Result<std::unique_ptr<Fat32Filesystem>> fs = Fat32Filesystem::open(source, {}, &logger);
    RECOVERY_ASSERT_OK(fs);
    RECOVERY_ASSERT_OK(fs.value()->scan({}, {}));
    const auto records = sink->records();
    EXPECT_TRUE(std::any_of(records.begin(), records.end(),
                            [](const auto& r) { return r.message == "FAT32 volume opened"; }));
    EXPECT_TRUE(std::any_of(records.begin(), records.end(),
                            [](const auto& r) { return r.message == "FAT32 scan finished"; }));
}

}  // namespace
}  // namespace recovery::filesystem::fat32
