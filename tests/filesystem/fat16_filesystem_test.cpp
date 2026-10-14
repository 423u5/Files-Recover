// FAT16 (after P20: the user's production-readiness list of 2026-10-12): the
// FAT32 module reads FAT16 volumes as Windows formats them (cards of 2 GB and
// less): 16-bit FAT entries, a root directory of fixed size before cluster 2,
// no FSInfo and no backup boot sector. FAT12 is refused.

#include "filesystem/fat32/fat32_boot_sector.hpp"
#include "filesystem/fat32/fat32_filesystem.hpp"

#include "recovery/byte_order.hpp"
#include "support/fat32_builder.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace recovery::filesystem::fat32 {
namespace {

using test::Fat32ImageBuilder;

test::Fat32BuilderOptions fat16(std::uint32_t clusters = 4200) {
    test::Fat32BuilderOptions options;
    options.fat16 = true;
    options.clusterCount = clusters;
    options.reservedSectors = 1;
    options.label = "CARD16";
    options.volumeId = 0x5A5A1616;
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

Mounted mount(std::vector<std::byte> image) {
    Mounted m;
    m.source = std::make_unique<test::MemoryStorageSource>(std::move(image), 512);
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

TEST(Fat16BootSectorTest, ParsesTheLayoutWindowsWrites) {
    Fat32ImageBuilder builder(fat16());
    const std::vector<std::byte> image = builder.build();
    const Result<BootSector> boot = parseBootSector(std::span(image).first(512), image.size());
    RECOVERY_ASSERT_OK(boot);
    EXPECT_EQ(boot->type, FatType::Fat16);
    EXPECT_EQ(boot->fatEntryBytes(), 2U);
    EXPECT_EQ(boot->clusterCount, 4200U);
    EXPECT_EQ(boot->rootEntries, 512U);
    EXPECT_EQ(boot->rootDirOffset, builder.fatOffset(2));
    EXPECT_EQ(boot->rootDirBytes, 512U * 32);
    EXPECT_EQ(boot->dataOffset, builder.clusterOffset(2));
    EXPECT_EQ(boot->rootCluster, 0U);
    EXPECT_EQ(boot->volumeLabel, "CARD16");
    EXPECT_EQ(boot->volumeId, 0x5A5A1616U);
    EXPECT_TRUE(boot->warnings.empty());
}

TEST(Fat16BootSectorTest, Fat12IsRefusedAndTooManyClustersAreCorrupt) {
    Fat32ImageBuilder builder(fat16());
    std::vector<std::byte> sector = builder.build();
    sector.resize(512);
    const std::uint16_t total = loadLe16(sector, 19);
    // 4084 clusters: FAT12 by the specification's count.
    storeLe16(sector, 19, static_cast<std::uint16_t>(total - (4200 - 4084)));
    const Result<BootSector> fat12 = parseBootSector(sector, 64 * kMiB);
    RECOVERY_EXPECT_ERROR(fat12, ErrorCode::UnsupportedFilesystem);
    EXPECT_NE(fat12.error().message.find("FAT12"), std::string::npos) << fat12.error().message;
    // 65525 clusters cannot have 16-bit entries.
    storeLe16(sector, 19, 0);
    storeLe32(sector, 32, total - 4200 + 65525);
    RECOVERY_EXPECT_ERROR(parseBootSector(sector, 64 * kMiB), ErrorCode::CorruptedFilesystem);
    // No root directory entries.
    Fat32ImageBuilder other(fat16());
    std::vector<std::byte> noRoot = other.build();
    noRoot.resize(512);
    storeLe16(noRoot, 17, 0);
    storeLe16(noRoot, 22, 0);  // and no 16-bit FAT size: read as FAT32, whose 32-bit sector count is zero
    RECOVERY_EXPECT_ERROR(parseBootSector(noRoot, 64 * kMiB), ErrorCode::CorruptedFilesystem);
}

TEST(Fat16FilesystemTest, FindsFilesInTheFixedRootAndInFolders) {
    Fat32ImageBuilder builder(fat16());
    const std::vector<std::byte> a = test::makePattern(3000, 1);
    const std::vector<std::byte> b = test::makePattern(700, 2);
    const std::vector<std::byte> c = test::makePattern(9000, 3);
    const std::vector<std::byte> d = test::makePattern(100, 4);
    (void)builder.addFile(builder.root(), "A.TXT", a);
    (void)builder.addFile(builder.root(), "A long name.jpg", b);
    const auto dcim = builder.addDirectory(builder.root(), "DCIM");
    const auto camera = builder.addDirectory(dcim.clusters.front(), "100CANON");
    (void)builder.addFile(camera.clusters.front(), "IMG_0001.JPG", c);
    (void)builder.addFile(dcim.clusters.front(), "Grüße.txt", d);

    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    EXPECT_EQ(m.fs->info().type, FilesystemType::Fat16);
    EXPECT_EQ(toString(m.fs->info().type), "FAT16");
    EXPECT_EQ(m.fs->info().label, "CARD16");
    EXPECT_EQ(m.fs->info().clusterCount, 4200U);

    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.complete);
    EXPECT_TRUE(scan.issues.empty());
    for (const auto& [path, data] : {std::pair{"/A.TXT", &a}, std::pair{"/A long name.jpg", &b},
                                     std::pair{"/DCIM/100CANON/IMG_0001.JPG", &c},
                                     std::pair{"/DCIM/Grüße.txt", &d}}) {
        const FileRecord* record = find(scan, path);
        ASSERT_NE(record, nullptr) << path;
        EXPECT_EQ(record->entry.state, EntryState::Active);
        EXPECT_EQ(dataOf(m, *record), *data) << path;
        EXPECT_TRUE(record->allocation.issues.empty()) << path;
    }
    ASSERT_NE(find(scan, "/DCIM/100CANON"), nullptr);
    EXPECT_TRUE(find(scan, "/DCIM/100CANON")->entry.isDirectory);
}

TEST(Fat16FilesystemTest, DeletedFilesAndFoldersAreFound) {
    Fat32ImageBuilder builder(fat16());
    const std::vector<std::byte> photo = test::makePattern(5000, 5);
    const std::vector<std::byte> inside = test::makePattern(1500, 6);
    const auto deletedRoot = builder.addFile(builder.root(), "Holiday.jpg", photo);
    const auto folder = builder.addDirectory(builder.root(), "Old photos");  // a long name keeps its first letter
    (void)builder.addFile(folder.clusters.front(), "NOTE.TXT", inside);
    builder.deleteEntry(deletedRoot);
    builder.deleteEntry(folder);  // its cluster freed; NOTE.TXT inside it, chain still recorded

    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    const FileScan scan = m.scan();
    const FileRecord* holiday = find(scan, "/Holiday.jpg");
    ASSERT_NE(holiday, nullptr);
    EXPECT_EQ(holiday->entry.state, EntryState::Deleted);
    EXPECT_EQ(holiday->allocation.method, AllocationMethod::ContiguousGuess);
    EXPECT_EQ(dataOf(m, *holiday), photo);
    const FileRecord* note = find(scan, "/Old photos/NOTE.TXT");
    ASSERT_NE(note, nullptr);
    EXPECT_EQ(note->entry.state, EntryState::Deleted);
    EXPECT_TRUE(note->parentDeleted);
    EXPECT_EQ(dataOf(m, *note), inside);
}

TEST(Fat16FilesystemTest, AFragmentedFileFollowsItsChain) {
    Fat32ImageBuilder builder(fat16());
    const std::vector<std::byte> video = test::makePattern(4 * 512, 7);
    (void)builder.addFileInClusters(builder.root(), "CLIP.MP4", video, {40, 41, 12, 300});
    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    const FileScan scan = m.scan();
    const FileRecord* clip = find(scan, "/CLIP.MP4");
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->allocation.method, AllocationMethod::ClusterChain);
    EXPECT_EQ(clip->allocation.extents.size(), 3U);  // 40-41 | 12 | 300
    EXPECT_EQ(dataOf(m, *clip), video);
    EXPECT_TRUE(clip->allocation.issues.empty());
}

TEST(Fat16FilesystemTest, AFullRootDirectoryIsReadToItsLastSlot) {
    Fat32ImageBuilder builder(fat16());
    // The label takes one of the 512 slots.
    for (int i = 0; i < 511; ++i) {
        const std::string name = "F" + std::to_string(i) + ".BIN";
        (void)builder.addFile(builder.root(), name, test::makePattern(10, static_cast<std::uint64_t>(i)));
    }
    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    const FileScan scan = m.scan();
    EXPECT_EQ(scan.records.size(), 511U);
    ASSERT_NE(find(scan, "/F510.BIN"), nullptr);
    EXPECT_EQ(dataOf(m, *find(scan, "/F510.BIN")), test::makePattern(10, 510));
}

TEST(Fat16FilesystemTest, AnUnreadableRootSectorLosesOnlyItsEntries) {
    Fat32ImageBuilder builder(fat16());
    for (int i = 0; i < 40; ++i) {
        (void)builder.addFile(builder.root(), "G" + std::to_string(i) + ".BIN", test::makePattern(10, 100 + i));
    }
    const std::uint64_t rootSector = builder.fatOffset(2) / 512;
    Mounted m;
    m.source = std::make_unique<test::MemoryStorageSource>(builder.build(), 512);
    m.source->addBadSector(rootSector + 1, test::MemoryStorageSource::kErrorCrc);  // slots 16-31
    RECOVERY_ASSERT_OK(m.source->open());
    Result<std::unique_ptr<Fat32Filesystem>> fs = Fat32Filesystem::open(*m.source);
    RECOVERY_ASSERT_OK(fs);
    m.fs = std::move(fs).value();
    const FileScan scan = m.scan();
    // Slot 0 holds the label: files 0-14 in sector 0, 15-30 lost with sector 1, 31-39 in sector 2.
    EXPECT_EQ(scan.records.size(), 40U - 16U);
    EXPECT_NE(find(scan, "/G0.BIN"), nullptr);
    EXPECT_EQ(find(scan, "/G20.BIN"), nullptr);
    EXPECT_NE(find(scan, "/G39.BIN"), nullptr);
    ASSERT_FALSE(scan.issues.empty());
    EXPECT_EQ(scan.issues.front().kind, ScanIssueKind::DirectoryUnreadable);
    EXPECT_NE(scan.issues.front().detail.find("root directory sector 1"), std::string::npos)
        << scan.issues.front().detail;
}

}  // namespace
}  // namespace recovery::filesystem::fat32
