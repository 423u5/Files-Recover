// exFAT filesystem: normal, contiguous, fragmented, deleted and Unicode-named
// files on generated images, read through the filesystem-independent interface.

#include "filesystem/exfat/exfat_filesystem.hpp"

#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/byte_order.hpp"
#include "support/exfat_builder.hpp"
#include "support/fat32_builder.hpp"  // readExtents, utf8ToUtf16
#include "support/memory_source.hpp"
#include "support/partition_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>

namespace recovery::filesystem::exfat {
namespace {

using test::ExFatImageBuilder;

std::vector<std::byte> bytesOf(std::size_t size, std::uint64_t seed) {
    return test::makePattern(size, seed);
}

// Opens an image and keeps the source alive alongside the filesystem.
struct Mounted {
    std::unique_ptr<test::MemoryStorageSource> source;
    std::unique_ptr<ExFatFilesystem> fs;

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
    Result<std::unique_ptr<ExFatFilesystem>> fs = ExFatFilesystem::open(*m.source);
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

std::string issuesOf(const FileRecord& record) {
    std::string text;
    for (const EntryIssue issue : record.entry.issues) {
        text += std::string(toString(issue)) + " ";
    }
    for (const AllocationIssue issue : record.allocation.issues) {
        text += std::string(toString(issue)) + " ";
    }
    return text;
}

bool hasWarning(const FilesystemInfo& info, std::string_view text) {
    return std::any_of(info.warnings.begin(), info.warnings.end(),
                       [&](const std::string& w) { return w.find(text) != std::string::npos; });
}

TEST(ExFatFilesystemTest, OpensAndReportsInfo) {
    ExFatImageBuilder builder;
    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    const FilesystemInfo& info = m.fs->info();
    EXPECT_EQ(info.type, FilesystemType::ExFat);
    EXPECT_EQ(info.label, "TESTVOL");
    EXPECT_EQ(info.serialNumber, 0x1234ABCDu);
    EXPECT_EQ(info.bytesPerSector, 512u);
    EXPECT_EQ(info.clusterSize, 512u);
    EXPECT_EQ(info.clusterCount, 4096u);
    EXPECT_EQ(info.firstCluster, 2u);
    EXPECT_EQ(info.dataOffset, builder.clusterOffset(2));
    EXPECT_TRUE(info.warnings.empty()) << info.warnings.front();
    EXPECT_TRUE(m.fs->hasAllocationBitmap());
    EXPECT_TRUE(m.fs->usesVolumeUpcaseTable());

    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.records.empty());
    EXPECT_TRUE(scan.issues.empty());
}

TEST(ExFatFilesystemTest, UnicodeLabelAndNoLabel) {
    test::ExFatBuilderOptions options;
    options.label = "Caf\xC3\xA9 \xF0\x9F\x93\xB7";  // "Café 📷": 7 UTF-16 units
    ExFatImageBuilder labelled(options);
    EXPECT_EQ(mount(labelled.build()).fs->info().label, options.label);

    options.label.clear();
    ExFatImageBuilder unlabelled(options);
    EXPECT_EQ(mount(unlabelled.build()).fs->info().label, "");
}

TEST(ExFatFilesystemTest, FindsNormalFilesAndDirectories) {
    ExFatImageBuilder builder;
    const auto small = bytesOf(100, 1);
    const auto multi = bytesOf(5000, 2);
    const auto nested = bytesOf(1234, 3);
    (void)builder.addFile(builder.root(), "small.txt", small);
    (void)builder.addFile(builder.root(), "multi.bin", multi);
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
        {"/small.txt", &small}, {"/multi.bin", &multi}, {"/DCIM/100MEDIA/IMG_0001.JPG", &nested}};
    for (const auto& [path, data] : expected) {
        const FileRecord* record = find(scan, path);
        ASSERT_NE(record, nullptr) << path;
        EXPECT_EQ(record->entry.state, EntryState::Active);
        EXPECT_FALSE(record->entry.isDirectory);
        EXPECT_TRUE(record->entry.issues.empty()) << path << ": " << issuesOf(*record);
        EXPECT_EQ(record->entry.size, data->size());
        EXPECT_EQ(record->entry.validDataLength, data->size());
        EXPECT_TRUE(record->entry.shortName.empty());
        EXPECT_EQ(record->allocation.method, AllocationMethod::Contiguous);
        EXPECT_TRUE(record->allocation.issues.empty()) << path << ": " << issuesOf(*record);
        EXPECT_EQ(test::readExtents(*m.source, record->allocation.extents), *data) << path;
    }

    const FileRecord* directory = find(scan, "/DCIM/100MEDIA");
    ASSERT_NE(directory, nullptr);
    EXPECT_TRUE(directory->entry.isDirectory);
    EXPECT_EQ(directory->entry.name, "100MEDIA");
    EXPECT_EQ(directory->entry.size, 512u);
    EXPECT_TRUE(directory->allocation.issues.empty());
    // Everything the bitmap marks as allocated is referenced by something.
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(scan.referencedClusters, usage->allocated);
    EXPECT_EQ(usage->allocated, builder.allocatedClusters());
}

TEST(ExFatFilesystemTest, ContiguousFileNeedsNoFatChain) {
    ExFatImageBuilder builder;
    const auto data = bytesOf(10 * 512 + 3, 5);
    const auto file = builder.addFile(builder.root(), "movie.mp4", data);
    const Mounted m = mount(builder.build());
    // The FAT is not used for NoFatChain files, so the builder leaves it empty.
    for (const std::uint32_t c : file.clusters) {
        EXPECT_EQ(builder.fat(c), 0u);
    }
    const FileScan scan = m.scan();
    const FileRecord& record = scan.records.at(0);
    EXPECT_TRUE(record.entry.contiguousData);
    EXPECT_EQ(record.entry.firstCluster, ClusterNumber{file.clusters.front()});
    EXPECT_EQ(record.allocation.method, AllocationMethod::Contiguous);
    ASSERT_EQ(record.allocation.fragmentCount(), 1u);
    EXPECT_EQ(record.allocation.extents[0], (Extent{builder.clusterOffset(file.clusters.front()), data.size()}));
    EXPECT_EQ(record.allocation.clusterCount, 11u);
    EXPECT_EQ(test::readExtents(*m.source, record.allocation.extents), data);
}

TEST(ExFatFilesystemTest, FragmentedFileFollowsTheChain) {
    ExFatImageBuilder builder;
    const auto data = bytesOf(6 * 512 - 100, 7);
    (void)builder.addFileInClusters(builder.root(), "frag.bin", data, {400, 401, 402, 900, 901, 600});
    const Mounted m = mount(builder.build());

    const FileScan scan = m.scan();
    const FileRecord& record = scan.records.at(0);
    EXPECT_FALSE(record.entry.contiguousData);
    EXPECT_EQ(record.allocation.method, AllocationMethod::ClusterChain);
    EXPECT_TRUE(record.allocation.issues.empty()) << issuesOf(record);
    ASSERT_EQ(record.allocation.fragmentCount(), 3u);
    EXPECT_EQ(record.allocation.extents[0], (Extent{builder.clusterOffset(400), 3 * 512}));
    EXPECT_EQ(record.allocation.extents[1], (Extent{builder.clusterOffset(900), 2 * 512}));
    EXPECT_EQ(record.allocation.extents[2], (Extent{builder.clusterOffset(600), 512 - 100}));
    EXPECT_EQ(record.allocation.clusterCount, 6u);
    EXPECT_EQ(test::readExtents(*m.source, record.allocation.extents), data);
}

TEST(ExFatFilesystemTest, ChainedFileInConsecutiveClustersIsOneExtent) {
    ExFatImageBuilder builder;
    const auto data = bytesOf(4 * 512, 8);
    (void)builder.addFileInClusters(builder.root(), "chained.bin", data, {700, 701, 702, 703});
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord& record = scan.records.at(0);
    EXPECT_EQ(record.allocation.method, AllocationMethod::ClusterChain);
    EXPECT_EQ(record.allocation.fragmentCount(), 1u);
    EXPECT_EQ(test::readExtents(*m.source, record.allocation.extents), data);
}

TEST(ExFatFilesystemTest, LongAndUnicodeNames) {
    ExFatImageBuilder builder;
    const std::string unicode =
        "\xC3\x9Cn\xC3\xAF"
        "c\xC3\xB8"
        "d\xC3\xA9 \xE5\x86\x99\xE7\x9C\x9F \xF0\x9F\x98\x80.jpeg";
    const std::string greek = "\xCE\xB1\xCE\xB2\xCE\xB3 \xCF\x82 \xD0\xB4\xD0\xB0.txt";  // "αβγ ς да.txt"
    const std::string longest = std::string(250, 'x') + ".jpeg";                        // 255 characters
    const std::string exactly15 = "abcdefghijklmno";                                    // fills one name entry
    const std::string sixteen = "abcdefghijklmnop";
    for (const std::string* name : {&unicode, &greek, &longest, &exactly15, &sixteen}) {
        (void)builder.addFile(builder.root(), *name, bytesOf(700, name->size()));
    }
    const Mounted m = mount(builder.build());

    const FileScan scan = m.scan();
    ASSERT_EQ(scan.records.size(), 5u);
    EXPECT_EQ(scan.records[0].entry.name, unicode);
    EXPECT_EQ(scan.records[0].path, "/" + unicode);
    EXPECT_EQ(scan.records[1].entry.name, greek);
    EXPECT_EQ(scan.records[2].entry.name, longest);
    EXPECT_EQ(scan.records[3].entry.name, exactly15);
    EXPECT_EQ(scan.records[4].entry.name, sixteen);
    for (const FileRecord& record : scan.records) {
        // The Greek and Cyrillic hashes only verify with the volume's up-case table.
        EXPECT_TRUE(record.entry.issues.empty()) << record.entry.name << ": " << issuesOf(record);
    }
}

TEST(ExFatFilesystemTest, DecodesTimestampsAndAttributes) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.txt", bytesOf(10, 1));
    storeLe16(builder.slot(file, 0), 4, 0x20 | 0x01 | 0x02);
    builder.rechecksum(file);
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const DirectoryEntry& entry = scanned.records.at(0).entry;

    using namespace std::chrono;
    // Stored as 13:45:30 local time at UTC+02:00.
    const auto utc = sys_days{year{2024} / 5 / 17} + hours{11} + minutes{45} + seconds{30};
    ASSERT_TRUE(entry.modified.has_value());
    EXPECT_EQ(entry.modified->time, utc);
    EXPECT_FALSE(entry.modified->local);
    ASSERT_TRUE(entry.created.has_value());
    EXPECT_EQ(entry.created->time, utc + milliseconds{500});
    ASSERT_TRUE(entry.accessed.has_value());
    EXPECT_EQ(entry.accessed->time, utc);
    EXPECT_TRUE(entry.attributes.readOnly);
    EXPECT_TRUE(entry.attributes.hidden);
    EXPECT_TRUE(entry.attributes.archive);
    EXPECT_FALSE(entry.attributes.system);
    EXPECT_TRUE(entry.issues.empty());
}

TEST(ExFatFilesystemTest, TimestampsWithoutUtcOffsetAreLocal) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.txt", bytesOf(10, 1));
    builder.slot(file, 0)[23] = std::byte{0x00};  // modified: offset not recorded
    builder.slot(file, 0)[24] = std::byte{0x80 | 0x7C};  // accessed: UTC-01:00
    builder.rechecksum(file);
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const DirectoryEntry& entry = scanned.records.at(0).entry;
    using namespace std::chrono;
    const auto local = sys_days{year{2024} / 5 / 17} + hours{13} + minutes{45} + seconds{30};
    ASSERT_TRUE(entry.modified.has_value());
    EXPECT_TRUE(entry.modified->local);
    EXPECT_EQ(entry.modified->time, local);
    ASSERT_TRUE(entry.accessed.has_value());
    EXPECT_FALSE(entry.accessed->local);
    EXPECT_EQ(entry.accessed->time, local + hours{1});
}

TEST(ExFatFilesystemTest, InvalidTimestampIsFlagged) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.txt", bytesOf(10, 1));
    storeLe32(builder.slot(file, 0), 12, (((44U << 9) | (13U << 5) | 1U) << 16));  // month 13
    builder.slot(file, 0)[20] = std::byte{250};                                    // 10 ms increment > 199
    builder.rechecksum(file);
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const DirectoryEntry& entry = scanned.records.at(0).entry;
    EXPECT_FALSE(entry.modified.has_value());
    EXPECT_FALSE(entry.created.has_value());
    EXPECT_TRUE(entry.hasIssue(EntryIssue::InvalidTimestamp));
}

TEST(ExFatFilesystemTest, EmptyFile) {
    ExFatImageBuilder builder;
    (void)builder.addFile(builder.root(), "empty.txt", {});
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const FileRecord& record = scanned.records.at(0);
    EXPECT_EQ(record.entry.size, 0u);
    EXPECT_EQ(record.entry.firstCluster, ClusterNumber{0});
    EXPECT_EQ(record.allocation.method, AllocationMethod::None);
    EXPECT_TRUE(record.allocation.extents.empty());
    EXPECT_TRUE(record.allocation.issues.empty());
}

TEST(ExFatFilesystemTest, ValidDataLengthIsReported) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "preallocated.bin", bytesOf(3000, 1));
    storeLe64(builder.slot(file, 1), 8, 1000);  // only the first 1000 bytes were written
    builder.rechecksum(file);
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const FileRecord& record = scanned.records.at(0);
    EXPECT_EQ(record.entry.size, 3000u);
    EXPECT_EQ(record.entry.validDataLength, 1000u);
    EXPECT_TRUE(record.entry.issues.empty());
    EXPECT_EQ(record.allocation.dataBytes(), 3000u);  // the allocation still covers the whole size
}

TEST(ExFatFilesystemTest, LargeDirectorySpanningClusters) {
    ExFatImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "Many files");
    (void)builder.addFile(builder.root(), "blocker.bin", bytesOf(512, 9));  // takes the cluster after the folder
    for (int i = 0; i < 60; ++i) {
        (void)builder.addFile(folder.clusters[0], "photo number " + std::to_string(i) + ".jpg",
                              bytesOf(10, static_cast<std::uint64_t>(i)));
    }
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.issues.empty()) << scan.issues.front().detail;
    EXPECT_EQ(scan.records.size(), 62u);
    for (int i = 0; i < 60; ++i) {
        const FileRecord* record = find(scan, "/Many files/photo number " + std::to_string(i) + ".jpg");
        ASSERT_NE(record, nullptr) << i;
        EXPECT_TRUE(record->entry.issues.empty()) << i;
    }
    const FileRecord* directory = find(scan, "/Many files");
    ASSERT_NE(directory, nullptr);
    // The directory grew past the blocker, so it is now FAT-chained and fragmented.
    EXPECT_FALSE(directory->entry.contiguousData);
    EXPECT_EQ(directory->allocation.method, AllocationMethod::ClusterChain);
    EXPECT_GT(directory->allocation.fragmentCount(), 1u);
    EXPECT_TRUE(directory->allocation.issues.empty()) << issuesOf(*directory);
}

TEST(ExFatFilesystemTest, ContiguousDirectoryGrowsInPlace) {
    ExFatImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "Grown");
    for (int i = 0; i < 20; ++i) {
        (void)builder.addFile(folder.clusters[0], "empty " + std::to_string(i), {});  // allocates no clusters
    }
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.issues.empty()) << scan.issues.front().detail;
    EXPECT_EQ(scan.records.size(), 21u);
    const FileRecord* directory = find(scan, "/Grown");
    ASSERT_NE(directory, nullptr);
    // 20 three-entry sets need four 512-byte clusters, all taken from the free space after the first.
    EXPECT_TRUE(directory->entry.contiguousData);
    EXPECT_EQ(directory->entry.size, 4u * 512u);
    EXPECT_EQ(directory->allocation.method, AllocationMethod::Contiguous);
    EXPECT_EQ(directory->allocation.fragmentCount(), 1u);
    EXPECT_TRUE(directory->allocation.issues.empty()) << issuesOf(*directory);
}

TEST(ExFatFilesystemTest, RootDirectorySpanningClusters) {
    ExFatImageBuilder builder;
    for (int i = 0; i < 40; ++i) {
        (void)builder.addFile(builder.root(), "root file " + std::to_string(i),
                              bytesOf(5, static_cast<std::uint64_t>(i)));
    }
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.issues.empty()) << scan.issues.front().detail;
    EXPECT_EQ(scan.records.size(), 40u);
    const Result<FileAllocation> root = m.fs->resolveAllocation(m.fs->rootDirectory());
    RECOVERY_ASSERT_OK(root);
    EXPECT_GT(root->clusterCount, 1u);
    EXPECT_TRUE(root->issues.empty());
}

TEST(ExFatFilesystemTest, VendorExtensionEntriesAreSkipped) {
    ExFatImageBuilder builder;
    const auto data = bytesOf(700, 4);
    (void)builder.addFile(builder.root(), "vendor.bin", data, 2);
    (void)builder.addFile(builder.root(), "after.bin", bytesOf(10, 5));
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    ASSERT_EQ(scan.records.size(), 2u);
    EXPECT_TRUE(scan.issues.empty()) << scan.issues.front().detail;
    EXPECT_TRUE(scan.records[0].entry.issues.empty()) << issuesOf(scan.records[0]);
    EXPECT_EQ(test::readExtents(*m.source, scan.records[0].allocation.extents), data);
    EXPECT_EQ(scan.records[1].entry.name, "after.bin");
}

TEST(ExFatFilesystemTest, MetadataOffsetPointsAtTheFileEntry) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "x.bin", bytesOf(10, 1));
    const Mounted m = mount(builder.build());
    EXPECT_EQ(m.scan().records.at(0).entry.metadataOffset, builder.slotOffset(file, 0));
}

// ---------------------------------------------------------------------------
// Deleted entries
// ---------------------------------------------------------------------------

TEST(ExFatFilesystemTest, DeletedContiguousFileKeepsItsExactRun) {
    ExFatImageBuilder builder;
    const auto data = bytesOf(3000, 11);
    const auto file = builder.addFile(builder.root(), "Vacation photo.jpg", data);
    (void)builder.addFile(builder.root(), "keep.txt", bytesOf(10, 12));
    builder.deleteEntry(file);
    const Mounted m = mount(builder.build());

    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/Vacation photo.jpg");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->entry.state, EntryState::Deleted);
    EXPECT_TRUE(record->entry.issues.empty()) << issuesOf(*record);  // the whole name survives
    EXPECT_EQ(record->entry.size, data.size());
    ASSERT_TRUE(record->entry.modified.has_value());
    EXPECT_EQ(record->allocation.method, AllocationMethod::Contiguous);
    EXPECT_TRUE(record->allocation.issues.empty()) << issuesOf(*record);
    EXPECT_EQ(test::readExtents(*m.source, record->allocation.extents), data);
    EXPECT_EQ(find(scan, "/keep.txt")->entry.state, EntryState::Active);
}

TEST(ExFatFilesystemTest, DeletedFragmentedFileUsesItsSurvivingChain) {
    ExFatImageBuilder builder;
    const auto data = bytesOf(3 * 512 + 20, 5);
    const auto file = builder.addFileInClusters(builder.root(), "frag.bin", data, {500, 501, 800, 650});
    builder.deleteEntry(file);
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const FileRecord& record = scanned.records.at(0);
    EXPECT_EQ(record.entry.state, EntryState::Deleted);
    EXPECT_EQ(record.allocation.method, AllocationMethod::ClusterChain);
    EXPECT_TRUE(record.allocation.issues.empty()) << issuesOf(record);
    EXPECT_EQ(record.allocation.fragmentCount(), 3u);
    EXPECT_EQ(test::readExtents(*m.source, record.allocation.extents), data);
}

TEST(ExFatFilesystemTest, DeletedFragmentedFileWithClearedChainIsOnlyAGuess) {
    test::ExFatBuilderOptions options;
    options.clearFatOnDelete = true;
    ExFatImageBuilder builder(options);
    const auto data = bytesOf(3 * 512, 5);
    const auto file = builder.addFileInClusters(builder.root(), "frag.bin", data, {500, 501, 700});
    builder.deleteEntry(file);
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const FileRecord& record = scanned.records.at(0);
    // Fragment reconstruction (P13) has to resolve such files.
    EXPECT_EQ(record.allocation.method, AllocationMethod::ContiguousGuess);
    ASSERT_EQ(record.allocation.extents.size(), 1u);
    EXPECT_EQ(record.allocation.extents[0], (Extent{builder.clusterOffset(500), 3 * 512}));
    EXPECT_NE(test::readExtents(*m.source, record.allocation.extents), data);
}

TEST(ExFatFilesystemTest, DeletedFileWhoseClustersWereReused) {
    ExFatImageBuilder builder;
    const auto old = builder.addFile(builder.root(), "old.bin", bytesOf(2048, 1));
    builder.deleteEntry(old);
    (void)builder.addFileInClusters(builder.root(), "new.bin", bytesOf(1024, 2), {old.clusters[2], old.clusters[3]});
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/old.bin");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->allocation.hasIssue(AllocationIssue::ClustersInUse));
    EXPECT_TRUE(find(scan, "/new.bin")->allocation.issues.empty());
}

TEST(ExFatFilesystemTest, DeletedUnicodeName) {
    ExFatImageBuilder builder;
    const std::string name = "\xC3\xA9t\xC3\xA9 \xCE\xB1\xCF\x89 \xF0\x9F\x8C\x9E.png";
    const auto file = builder.addFile(builder.root(), name, bytesOf(100, 1));
    builder.deleteEntry(file);
    const Mounted m = mount(builder.build());
    const FileScan scanned = m.scan();
    const FileRecord& record = scanned.records.at(0);
    EXPECT_EQ(record.entry.name, name);
    EXPECT_EQ(record.entry.state, EntryState::Deleted);
    EXPECT_TRUE(record.entry.issues.empty()) << issuesOf(record);
}

TEST(ExFatFilesystemTest, FilesInsideDeletedDirectory) {
    ExFatImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "Old Photos");
    const auto data = bytesOf(1500, 21);
    const auto inner = builder.addFile(folder.clusters[0], "beach.jpg", data);
    const auto kept = builder.addFile(folder.clusters[0], "not deleted separately.jpg", bytesOf(700, 22));
    builder.deleteEntry(inner);
    builder.deleteEntry(folder);
    (void)kept;
    const Mounted m = mount(builder.build());

    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.issues.empty()) << scan.issues.front().detail;
    const FileRecord* directory = find(scan, "/Old Photos");
    ASSERT_NE(directory, nullptr);
    EXPECT_EQ(directory->entry.state, EntryState::Deleted);
    const FileRecord* file = find(scan, "/Old Photos/beach.jpg");
    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(file->parentDeleted);
    EXPECT_EQ(file->entry.state, EntryState::Deleted);
    EXPECT_EQ(test::readExtents(*m.source, file->allocation.extents), data);
    // An entry still marked in use inside a deleted directory is deleted as well.
    const FileRecord* other = find(scan, "/Old Photos/not deleted separately.jpg");
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(other->entry.state, EntryState::Deleted);
    EXPECT_TRUE(other->parentDeleted);

    ScanLimits noDeletedDirs;
    noDeletedDirs.recurseIntoDeletedDirectories = false;
    EXPECT_EQ(find(m.scan(noDeletedDirs), "/Old Photos/beach.jpg"), nullptr);
}

TEST(ExFatFilesystemTest, DeletedDirectoryWhoseClusterWasReused) {
    ExFatImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "Gone");
    builder.deleteEntry(folder);
    (void)builder.addFileInClusters(builder.root(), "new.bin", bytesOf(100, 1), {folder.clusters[0]});
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    ASSERT_EQ(scan.issues.size(), 1u);
    EXPECT_EQ(scan.issues[0].kind, ScanIssueKind::DirectoryInvalid);
    EXPECT_EQ(scan.issues[0].path, "/Gone");
}

TEST(ExFatFilesystemTest, OrphanedStreamAndNameEntriesAreRecovered) {
    ExFatImageBuilder builder;
    const auto data = bytesOf(2500, 31);
    const auto old = builder.addFile(builder.root(), "A long deleted file name.jpg", data);  // 2 name entries
    builder.deleteEntry(old);
    // A new file set reuses the first slot of the deleted set: its File entry is gone.
    const std::span<std::byte> file = builder.slot(old, 0);
    file[0] = std::byte{0x85};
    file[1] = std::byte{0x00};  // invalid secondary count: the new set is broken, not listed
    const Mounted m = mount(builder.build());

    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/A long deleted file name.jpg");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->entry.state, EntryState::Deleted);
    EXPECT_TRUE(record->entry.hasIssue(EntryIssue::MetadataIncomplete));
    EXPECT_FALSE(record->entry.modified.has_value());
    EXPECT_EQ(record->entry.metadataOffset, builder.slotOffset(old, 1));
    EXPECT_EQ(record->allocation.method, AllocationMethod::Contiguous);
    EXPECT_EQ(test::readExtents(*m.source, record->allocation.extents), data);
}

TEST(ExFatFilesystemTest, OrphanAfterNewEntrySetOverwroteTheHead) {
    ExFatImageBuilder builder;
    const auto data = bytesOf(900, 32);
    const auto old = builder.addFile(builder.root(), "overwritten head.bin", data);  // 4 slots
    builder.deleteEntry(old);
    // Emulate a new two-slot entry: an in-use File entry overwrites slot 0 and
    // its partner occupies nothing else here; the deleted stream follows.
    const std::span<std::byte> file = builder.slot(old, 0);
    std::fill(file.begin(), file.end(), std::byte{0});
    file[0] = std::byte{0xA0};  // benign primary (volume GUID) with no secondaries
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/overwritten head.bin");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->entry.hasIssue(EntryIssue::MetadataIncomplete));
    EXPECT_EQ(test::readExtents(*m.source, record->allocation.extents), data);
}

TEST(ExFatFilesystemTest, ExcludingDeletedEntries) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "gone.txt", bytesOf(10, 1));
    (void)builder.addFile(builder.root(), "here.txt", bytesOf(10, 1));
    builder.deleteEntry(file);
    const Mounted m = mount(builder.build());
    ScanLimits limits;
    limits.includeDeleted = false;
    const FileScan scan = m.scan(limits);
    ASSERT_EQ(scan.records.size(), 1u);
    EXPECT_EQ(scan.records[0].path, "/here.txt");
}

// ---------------------------------------------------------------------------
// Different geometries
// ---------------------------------------------------------------------------

struct Geometry {
    std::uint8_t bytesPerSectorShift;
    std::uint8_t sectorsPerClusterShift;
    std::uint32_t clusterCount;
};

class ExFatGeometryTest : public ::testing::TestWithParam<Geometry> {};

TEST_P(ExFatGeometryTest, FilesRoundTrip) {
    const Geometry g = GetParam();
    test::ExFatBuilderOptions options;
    options.bytesPerSectorShift = g.bytesPerSectorShift;
    options.sectorsPerClusterShift = g.sectorsPerClusterShift;
    options.clusterCount = g.clusterCount;
    options.fatOffsetSectors = 24;
    ExFatImageBuilder builder(options);
    const std::uint32_t cs = builder.clusterSize();

    std::vector<std::vector<std::byte>> files;
    for (const std::size_t size : {std::size_t{1}, std::size_t{cs - 1}, std::size_t{cs}, std::size_t{cs + 1},
                                   std::size_t{3 * cs + 17}}) {
        files.push_back(bytesOf(size, size));
        (void)builder.addFile(builder.root(), "File of " + std::to_string(size) + " bytes.bin", files.back());
    }
    const auto fragmented = bytesOf(2 * cs + 5, 77);
    const std::uint32_t last = builder.lastCluster();
    (void)builder.addFileInClusters(builder.root(), "fragmented.bin", fragmented, {last, last - 2, last - 4});
    const Mounted m = mount(builder.build(), 1U << g.bytesPerSectorShift);
    ASSERT_NE(m.fs, nullptr);
    EXPECT_EQ(m.fs->info().clusterSize, cs);
    EXPECT_TRUE(m.fs->info().warnings.empty()) << m.fs->info().warnings.front();

    const FileScan scan = m.scan();
    ASSERT_EQ(scan.records.size(), files.size() + 1);
    for (std::size_t i = 0; i < files.size(); ++i) {
        EXPECT_TRUE(scan.records[i].allocation.issues.empty()) << issuesOf(scan.records[i]);
        EXPECT_EQ(test::readExtents(*m.source, scan.records[i].allocation.extents), files[i]) << i;
    }
    EXPECT_EQ(scan.records.back().allocation.fragmentCount(), 3u);
    EXPECT_EQ(test::readExtents(*m.source, scan.records.back().allocation.extents), fragmented);
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->allocated, scan.referencedClusters);
}

INSTANTIATE_TEST_SUITE_P(ClusterSizes, ExFatGeometryTest,
                         ::testing::Values(Geometry{9, 0, 4096}, Geometry{9, 1, 2048}, Geometry{9, 3, 512},
                                           Geometry{9, 6, 64}, Geometry{9, 8, 32}, Geometry{12, 0, 512},
                                           Geometry{12, 3, 64}, Geometry{11, 2, 256}),
                         [](const auto& info) {
                             return std::to_string(1U << info.param.bytesPerSectorShift) + "x" +
                                    std::to_string(1U << info.param.sectorsPerClusterShift);
                         });

// ---------------------------------------------------------------------------
// Allocation analysis
// ---------------------------------------------------------------------------

TEST(ExFatFilesystemTest, ClusterUsageMatchesImage) {
    ExFatImageBuilder builder;
    (void)builder.addFile(builder.root(), "a.bin", bytesOf(5000, 1));
    (void)builder.addDirectory(builder.root(), "dir");
    builder.setFat(3000, 0xFFFFFFF7);  // bad cluster, marked allocated like Windows does
    builder.setAllocated(3000, true);
    const std::uint32_t allocated = builder.allocatedClusters();
    const Mounted m = mount(builder.build());

    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->total, 4096u);
    EXPECT_EQ(usage->bad, 1u);
    EXPECT_EQ(usage->allocated, allocated - 1u);
    EXPECT_EQ(usage->free, 4096u - allocated);
    EXPECT_EQ(usage->invalid, 0u);
    EXPECT_EQ(usage->unreadable, 0u);
    EXPECT_EQ(usage->mirrorMismatches, 0u);
    EXPECT_FALSE(usage->recordedFree.has_value());

    EXPECT_EQ(m.fs->clusterState(ClusterNumber{2}).value(), ClusterState::Allocated);
    EXPECT_EQ(m.fs->clusterState(ClusterNumber{3000}).value(), ClusterState::Bad);
    EXPECT_EQ(m.fs->clusterState(ClusterNumber{3500}).value(), ClusterState::Free);
    RECOVERY_EXPECT_ERROR(m.fs->clusterState(ClusterNumber{1}), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(m.fs->clusterState(ClusterNumber{4098}), ErrorCode::InvalidInput);
}

TEST(ExFatFilesystemTest, OrphanedClustersShowAsUnreferenced) {
    ExFatImageBuilder builder;
    (void)builder.addFile(builder.root(), "a.bin", bytesOf(1000, 1));
    for (std::uint32_t c = 2000; c < 2010; ++c) {
        builder.setAllocated(c, true);  // allocated, but no entry refers to them
    }
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->allocated - scan.referencedClusters, 10u);
}

// ---------------------------------------------------------------------------
// Opening, damaged media and limits
// ---------------------------------------------------------------------------

TEST(ExFatFilesystemTest, FallsBackToBackupBootRegion) {
    ExFatImageBuilder builder;
    (void)builder.addFile(builder.root(), "a.txt", bytesOf(10, 1));
    std::vector<std::byte> image = builder.build();
    std::fill_n(image.begin(), 512, std::byte{0});
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_TRUE(hasWarning(m.fs->info(), "using the backup boot region"));
    EXPECT_EQ(m.scan().records.size(), 1u);
}

TEST(ExFatFilesystemTest, MainChecksumMismatchUsesBackup) {
    ExFatImageBuilder builder;
    std::vector<std::byte> image = builder.build();
    image[3 * 512 + 7] ^= std::byte{0xFF};  // an extended boot sector
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_TRUE(hasWarning(m.fs->info(), "main boot region invalid (checksum mismatch)"));
}

TEST(ExFatFilesystemTest, BothChecksumsBadStillOpens) {
    ExFatImageBuilder builder;
    (void)builder.addFile(builder.root(), "a.txt", bytesOf(10, 1));
    std::vector<std::byte> image = builder.build();
    image[11 * 512] ^= std::byte{0xFF};
    image[23 * 512] ^= std::byte{0xFF};
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_TRUE(hasWarning(m.fs->info(), "checksum mismatch"));
    EXPECT_EQ(m.scan().records.size(), 1u);
}

TEST(ExFatFilesystemTest, RejectsNonExFatVolumes) {
    test::MemoryStorageSource zeros(std::vector<std::byte>(64 * 1024), 512);
    RECOVERY_ASSERT_OK(zeros.open());
    RECOVERY_EXPECT_ERROR(ExFatFilesystem::open(zeros), ErrorCode::UnsupportedFilesystem);

    test::MemoryStorageSource tiny(std::vector<std::byte>(100), 512);
    RECOVERY_ASSERT_OK(tiny.open());
    RECOVERY_EXPECT_ERROR(ExFatFilesystem::open(tiny), ErrorCode::UnsupportedFilesystem);

    test::MemoryStorageSource closed(std::vector<std::byte>(4096), 512);
    RECOVERY_EXPECT_ERROR(ExFatFilesystem::open(closed), ErrorCode::InvalidInput);

    test::Fat32ImageBuilder fat32;
    test::MemoryStorageSource fat(fat32.build(), 512);
    RECOVERY_ASSERT_OK(fat.open());
    RECOVERY_EXPECT_ERROR(ExFatFilesystem::open(fat), ErrorCode::UnsupportedFilesystem);
}

TEST(ExFatFilesystemTest, UnreadableBitmapSector) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", bytesOf(1000, 1));
    const auto gone = builder.addFile(builder.root(), "gone.bin", bytesOf(1000, 2));
    builder.deleteEntry(gone);
    std::vector<std::byte> image = builder.build();
    auto source = std::make_unique<test::MemoryStorageSource>(image, 512);
    source->addBadSector(builder.clusterOffset(builder.bitmapCluster()) / 512);  // bits of clusters 2..4097
    RECOVERY_ASSERT_OK(source->open());
    Result<std::unique_ptr<ExFatFilesystem>> fs = ExFatFilesystem::open(*source);
    RECOVERY_ASSERT_OK(fs);

    EXPECT_EQ(fs.value()->clusterState(ClusterNumber{file.clusters[0]}).value(), ClusterState::Unreadable);
    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    ASSERT_EQ(scan->records.size(), 2u);
    EXPECT_TRUE(scan->records[0].allocation.hasIssue(AllocationIssue::UnreadableAllocationTable));
    EXPECT_TRUE(scan->records[1].allocation.hasIssue(AllocationIssue::UnreadableAllocationTable));
    // The data is still located exactly.
    EXPECT_EQ(test::readExtents(*source, scan->records[1].allocation.extents), bytesOf(1000, 2));

    const Result<ClusterUsage> usage = fs.value()->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->unreadable, 4096u);
}

TEST(ExFatFilesystemTest, UnreadableFatSectorBreaksOnlyChains) {
    ExFatImageBuilder builder;
    const auto contiguous = bytesOf(2000, 1);
    (void)builder.addFile(builder.root(), "contiguous.bin", contiguous);
    (void)builder.addFileInClusters(builder.root(), "chained.bin", bytesOf(1024, 2), {60, 90});  // FAT sector 0
    std::vector<std::byte> image = builder.build();
    auto source = std::make_unique<test::MemoryStorageSource>(image, 512);
    source->addBadSector(builder.fatOffset() / 512);  // entries 0-127
    RECOVERY_ASSERT_OK(source->open());
    Result<std::unique_ptr<ExFatFilesystem>> fs = ExFatFilesystem::open(*source);
    RECOVERY_ASSERT_OK(fs);
    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    const FileRecord* ok = find(*scan, "/contiguous.bin");
    ASSERT_NE(ok, nullptr);
    EXPECT_TRUE(ok->allocation.issues.empty());
    EXPECT_EQ(test::readExtents(*source, ok->allocation.extents), contiguous);
    const FileRecord* broken = find(*scan, "/chained.bin");
    ASSERT_NE(broken, nullptr);
    EXPECT_TRUE(broken->allocation.hasIssue(AllocationIssue::UnreadableAllocationTable));
    EXPECT_TRUE(broken->allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
}

TEST(ExFatFilesystemTest, UnreadableDirectorySectorIsReported) {
    ExFatImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "Broken");
    (void)builder.addFile(folder.clusters[0], "lost.txt", bytesOf(10, 1));
    (void)builder.addFile(builder.root(), "fine.txt", bytesOf(10, 1));
    std::vector<std::byte> image = builder.build();
    auto source = std::make_unique<test::MemoryStorageSource>(image, 512);
    source->addBadSector(builder.clusterOffset(folder.clusters[0]) / 512);
    RECOVERY_ASSERT_OK(source->open());
    Result<std::unique_ptr<ExFatFilesystem>> fs = ExFatFilesystem::open(*source);
    RECOVERY_ASSERT_OK(fs);

    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    EXPECT_EQ(scan->records.size(), 2u);  // Broken and fine.txt
    ASSERT_EQ(scan->issues.size(), 1u);
    EXPECT_EQ(scan->issues[0].kind, ScanIssueKind::DirectoryUnreadable);
    EXPECT_EQ(scan->issues[0].path, "/Broken");
}

TEST(ExFatFilesystemTest, EntrySetAcrossAnUnreadableSectorIsDropped) {
    test::ExFatBuilderOptions options;
    options.sectorsPerClusterShift = 1;  // 1 KiB clusters: two sectors per directory cluster
    ExFatImageBuilder builder(options);
    const auto folder = builder.addDirectory(builder.root(), "Split");
    // 16 entries per sector; 5 three-slot sets fill 15, the sixth straddles the sectors.
    for (int i = 0; i < 8; ++i) {
        (void)builder.addFile(folder.clusters[0], "f" + std::to_string(i), bytesOf(10, static_cast<std::uint64_t>(i)));
    }
    std::vector<std::byte> image = builder.build();
    auto source = std::make_unique<test::MemoryStorageSource>(image, 512);
    source->addBadSector(builder.clusterOffset(folder.clusters[0]) / 512 + 1);
    RECOVERY_ASSERT_OK(source->open());
    Result<std::unique_ptr<ExFatFilesystem>> fs = ExFatFilesystem::open(*source);
    RECOVERY_ASSERT_OK(fs);
    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    // f0-f4 are in the readable sector; f5 is cut off and must not be half-parsed.
    EXPECT_NE(find(*scan, "/Split/f4"), nullptr);
    EXPECT_EQ(find(*scan, "/Split/f5"), nullptr);
    for (const FileRecord& record : scan->records) {
        EXPECT_TRUE(record.entry.issues.empty()) << record.path << ": " << issuesOf(record);
    }
}

TEST(ExFatFilesystemTest, MissingUpcaseTableFallsBack) {
    ExFatImageBuilder builder;
    const std::string greek = "\xCE\xB1\xCE\xB2\xCE\xB3.txt";
    (void)builder.addFile(builder.root(), "plain ascii.txt", bytesOf(10, 1));
    (void)builder.addFile(builder.root(), "\xC3\xA9t\xC3\xA9.txt", bytesOf(10, 2));
    (void)builder.addFile(builder.root(), greek, bytesOf(10, 3));
    // Turn the up-case table entry into an unused entry.
    std::vector<std::byte> image = builder.build();
    auto slot = std::span<std::byte>(image).subspan(
        static_cast<std::size_t>(builder.clusterOffset(builder.root())) + 2 * 32, 32);
    ASSERT_EQ(slot[0], std::byte{0x82});
    slot[0] = std::byte{0x02};
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_FALSE(m.fs->usesVolumeUpcaseTable());
    EXPECT_TRUE(hasWarning(m.fs->info(), "up-case table entry is missing"));

    const FileScan scan = m.scan();
    ASSERT_EQ(scan.records.size(), 3u);
    EXPECT_TRUE(scan.records[0].entry.issues.empty());
    EXPECT_TRUE(scan.records[1].entry.issues.empty());  // Latin-1 is covered by the fallback
    EXPECT_TRUE(scan.records[2].entry.hasIssue(EntryIssue::LongNameUnverified));
    EXPECT_FALSE(scan.records[2].entry.hasIssue(EntryIssue::NameHashMismatch));
    EXPECT_EQ(scan.records[2].entry.name, greek);
}

TEST(ExFatFilesystemTest, CorruptedUpcaseTableFallsBack) {
    ExFatImageBuilder builder;
    std::vector<std::byte> image = builder.build();
    image[static_cast<std::size_t>(builder.clusterOffset(builder.upcaseCluster())) + 200] ^= std::byte{0x01};
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_FALSE(m.fs->usesVolumeUpcaseTable());
    EXPECT_TRUE(hasWarning(m.fs->info(), "up-case table checksum mismatch"));
}

TEST(ExFatFilesystemTest, MissingBitmapLeavesAllocationUnknown) {
    ExFatImageBuilder builder;
    (void)builder.addFile(builder.root(), "a.bin", bytesOf(1000, 1));
    std::vector<std::byte> image = builder.build();
    auto slot =
        std::span<std::byte>(image).subspan(static_cast<std::size_t>(builder.clusterOffset(builder.root())) + 32, 32);
    ASSERT_EQ(slot[0], std::byte{0x81});
    slot[0] = std::byte{0x01};
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_FALSE(m.fs->hasAllocationBitmap());
    EXPECT_TRUE(hasWarning(m.fs->info(), "allocation bitmap entry is missing"));
    const FileScan scan = m.scan();
    ASSERT_EQ(scan.records.size(), 1u);
    EXPECT_TRUE(scan.records[0].allocation.hasIssue(AllocationIssue::UnreadableAllocationTable));
    EXPECT_EQ(test::readExtents(*m.source, scan.records[0].allocation.extents), bytesOf(1000, 1));
    EXPECT_EQ(m.fs->clusterState(ClusterNumber{2}).value(), ClusterState::Unreadable);
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->unreadable, 4096u);
}

TEST(ExFatFilesystemTest, SystemStructuresWithoutFatChainsAreAssumedContiguous) {
    ExFatImageBuilder builder;
    (void)builder.addFile(builder.root(), "a.bin", bytesOf(100, 1));
    for (std::uint32_t c = builder.bitmapCluster(); c < builder.root(); ++c) {
        builder.setFat(c, 0);  // some formatters leave these entries empty
    }
    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    EXPECT_TRUE(m.fs->hasAllocationBitmap());
    EXPECT_TRUE(m.fs->usesVolumeUpcaseTable());
    EXPECT_TRUE(hasWarning(m.fs->info(), "assuming it is contiguous"));
    EXPECT_TRUE(m.scan().records.at(0).allocation.issues.empty());
}

TEST(ExFatFilesystemTest, TruncatedImage) {
    ExFatImageBuilder builder;
    const auto early = bytesOf(1000, 1);
    (void)builder.addFile(builder.root(), "early.bin", early);
    builder.skipClusters(3000);
    (void)builder.addFile(builder.root(), "late.bin", bytesOf(1000, 2));
    std::vector<std::byte> image = builder.build();
    image.resize(static_cast<std::size_t>(builder.clusterOffset(1000)));
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_TRUE(hasWarning(m.fs->info(), "truncated"));
    const FileScan scan = m.scan();
    ASSERT_EQ(scan.records.size(), 2u);
    EXPECT_EQ(test::readExtents(*m.source, find(scan, "/early.bin")->allocation.extents), early);
    // The late file's extent lies beyond the image: recovery must treat it as unreadable.
    EXPECT_GT(find(scan, "/late.bin")->allocation.extents.at(0).offset, m.source->size());
}

TEST(ExFatFilesystemTest, DepthAndEntryLimits) {
    ExFatImageBuilder builder;
    std::uint32_t parent = builder.root();
    for (int depth = 0; depth < 5; ++depth) {
        parent = builder.addDirectory(parent, "level" + std::to_string(depth)).clusters[0];
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

TEST(ExFatFilesystemTest, ListingLimit) {
    ExFatImageBuilder builder;
    for (int i = 0; i < 30; ++i) {
        (void)builder.addFile(builder.root(), "f" + std::to_string(i), bytesOf(1, 1));
    }
    const std::vector<std::byte> image = builder.build();
    test::MemoryStorageSource source(image, 512);
    RECOVERY_ASSERT_OK(source.open());
    ExFatOptions options;
    options.maxListingEntries = 10;
    Result<std::unique_ptr<ExFatFilesystem>> fs = ExFatFilesystem::open(source, options);
    RECOVERY_ASSERT_OK(fs);
    const Result<DirectoryListing> listing = fs.value()->readDirectory(fs.value()->rootDirectory());
    RECOVERY_ASSERT_OK(listing);
    EXPECT_GE(listing->entries.size(), 10u);
    EXPECT_LT(listing->entries.size(), 30u);
    ASSERT_FALSE(listing->issues.empty());
    EXPECT_EQ(listing->issues.back().kind, ScanIssueKind::EntryLimit);
}

TEST(ExFatFilesystemTest, ScanCanBeCancelled) {
    ExFatImageBuilder builder;
    (void)builder.addFile(builder.root(), "a.txt", bytesOf(10, 1));
    const Mounted m = mount(builder.build());
    CancellationSource cancel;
    cancel.requestCancellation();
    RECOVERY_EXPECT_ERROR(m.fs->scan({}, cancel.token()), ErrorCode::Cancelled);
    RECOVERY_EXPECT_ERROR(m.fs->analyzeClusters(cancel.token()), ErrorCode::Cancelled);
}

TEST(ExFatFilesystemTest, ReadDirectoryDirectly) {
    ExFatImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "Sub folder");
    (void)builder.addFile(folder.clusters[0], "x.txt", bytesOf(10, 1));
    const Mounted m = mount(builder.build());

    const Result<DirectoryListing> root = m.fs->readDirectory(m.fs->rootDirectory());
    RECOVERY_ASSERT_OK(root);
    ASSERT_EQ(root->entries.size(), 1u);
    EXPECT_EQ(root->entries[0].name, "Sub folder");

    const Result<DirectoryListing> sub = m.fs->readDirectory(root->entries[0]);
    RECOVERY_ASSERT_OK(sub);
    ASSERT_EQ(sub->entries.size(), 1u);
    EXPECT_EQ(sub->entries[0].name, "x.txt");

    RECOVERY_EXPECT_ERROR(m.fs->readDirectory(sub->entries[0]), ErrorCode::InvalidInput);
    const Result<FileAllocation> allocation = m.fs->resolveAllocation(sub->entries[0]);
    RECOVERY_ASSERT_OK(allocation);
    EXPECT_EQ(allocation->method, AllocationMethod::Contiguous);
}

// ---------------------------------------------------------------------------
// exFAT inside a partition table (P3 + P5)
// ---------------------------------------------------------------------------

TEST(ExFatFilesystemTest, OpensInsideMbrPartition) {
    ExFatImageBuilder builder;
    const auto data = bytesOf(2500, 42);
    (void)builder.addFile(builder.root(), "in partition.jpg", data);
    const std::vector<std::byte> volume = builder.build();

    constexpr std::uint32_t kStart = 2048;
    std::vector<std::byte> disk((kStart + volume.size() / 512 + 100) * 512);
    std::copy(volume.begin(), volume.end(), disk.begin() + kStart * 512);
    test::writeMbrSector(disk, 512, 0, {{0x80, 0x07, kStart, static_cast<std::uint32_t>(volume.size() / 512)}});

    test::MemoryStorageSource device(disk, 512);
    RECOVERY_ASSERT_OK(device.open());
    const Result<partition::PartitionTable> table = partition::readPartitionTable(device);
    RECOVERY_ASSERT_OK(table);
    ASSERT_EQ(table->partitions.size(), 1u);
    ASSERT_TRUE(table->partitions[0].candidates.exfat);

    partition::PartitionSource part(device, table->partitions[0]);
    RECOVERY_ASSERT_OK(part.open());
    Result<std::unique_ptr<ExFatFilesystem>> fs = ExFatFilesystem::open(part);
    RECOVERY_ASSERT_OK(fs);
    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    ASSERT_EQ(scan->records.size(), 1u);
    EXPECT_EQ(scan->records[0].entry.name, "in partition.jpg");
    EXPECT_EQ(test::readExtents(part, scan->records[0].allocation.extents), data);
}

TEST(ExFatFilesystemTest, SuperfloppyVolume) {
    ExFatImageBuilder builder;
    (void)builder.addFile(builder.root(), "a.txt", bytesOf(10, 1));
    test::MemoryStorageSource device(builder.build(), 512);
    RECOVERY_ASSERT_OK(device.open());
    const Result<partition::PartitionTable> table = partition::readPartitionTable(device);
    RECOVERY_ASSERT_OK(table);
    EXPECT_EQ(table->scheme, partition::PartitionScheme::Unpartitioned);
    ASSERT_EQ(table->partitions.size(), 1u);
    EXPECT_TRUE(table->partitions[0].candidates.exfat);
}

TEST(ExFatFilesystemTest, LogsOpenAndScan) {
    ExFatImageBuilder builder;
    auto sink = std::make_shared<diagnostics::MemorySink>();
    diagnostics::Logger logger(diagnostics::LogLevel::Debug);
    logger.addSink(sink);
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    Result<std::unique_ptr<ExFatFilesystem>> fs = ExFatFilesystem::open(source, {}, &logger);
    RECOVERY_ASSERT_OK(fs);
    RECOVERY_ASSERT_OK(fs.value()->scan({}, {}));
    const auto records = sink->records();
    EXPECT_TRUE(std::any_of(records.begin(), records.end(),
                            [](const auto& r) { return r.message == "exFAT volume opened"; }));
    EXPECT_TRUE(std::any_of(records.begin(), records.end(),
                            [](const auto& r) { return r.message == "exFAT scan finished"; }));
}

}  // namespace
}  // namespace recovery::filesystem::exfat
