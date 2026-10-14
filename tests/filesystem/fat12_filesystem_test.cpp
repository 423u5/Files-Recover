// FAT12 (after P20, the user's decision of 2026-10-14 on L225): the FAT32
// module reads FAT12 volumes (floppy disks, volumes of a few megabytes) as it
// reads FAT16, with 12-bit FAT entries: two in three bytes, so every third
// FAT sector boundary falls inside an entry (entries 341 and 682 of every
// 1024 with 512-byte sectors).

#include "filesystem/fat32/fat32_boot_sector.hpp"
#include "filesystem/fat32/fat32_filesystem.hpp"

#include "recovery/byte_order.hpp"
#include "support/fat32_builder.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace recovery::filesystem::fat32 {
namespace {

using test::Fat32ImageBuilder;

// A 1.44 MB floppy disk as Windows formats it: 2880 sectors, 2 FATs of 9
// sectors, 224 root entries, 2847 clusters of one sector.
test::Fat32BuilderOptions floppy() {
    test::Fat32BuilderOptions options;
    options.fat12 = true;
    options.clusterCount = 2847;
    options.reservedSectors = 1;
    options.rootEntries = 224;
    options.media = 0xF0;
    options.label = "FLOPPY";
    options.volumeId = 0x12121212;
    return options;
}

struct Mounted {
    std::unique_ptr<test::MemoryStorageSource> source;
    std::unique_ptr<Fat32Filesystem> fs;

    [[nodiscard]] FileScan scan() const {
        Result<FileScan> result = fs->scan({}, {});
        EXPECT_TRUE(result.ok()) << (result.ok() ? "" : describe(result.error()));
        return result.ok() ? std::move(result).value() : FileScan{};
    }
};

// `badSectors`: sectors of the image that fail to read.
Mounted mount(std::vector<std::byte> image, const std::vector<std::uint64_t>& badSectors = {}) {
    Mounted m;
    m.source = std::make_unique<test::MemoryStorageSource>(std::move(image), 512);
    for (const std::uint64_t sector : badSectors) {
        m.source->addBadSector(sector, test::MemoryStorageSource::kErrorCrc);
    }
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

std::vector<std::byte> dataOf(const Mounted& m, const FileRecord& record) {
    return test::readExtents(*m.source, record.allocation.extents);
}

TEST(Fat12BootSectorTest, ParsesAFloppyDisk) {
    Fat32ImageBuilder builder(floppy());
    const std::vector<std::byte> image = builder.build();
    EXPECT_EQ(image.size(), 1'474'560U);
    const Result<BootSector> boot = parseBootSector(std::span(image).first(512), image.size());
    RECOVERY_ASSERT_OK(boot);
    EXPECT_EQ(boot->type, FatType::Fat12);
    EXPECT_EQ(boot->fatEntryBits(), 12U);
    EXPECT_TRUE(boot->hasFixedRoot());
    EXPECT_EQ(boot->clusterCount, 2847U);
    EXPECT_EQ(boot->fatSectors, 9U);
    EXPECT_EQ(boot->rootEntries, 224U);
    EXPECT_EQ(boot->rootDirOffset, 19U * 512);
    EXPECT_EQ(boot->dataOffset, 33U * 512);
    EXPECT_EQ(boot->dataOffset, builder.clusterOffset(2));
    EXPECT_EQ(boot->media, 0xF0);
    EXPECT_EQ(boot->volumeLabel, "FLOPPY");
    EXPECT_EQ(boot->volumeId, 0x12121212U);
    EXPECT_TRUE(boot->warnings.empty());
}

// As the specification says: fewer than 4085 clusters is FAT12, whatever the
// label at offset 54 says.
TEST(Fat12BootSectorTest, TheClusterCountDecidesBetweenFat12AndFat16) {
    test::Fat32BuilderOptions options;
    options.fat16 = true;
    options.clusterCount = 4200;
    options.reservedSectors = 1;
    Fat32ImageBuilder builder(options);
    std::vector<std::byte> sector = builder.build();
    sector.resize(512);
    const std::uint16_t total = loadLe16(sector, 19);
    for (const auto& [clusters, type] : {std::pair{4084U, FatType::Fat12}, std::pair{4085U, FatType::Fat16},
                                         std::pair{1U, FatType::Fat12}}) {
        storeLe16(sector, 19, static_cast<std::uint16_t>(total - (4200 - clusters)));
        const Result<BootSector> boot = parseBootSector(sector, 64 * kMiB);
        RECOVERY_ASSERT_OK(boot);
        EXPECT_EQ(boot->type, type) << clusters;
        EXPECT_EQ(boot->clusterCount, clusters);
    }
    // No data cluster at all.
    storeLe16(sector, 19, static_cast<std::uint16_t>(total - 4200));
    const Result<BootSector> empty = parseBootSector(sector, 64 * kMiB);
    RECOVERY_EXPECT_ERROR(empty, ErrorCode::CorruptedFilesystem);
    EXPECT_NE(empty.error().message.find("FAT12/16 boot sector"), std::string::npos) << empty.error().message;
}

TEST(Fat12BootSectorTest, AFatTooSmallForItsClustersCoversWhatItCan) {
    Fat32ImageBuilder builder(floppy());
    std::vector<std::byte> sector = builder.build();
    sector.resize(512);
    // One FAT sector: 341 entries, two of them reserved; 2863 clusters follow the smaller FATs.
    storeLe16(sector, 22, 1);
    const Result<BootSector> boot = parseBootSector(sector, 2 * kMiB);
    RECOVERY_ASSERT_OK(boot);
    EXPECT_EQ(boot->type, FatType::Fat12);
    EXPECT_EQ(boot->clusterCount, 339U);
    ASSERT_EQ(boot->warnings.size(), 1U);
    EXPECT_NE(boot->warnings.front().find("FAT covers only 339 of 2863 clusters"), std::string::npos)
        << boot->warnings.front();
}

TEST(Fat12FilesystemTest, FindsFilesInTheFixedRootAndInFolders) {
    Fat32ImageBuilder builder(floppy());
    const std::vector<std::byte> a = test::makePattern(3000, 1);
    const std::vector<std::byte> b = test::makePattern(700, 2);
    const std::vector<std::byte> c = test::makePattern(9000, 3);
    const std::vector<std::byte> d = test::makePattern(100, 4);
    (void)builder.addFile(builder.root(), "README.TXT", a);
    (void)builder.addFile(builder.root(), "A long name.doc", b);
    const auto docs = builder.addDirectory(builder.root(), "DOCS");
    const auto old = builder.addDirectory(docs.clusters.front(), "1999");
    (void)builder.addFile(old.clusters.front(), "LETTER.WRI", c);
    (void)builder.addFile(docs.clusters.front(), "Grüße.txt", d);

    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    EXPECT_EQ(m.fs->info().type, FilesystemType::Fat12);
    EXPECT_EQ(toString(m.fs->info().type), "FAT12");
    EXPECT_EQ(m.fs->info().label, "FLOPPY");
    EXPECT_EQ(m.fs->info().clusterCount, 2847U);

    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.complete);
    EXPECT_TRUE(scan.issues.empty());
    for (const auto& [path, data] : {std::pair{"/README.TXT", &a}, std::pair{"/A long name.doc", &b},
                                     std::pair{"/DOCS/1999/LETTER.WRI", &c}, std::pair{"/DOCS/Grüße.txt", &d}}) {
        const FileRecord* record = find(scan, path);
        ASSERT_NE(record, nullptr) << path;
        EXPECT_EQ(record->entry.state, EntryState::Active);
        EXPECT_EQ(dataOf(m, *record), *data) << path;
        EXPECT_TRUE(record->allocation.issues.empty()) << path;
    }
    ASSERT_NE(find(scan, "/DOCS/1999"), nullptr);
    EXPECT_TRUE(find(scan, "/DOCS/1999")->entry.isDirectory);
}

TEST(Fat12FilesystemTest, DeletedFilesAndFoldersAreFound) {
    Fat32ImageBuilder builder(floppy());
    const std::vector<std::byte> photo = test::makePattern(5000, 5);
    const std::vector<std::byte> inside = test::makePattern(1500, 6);
    const auto deletedRoot = builder.addFile(builder.root(), "Scan.jpg", photo);
    const auto folder = builder.addDirectory(builder.root(), "Old letters");  // a long name keeps its first letter
    (void)builder.addFile(folder.clusters.front(), "NOTE.TXT", inside);
    builder.deleteEntry(deletedRoot);
    builder.deleteEntry(folder);  // its cluster freed; NOTE.TXT inside it, chain still recorded

    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    const FileScan scan = m.scan();
    const FileRecord* scanned = find(scan, "/Scan.jpg");
    ASSERT_NE(scanned, nullptr);
    EXPECT_EQ(scanned->entry.state, EntryState::Deleted);
    EXPECT_EQ(scanned->allocation.method, AllocationMethod::ContiguousGuess);
    EXPECT_EQ(dataOf(m, *scanned), photo);
    const FileRecord* note = find(scan, "/Old letters/NOTE.TXT");
    ASSERT_NE(note, nullptr);
    EXPECT_EQ(note->entry.state, EntryState::Deleted);
    EXPECT_TRUE(note->parentDeleted);
    EXPECT_EQ(dataOf(m, *note), inside);
}

// Entries 341, 682, 1365 and 2730 lie across FAT sector boundaries; even and
// odd entries are packed differently.
TEST(Fat12FilesystemTest, AChainThroughEntriesAcrossFatSectorsIsFollowed) {
    Fat32ImageBuilder builder(floppy());
    const std::vector<std::byte> data = test::makePattern(6 * 512, 7);
    const std::vector<std::uint32_t> clusters{340, 341, 682, 1365, 2730, 2848};
    (void)builder.addFileInClusters(builder.root(), "CROSS.BIN", data, clusters);
    builder.setFat(2848, 0x0FFFFFF8);  // 0xFF8 ends a chain too
    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    const FileScan scan = m.scan();
    const FileRecord* cross = find(scan, "/CROSS.BIN");
    ASSERT_NE(cross, nullptr);
    EXPECT_EQ(cross->allocation.method, AllocationMethod::ClusterChain);
    EXPECT_EQ(cross->allocation.extents.size(), 5U);  // 340-341 | 682 | 1365 | 2730 | 2848
    EXPECT_EQ(dataOf(m, *cross), data);
    EXPECT_TRUE(cross->allocation.issues.empty());
    for (const std::uint32_t cluster : clusters) {
        EXPECT_EQ(*m.fs->clusterState(ClusterNumber{cluster}), ClusterState::Allocated) << cluster;
    }
    EXPECT_EQ(*m.fs->clusterState(ClusterNumber{342}), ClusterState::Free);
}

TEST(Fat12FilesystemTest, ClusterAnalysisReadsEveryEntry) {
    Fat32ImageBuilder builder(floppy());
    (void)builder.addFile(builder.root(), "A.BIN", test::makePattern(5 * 512, 8));  // clusters 3-7
    builder.setFat(100, 0x0FFFFFF7);  // bad
    builder.setFat(341, 0x0FFFFFF7);  // bad, across FAT sectors 0 and 1
    builder.setFatInCopy(1, 342, 0x0FFFFFFF);  // the second copy differs
    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->total, 2847U);
    EXPECT_EQ(usage->allocated, 5U);
    EXPECT_EQ(usage->bad, 2U);
    EXPECT_EQ(usage->free, 2847U - 5U - 2U);
    EXPECT_EQ(usage->invalid, 0U);
    EXPECT_EQ(usage->unreadable, 0U);
    EXPECT_EQ(usage->mirrorMismatches, 1U);
    EXPECT_FALSE(usage->recordedFree.has_value());
    EXPECT_EQ(*m.fs->clusterState(ClusterNumber{341}), ClusterState::Bad);
    EXPECT_EQ(*m.fs->clusterState(ClusterNumber{342}), ClusterState::Free);
}

TEST(Fat12FilesystemTest, AnUnreadableFatSectorIsReadFromTheOtherCopy) {
    Fat32ImageBuilder builder(floppy());
    const std::vector<std::byte> data = test::makePattern(4 * 512, 9);
    (void)builder.addFileInClusters(builder.root(), "CROSS.BIN", data, {340, 341, 682, 683});
    const std::uint64_t fatSector1 = builder.fatOffset(0) / 512 + 1;  // entries 341 (in part) to 682 (in part)
    const Mounted m = mount(builder.build(), {fatSector1});
    ASSERT_NE(m.fs, nullptr);
    const FileScan scan = m.scan();
    const FileRecord* cross = find(scan, "/CROSS.BIN");
    ASSERT_NE(cross, nullptr);
    EXPECT_TRUE(cross->allocation.issues.empty());
    EXPECT_EQ(dataOf(m, *cross), data);
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->unreadable, 0U);
    EXPECT_EQ(usage->allocated, 4U);
}

TEST(Fat12FilesystemTest, AFatSectorUnreadableInEveryCopyLosesTheEntriesItHolds) {
    Fat32ImageBuilder builder(floppy());
    const std::vector<std::byte> data = test::makePattern(4 * 512, 10);
    (void)builder.addFileInClusters(builder.root(), "CROSS.BIN", data, {340, 341, 682, 683});
    const Mounted m =
        mount(builder.build(), {builder.fatOffset(0) / 512 + 1, builder.fatOffset(1) / 512 + 1});
    ASSERT_NE(m.fs, nullptr);
    const FileScan scan = m.scan();
    const FileRecord* cross = find(scan, "/CROSS.BIN");
    ASSERT_NE(cross, nullptr);
    EXPECT_TRUE(cross->allocation.hasIssue(AllocationIssue::UnreadableAllocationTable));
    // Every entry with a byte in that sector: 341 (bytes 511-512) to 682 (bytes 1023-1024).
    EXPECT_EQ(*m.fs->clusterState(ClusterNumber{340}), ClusterState::Allocated);
    EXPECT_EQ(*m.fs->clusterState(ClusterNumber{341}), ClusterState::Unreadable);
    EXPECT_EQ(*m.fs->clusterState(ClusterNumber{682}), ClusterState::Unreadable);
    EXPECT_EQ(*m.fs->clusterState(ClusterNumber{683}), ClusterState::Allocated);
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->unreadable, 682U - 341U + 1U);
}

}  // namespace
}  // namespace recovery::filesystem::fat32
