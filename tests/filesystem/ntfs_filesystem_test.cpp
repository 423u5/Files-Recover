// NTFS filesystem: normal, resident, non-resident, fragmented, deleted,
// orphaned and Unicode-named files on generated images, read through the
// filesystem-independent interface.

#include "filesystem/ntfs/ntfs_filesystem.hpp"

#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/byte_order.hpp"
#include "support/fat32_builder.hpp"  // readExtents
#include "support/memory_source.hpp"
#include "support/ntfs_builder.hpp"
#include "support/partition_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

namespace recovery::filesystem::ntfs {
namespace {

using test::NtfsImageBuilder;

constexpr std::uint64_t kRoot = NtfsImageBuilder::root();

std::vector<std::byte> bytesOf(std::size_t size, std::uint64_t seed) {
    return test::makePattern(size, seed);
}

// Opens an image and keeps the source alive alongside the filesystem.
struct Mounted {
    std::unique_ptr<test::MemoryStorageSource> source;
    std::unique_ptr<NtfsFilesystem> fs;

    [[nodiscard]] FileScan scan(ScanLimits limits = {}) const {
        Result<FileScan> result = fs->scan(limits, {});
        EXPECT_TRUE(result.ok()) << (result.ok() ? "" : describe(result.error()));
        return result.ok() ? std::move(result).value() : FileScan{};
    }

    // The bytes a recovery would produce from an allocation.
    [[nodiscard]] std::vector<std::byte> content(const FileAllocation& allocation) const {
        if (allocation.method == AllocationMethod::Resident) {
            return allocation.residentData;
        }
        return test::readExtents(*source, allocation.extents);
    }
};

Mounted mount(std::vector<std::byte> image, NtfsOptions options = {}, std::uint32_t sectorSize = 512) {
    Mounted m;
    m.source = std::make_unique<test::MemoryStorageSource>(std::move(image), sectorSize);
    EXPECT_TRUE(m.source->open().ok());
    Result<std::unique_ptr<NtfsFilesystem>> fs = NtfsFilesystem::open(*m.source, options);
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

std::string issuesOf(const FileScan& scan) {
    std::string text;
    for (const ScanIssue& issue : scan.issues) {
        text += std::string(toString(issue.kind)) + " " + issue.path + ": " + issue.detail + "\n";
    }
    return text;
}

bool hasWarning(const FilesystemInfo& info, std::string_view text) {
    return std::any_of(info.warnings.begin(), info.warnings.end(),
                       [&](const std::string& w) { return w.find(text) != std::string::npos; });
}

// Records whose path starts with "/$" are NTFS metadata files.
std::vector<const FileRecord*> userRecords(const FileScan& scan) {
    std::vector<const FileRecord*> out;
    for (const FileRecord& record : scan.records) {
        if (!record.path.starts_with("/$")) {
            out.push_back(&record);
        }
    }
    return out;
}

TEST(NtfsFilesystemTest, OpensAndReportsInfo) {
    NtfsImageBuilder builder;
    const Mounted m = mount(builder.build());
    ASSERT_NE(m.fs, nullptr);
    const FilesystemInfo& info = m.fs->info();
    EXPECT_EQ(info.type, FilesystemType::Ntfs);
    EXPECT_EQ(info.label, "TESTVOL");
    EXPECT_EQ(info.serialNumber, 0x1122334455667788u);
    EXPECT_EQ(info.bytesPerSector, 512u);
    EXPECT_EQ(info.clusterSize, 512u);
    EXPECT_EQ(info.clusterCount, 8192u);
    EXPECT_EQ(info.firstCluster, 0u);
    EXPECT_EQ(info.dataOffset, 0u);
    EXPECT_EQ(info.volumeSize, 8192u * 512u);
    EXPECT_TRUE(info.warnings.empty()) << info.warnings.front();
    EXPECT_EQ(m.fs->majorVersion(), 3u);
    EXPECT_EQ(m.fs->minorVersion(), 1u);
    EXPECT_EQ(m.fs->recordCount(), 128u);
    EXPECT_TRUE(m.fs->hasClusterBitmap());
    EXPECT_FALSE(m.fs->usedMftMirror());
    EXPECT_EQ(m.fs->bootSector().mftCluster, builder.mftCluster());
    EXPECT_EQ(m.fs->recordOffset(64), builder.recordOffset(64));
}

TEST(NtfsFilesystemTest, FreshVolumeHoldsOnlyMetadataFiles) {
    NtfsImageBuilder builder;
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.complete);
    EXPECT_TRUE(scan.issues.empty()) << issuesOf(scan);
    EXPECT_EQ(scan.crossLinkedClusters, 0u);
    std::set<std::string> paths;
    for (const FileRecord& record : scan.records) {
        paths.insert(record.path);
        EXPECT_EQ(record.entry.state, EntryState::Active) << record.path;
        EXPECT_TRUE(record.entry.attributes.hidden && record.entry.attributes.system) << record.path;
        EXPECT_TRUE(record.entry.issues.empty()) << record.path << ": " << issuesOf(record);
        EXPECT_TRUE(record.allocation.issues.empty()) << record.path << ": " << issuesOf(record);
    }
    EXPECT_EQ(paths, (std::set<std::string>{"/$MFT", "/$MFTMirr", "/$LogFile", "/$Volume", "/$AttrDef", "/$Bitmap",
                                            "/$Boot", "/$BadClus", "/$Secure", "/$UpCase", "/$Extend"}));
    EXPECT_TRUE(find(scan, "/$Extend")->entry.isDirectory);
    EXPECT_EQ(find(scan, "/$MFT")->entry.size, 128u * 1024u);
    EXPECT_EQ(find(scan, "/$Boot")->allocation.extents.front().offset, 0u);

    // Every allocated cluster belongs to some attribute of an in-use record.
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->total, 8192u);
    EXPECT_EQ(usage->allocated, builder.allocatedClusters());
    EXPECT_EQ(usage->free, 8192u - builder.allocatedClusters());
    EXPECT_EQ(usage->bad, 0u);
    EXPECT_EQ(usage->unreadable, 0u);
    EXPECT_FALSE(usage->recordedFree.has_value());
    EXPECT_EQ(scan.referencedClusters, usage->allocated);
}

TEST(NtfsFilesystemTest, FindsNormalFilesAndDirectories) {
    NtfsImageBuilder builder;
    const auto small = bytesOf(100, 1);
    const auto multi = bytesOf(5000, 2);
    const auto nested = bytesOf(1234, 3);
    (void)builder.addFile(kRoot, "small.txt", small);
    (void)builder.addFile(kRoot, "multi.bin", multi);
    const auto dcim = builder.addDirectory(kRoot, "DCIM");
    const auto camera = builder.addDirectory(dcim.record, "100MEDIA");
    (void)builder.addFile(camera.record, "IMG_0001.JPG", nested);
    const Mounted m = mount(builder.build());

    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.complete);
    EXPECT_TRUE(scan.issues.empty()) << issuesOf(scan);
    EXPECT_EQ(scan.crossLinkedClusters, 0u);
    EXPECT_EQ(userRecords(scan).size(), 5u);

    const std::map<std::string, const std::vector<std::byte>*> expected = {
        {"/small.txt", &small}, {"/multi.bin", &multi}, {"/DCIM/100MEDIA/IMG_0001.JPG", &nested}};
    for (const auto& [path, data] : expected) {
        const FileRecord* record = find(scan, path);
        ASSERT_NE(record, nullptr) << path;
        EXPECT_EQ(record->entry.state, EntryState::Active);
        EXPECT_FALSE(record->entry.isDirectory);
        EXPECT_FALSE(record->parentDeleted);
        EXPECT_TRUE(record->entry.issues.empty()) << path << ": " << issuesOf(*record);
        EXPECT_TRUE(record->allocation.issues.empty()) << path << ": " << issuesOf(*record);
        EXPECT_EQ(record->entry.size, data->size());
        EXPECT_EQ(record->entry.validDataLength, data->size());
        EXPECT_TRUE(record->entry.attributes.archive);
        EXPECT_FALSE(record->entry.attributes.hidden);
        EXPECT_EQ(m.content(record->allocation), *data) << path;
    }
    EXPECT_EQ(find(scan, "/small.txt")->allocation.method, AllocationMethod::Resident);
    EXPECT_EQ(find(scan, "/multi.bin")->allocation.method, AllocationMethod::RunList);

    const FileRecord* directory = find(scan, "/DCIM/100MEDIA");
    ASSERT_NE(directory, nullptr);
    EXPECT_TRUE(directory->entry.isDirectory);
    EXPECT_EQ(directory->entry.name, "100MEDIA");
    EXPECT_EQ(directory->entry.size, 0u);
    EXPECT_EQ(directory->allocation.method, AllocationMethod::None);  // its index fits in the record

    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->allocated, builder.allocatedClusters());
    EXPECT_EQ(scan.referencedClusters, usage->allocated);
}

TEST(NtfsFilesystemTest, TimestampsAreUtc) {
    NtfsImageBuilder builder;
    (void)builder.addFile(kRoot, "a.txt", bytesOf(10, 1));
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/a.txt");
    ASSERT_NE(record, nullptr);
    using namespace std::chrono;
    const auto base = sys_days{year{2024} / 5 / 17} + hours{11} + minutes{45} + seconds{30};
    ASSERT_TRUE(record->entry.created.has_value());
    EXPECT_EQ(record->entry.created->time, base + milliseconds{500});
    EXPECT_FALSE(record->entry.created->local);
    ASSERT_TRUE(record->entry.modified.has_value());
    EXPECT_EQ(record->entry.modified->time, base);
    ASSERT_TRUE(record->entry.accessed.has_value());
    EXPECT_EQ(record->entry.accessed->time, base);
}

TEST(NtfsFilesystemTest, ResidentDataIsReadWithFixupsApplied) {
    NtfsImageBuilder builder;
    // 600 bytes cross the end of the record's first 512-byte block, where the
    // disk holds the update sequence number instead of the data.
    const auto data = bytesOf(600, 7);
    const auto file = builder.addFile(kRoot, "note.txt", data);
    (void)builder.addFile(kRoot, "empty.txt", {});
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();

    const FileRecord* record = find(scan, "/note.txt");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->allocation.method, AllocationMethod::Resident);
    EXPECT_TRUE(record->allocation.extents.empty());
    EXPECT_EQ(record->allocation.residentData, data);
    EXPECT_EQ(record->allocation.dataBytes(), 600u);
    EXPECT_EQ(record->allocation.clusterCount, 0u);
    EXPECT_EQ(record->entry.firstCluster, ClusterNumber{0});
    EXPECT_EQ(record->entry.metadataOffset, builder.recordOffset(file.record));

    const FileRecord* empty = find(scan, "/empty.txt");
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(empty->entry.size, 0u);
    EXPECT_EQ(empty->allocation.method, AllocationMethod::None);
    EXPECT_TRUE(empty->allocation.issues.empty());
}

TEST(NtfsFilesystemTest, ContiguousNonResidentFile) {
    NtfsImageBuilder builder;
    const auto data = bytesOf(10 * 512 + 3, 5);
    const auto file = builder.addFile(kRoot, "movie.mp4", data);
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/movie.mp4");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->entry.contiguousData);
    EXPECT_EQ(record->entry.firstCluster, ClusterNumber{file.clusters.front()});
    EXPECT_EQ(record->allocation.method, AllocationMethod::RunList);
    ASSERT_EQ(record->allocation.fragmentCount(), 1u);
    EXPECT_EQ(record->allocation.extents[0], (Extent{builder.clusterOffset(file.clusters.front()), data.size()}));
    EXPECT_EQ(record->allocation.clusterCount, 11u);
    EXPECT_EQ(m.content(record->allocation), data);
}

TEST(NtfsFilesystemTest, FragmentedFileFollowsItsRuns) {
    NtfsImageBuilder builder;
    const auto data = bytesOf(6 * 512 - 100, 7);
    // The last run lies before the one ahead of it: a negative run offset.
    (void)builder.addFileInClusters(kRoot, "frag.bin", data, {4400, 4401, 4402, 4900, 4901, 4600});
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/frag.bin");
    ASSERT_NE(record, nullptr);
    EXPECT_FALSE(record->entry.contiguousData);
    EXPECT_EQ(record->entry.firstCluster, ClusterNumber{4400});
    EXPECT_EQ(record->allocation.method, AllocationMethod::RunList);
    EXPECT_TRUE(record->allocation.issues.empty()) << issuesOf(*record);
    ASSERT_EQ(record->allocation.fragmentCount(), 3u);
    EXPECT_EQ(record->allocation.extents[0], (Extent{builder.clusterOffset(4400), 3 * 512}));
    EXPECT_EQ(record->allocation.extents[1], (Extent{builder.clusterOffset(4900), 2 * 512}));
    EXPECT_EQ(record->allocation.extents[2], (Extent{builder.clusterOffset(4600), 512 - 100}));
    EXPECT_EQ(record->allocation.clusterCount, 6u);
    EXPECT_EQ(m.content(record->allocation), data);
}

TEST(NtfsFilesystemTest, ManyFragments) {
    NtfsImageBuilder builder;
    std::vector<std::uint64_t> clusters;
    for (std::uint64_t i = 0; i < 60; ++i) {
        clusters.push_back(7000 - i * 3);  // every run goes backwards
    }
    const auto data = bytesOf(60 * 512, 8);
    (void)builder.addFileInClusters(kRoot, "scattered.bin", data, clusters);
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/scattered.bin");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->allocation.issues.empty()) << issuesOf(*record);
    EXPECT_EQ(record->allocation.fragmentCount(), 60u);
    EXPECT_EQ(m.content(record->allocation), data);
}

TEST(NtfsFilesystemTest, PreallocatedClustersBeyondTheSizeAreNotData) {
    NtfsImageBuilder builder;
    const auto data = bytesOf(2 * 512, 9);
    // Five clusters for two clusters of data.
    const auto file = builder.addFileWithRuns(kRoot, "prealloc.bin", data, {{5000, 5}});
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/prealloc.bin");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->allocation.issues.empty()) << issuesOf(*record);
    EXPECT_EQ(record->allocation.clusterCount, 2u);
    EXPECT_EQ(record->allocation.dataBytes(), data.size());
    EXPECT_EQ(m.content(record->allocation), data);
    // All five belong to the file, though.
    EXPECT_EQ(scan.referencedClusters, builder.allocatedClusters());
    EXPECT_EQ(file.clusters.size(), 5u);
}

// ---------------------------------------------------------------------------
// Deleted files.
// ---------------------------------------------------------------------------

TEST(NtfsFilesystemTest, DeletedResidentFileKeepsItsData) {
    NtfsImageBuilder builder;
    const auto data = bytesOf(300, 11);
    const auto file = builder.addFile(kRoot, "gone.txt", data);
    builder.deleteEntry(file);
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/gone.txt");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->entry.state, EntryState::Deleted);
    EXPECT_FALSE(record->parentDeleted);
    EXPECT_TRUE(record->entry.issues.empty()) << issuesOf(*record);
    EXPECT_EQ(record->allocation.method, AllocationMethod::Resident);
    EXPECT_EQ(record->allocation.residentData, data);
}

TEST(NtfsFilesystemTest, DeletedFragmentedFileKeepsItsExactRuns) {
    NtfsImageBuilder builder;
    const auto data = bytesOf(5 * 512 + 17, 12);
    const auto file = builder.addFileInClusters(kRoot, "deleted photo.jpg", data, {3000, 3001, 3500, 3200, 3201, 3202});
    builder.deleteEntry(file);
    for (const std::uint64_t c : file.clusters) {
        EXPECT_FALSE(builder.isAllocated(c));
    }
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/deleted photo.jpg");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->entry.state, EntryState::Deleted);
    EXPECT_EQ(record->entry.size, data.size());
    // Unlike FAT, NTFS keeps the run list of a deleted file: no guessing.
    EXPECT_EQ(record->allocation.method, AllocationMethod::RunList);
    EXPECT_TRUE(record->allocation.issues.empty()) << issuesOf(*record);
    ASSERT_EQ(record->allocation.fragmentCount(), 3u);
    EXPECT_EQ(m.content(record->allocation), data);
    // Deleted files own nothing.
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(scan.referencedClusters, usage->allocated);
}

TEST(NtfsFilesystemTest, DeletedFileWhoseClustersWereReused) {
    NtfsImageBuilder builder;
    const auto file = builder.addFileInClusters(kRoot, "old.bin", bytesOf(3 * 512, 13), {3000, 3001, 3002});
    builder.deleteEntry(file);
    builder.setAllocated(3001, true);  // taken by something else
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/old.bin");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->allocation.method, AllocationMethod::RunList);
    EXPECT_TRUE(record->allocation.hasIssue(AllocationIssue::ClustersInUse));
}

TEST(NtfsFilesystemTest, DeletedDirectoryAndItsFiles) {
    NtfsImageBuilder builder;
    const auto old = builder.addDirectory(kRoot, "Old stuff");
    const auto note = builder.addFile(old.record, "note.txt", bytesOf(900, 14));
    const auto photo = builder.addFileInClusters(old.record, "photo.jpg", bytesOf(2 * 512, 15), {3300, 3301});
    // NTFS deletes a directory's contents first.
    builder.deleteEntry(note);
    builder.deleteEntry(photo);
    builder.deleteEntry(old);
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.issues.empty()) << issuesOf(scan);

    const FileRecord* directory = find(scan, "/Old stuff");
    ASSERT_NE(directory, nullptr);
    EXPECT_EQ(directory->entry.state, EntryState::Deleted);
    EXPECT_TRUE(directory->entry.isDirectory);
    for (const std::string path : {"/Old stuff/note.txt", "/Old stuff/photo.jpg"}) {
        const FileRecord* record = find(scan, path);
        ASSERT_NE(record, nullptr) << path;
        EXPECT_EQ(record->entry.state, EntryState::Deleted);
        EXPECT_TRUE(record->parentDeleted);
        EXPECT_FALSE(record->entry.hasIssue(EntryIssue::ParentMissing));
    }
    EXPECT_EQ(m.content(find(scan, "/Old stuff/photo.jpg")->allocation), bytesOf(2 * 512, 15));

    // Deleted entries can be left out, and deleted directories not entered.
    ScanLimits limits;
    limits.recurseIntoDeletedDirectories = false;
    const FileScan shallow = m.scan(limits);
    EXPECT_NE(find(shallow, "/Old stuff"), nullptr);
    EXPECT_EQ(find(shallow, "/Old stuff/note.txt"), nullptr);
    EXPECT_EQ(find(shallow, "/$OrphanFiles/note.txt"), nullptr);  // they have a parent; they are just not visited
    limits = {};
    limits.includeDeleted = false;
    EXPECT_TRUE(userRecords(m.scan(limits)).empty());
}

TEST(NtfsFilesystemTest, FileWhoseParentRecordWasReusedIsAnOrphan) {
    NtfsImageBuilder builder;
    const auto directory = builder.addDirectory(kRoot, "Trip");
    const auto photo = builder.addFileInClusters(directory.record, "beach.jpg", bytesOf(3 * 512, 16), {3600, 3601, 3602});
    builder.deleteEntry(photo);
    builder.deleteEntry(directory);
    // A new file takes the lowest free record: the directory's.
    const auto newcomer = builder.addFile(kRoot, "new.txt", bytesOf(50, 17));
    ASSERT_EQ(newcomer.record, directory.record);
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();

    EXPECT_NE(find(scan, "/new.txt"), nullptr);
    EXPECT_EQ(find(scan, "/Trip"), nullptr);
    const FileRecord* orphan = find(scan, "/$OrphanFiles/beach.jpg");
    ASSERT_NE(orphan, nullptr);
    EXPECT_EQ(orphan->entry.state, EntryState::Deleted);
    EXPECT_TRUE(orphan->entry.hasIssue(EntryIssue::ParentMissing));
    EXPECT_EQ(orphan->allocation.method, AllocationMethod::RunList);
    EXPECT_EQ(m.content(orphan->allocation), bytesOf(3 * 512, 16));

    ScanLimits limits;
    limits.includeDeleted = false;
    EXPECT_EQ(find(m.scan(limits), "/$OrphanFiles/beach.jpg"), nullptr);
}

TEST(NtfsFilesystemTest, OrphanedDirectoryKeepsItsSubtree) {
    NtfsImageBuilder builder;
    const auto outer = builder.addDirectory(kRoot, "outer");
    const auto inner = builder.addDirectory(outer.record, "inner");
    const auto file = builder.addFile(inner.record, "deep.txt", bytesOf(40, 18));
    builder.deleteEntry(file);
    builder.deleteEntry(inner);
    builder.deleteEntry(outer);
    (void)builder.addFile(kRoot, "reuses outer.txt", bytesOf(5, 19));
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();

    const FileRecord* orphan = find(scan, "/$OrphanFiles/inner");
    ASSERT_NE(orphan, nullptr);
    EXPECT_TRUE(orphan->entry.isDirectory);
    EXPECT_TRUE(orphan->entry.hasIssue(EntryIssue::ParentMissing));
    const FileRecord* deep = find(scan, "/$OrphanFiles/inner/deep.txt");
    ASSERT_NE(deep, nullptr);
    EXPECT_TRUE(deep->parentDeleted);
    EXPECT_FALSE(deep->entry.hasIssue(EntryIssue::ParentMissing));
    EXPECT_EQ(m.content(deep->allocation), bytesOf(40, 18));
}

TEST(NtfsFilesystemTest, SequenceNumbersWrapWithoutZero) {
    NtfsImageBuilder builder;
    const auto directory = builder.addDirectory(kRoot, "dir");
    builder.setSequence(directory.record, 0xFFFF);
    const auto file = builder.addFile(directory.record, "f.txt", bytesOf(20, 20));
    builder.deleteEntry(file);
    builder.deleteEntry(directory);
    EXPECT_EQ(builder.sequence(directory.record), 1u);  // 0xFFFF + 1, skipping zero
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/dir/f.txt");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->parentDeleted);
    EXPECT_FALSE(record->entry.hasIssue(EntryIssue::ParentMissing));
}

// ---------------------------------------------------------------------------
// Names.
// ---------------------------------------------------------------------------

TEST(NtfsFilesystemTest, UnicodeNames) {
    NtfsImageBuilder builder;
    const std::vector<std::string> names = {
        "\xC3\x89t\xC3\xA9 \xCE\xB1\xCE\xB2\xCE\xB3.jpeg",                  // Été αβγ
        "\xD0\xA4\xD0\xBE\xD1\x82\xD0\xBE.png",                              // Фото
        "\xE5\x86\x99\xE7\x9C\x9F.jpg",                                      // 写真
        "\xF0\x9F\x93\xB7 camera roll.heic",                                 // 📷 (a surrogate pair)
        std::string(255, 'n'),
    };
    for (std::size_t i = 0; i < names.size(); ++i) {
        (void)builder.addFile(kRoot, names[i], bytesOf(700 + i, i));
    }
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    for (std::size_t i = 0; i < names.size(); ++i) {
        const FileRecord* record = find(scan, "/" + names[i]);
        ASSERT_NE(record, nullptr) << names[i];
        EXPECT_EQ(record->entry.name, names[i]);
        EXPECT_TRUE(record->entry.issues.empty()) << issuesOf(*record);
        EXPECT_EQ(m.content(record->allocation), bytesOf(700 + i, i));
    }
}

TEST(NtfsFilesystemTest, ShortNames) {
    NtfsImageBuilder builder;
    const auto file = builder.addFile(kRoot, "Long file name.txt", bytesOf(10, 21));
    builder.setShortName(file, "LONGFI~1.TXT");
    (void)builder.addFile(kRoot, "IMG_0001.JPG", bytesOf(10, 22));  // an 8.3 name needs no alias
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    EXPECT_EQ(userRecords(scan).size(), 2u);  // the DOS name is not a second entry
    const FileRecord* longName = find(scan, "/Long file name.txt");
    ASSERT_NE(longName, nullptr);
    EXPECT_EQ(longName->entry.shortName, "LONGFI~1.TXT");
    const FileRecord* shortName = find(scan, "/IMG_0001.JPG");
    ASSERT_NE(shortName, nullptr);
    EXPECT_EQ(shortName->entry.shortName, "IMG_0001.JPG");
}

TEST(NtfsFilesystemTest, HardLinksShareOneRecord) {
    NtfsImageBuilder builder;
    const auto folder = builder.addDirectory(kRoot, "folder");
    const auto data = bytesOf(3000, 23);
    const auto file = builder.addFile(kRoot, "original.bin", data);
    builder.addHardLink(file, folder.record, "link.bin");
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* original = find(scan, "/original.bin");
    const FileRecord* link = find(scan, "/folder/link.bin");
    ASSERT_NE(original, nullptr);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(original->entry.metadataOffset, link->entry.metadataOffset);
    EXPECT_EQ(original->allocation.extents, link->allocation.extents);
    EXPECT_TRUE(link->allocation.issues.empty()) << issuesOf(*link);  // one record: not a cross-link
    EXPECT_EQ(scan.crossLinkedClusters, 0u);
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(scan.referencedClusters, usage->allocated);
}

// ---------------------------------------------------------------------------
// Directory listing and allocation lookup.
// ---------------------------------------------------------------------------

TEST(NtfsFilesystemTest, ReadDirectory) {
    NtfsImageBuilder builder;
    const auto dir = builder.addDirectory(kRoot, "dir");
    (void)builder.addFile(dir.record, "a.txt", bytesOf(10, 1));
    const auto b = builder.addFile(dir.record, "b.txt", bytesOf(10, 2));
    builder.deleteEntry(b);
    const Mounted m = mount(builder.build());

    const Result<DirectoryListing> root = m.fs->readDirectory(m.fs->rootDirectory());
    RECOVERY_ASSERT_OK(root);
    EXPECT_TRUE(root->issues.empty());
    const auto named = [](const DirectoryListing& listing, std::string_view name) {
        const auto it = std::find_if(listing.entries.begin(), listing.entries.end(),
                                     [&](const DirectoryEntry& e) { return e.name == name; });
        return it == listing.entries.end() ? nullptr : &*it;
    };
    EXPECT_EQ(root->entries.size(), 12u);  // 11 metadata files and "dir"; "." is omitted
    const DirectoryEntry* dirEntry = named(*root, "dir");
    ASSERT_NE(dirEntry, nullptr);
    EXPECT_EQ(dirEntry->metadataOffset, builder.recordOffset(dir.record));

    const Result<DirectoryListing> listing = m.fs->readDirectory(*dirEntry);
    RECOVERY_ASSERT_OK(listing);
    ASSERT_EQ(listing->entries.size(), 2u);
    EXPECT_EQ(named(*listing, "a.txt")->state, EntryState::Active);
    EXPECT_EQ(named(*listing, "b.txt")->state, EntryState::Deleted);

    RECOVERY_EXPECT_ERROR(m.fs->readDirectory(*named(*listing, "a.txt")), ErrorCode::InvalidInput);
    DirectoryEntry bogus = *dirEntry;
    bogus.metadataOffset += 3;
    RECOVERY_EXPECT_ERROR(m.fs->readDirectory(bogus), ErrorCode::InvalidInput);
    bogus.metadataOffset = builder.recordOffset(dir.record + 1);  // a.txt: a file's record
    RECOVERY_EXPECT_ERROR(m.fs->readDirectory(bogus), ErrorCode::InvalidFormat);

    NtfsOptions options;
    options.maxListingEntries = 1;
    const Mounted limited = mount(builder.build(), options);
    const Result<DirectoryListing> cut = limited.fs->readDirectory(*dirEntry);
    RECOVERY_ASSERT_OK(cut);
    EXPECT_EQ(cut->entries.size(), 1u);
    ASSERT_EQ(cut->issues.size(), 1u);
    EXPECT_EQ(cut->issues[0].kind, ScanIssueKind::EntryLimit);
}

TEST(NtfsFilesystemTest, ResolveAllocationMatchesTheScan) {
    NtfsImageBuilder builder;
    (void)builder.addFile(kRoot, "resident.txt", bytesOf(200, 1));
    (void)builder.addFile(kRoot, "contiguous.bin", bytesOf(4000, 2));
    (void)builder.addFileInClusters(kRoot, "fragmented.bin", bytesOf(3 * 512, 3), {6000, 6002, 6004});
    const auto gone = builder.addFileInClusters(kRoot, "gone.bin", bytesOf(2 * 512, 4), {6100, 6101});
    builder.deleteEntry(gone);
    (void)builder.addDirectory(kRoot, "big folder", 3);
    const Mounted m = mount(builder.build());
    for (const FileRecord& record : m.scan().records) {
        const Result<FileAllocation> resolved = m.fs->resolveAllocation(record.entry);
        RECOVERY_ASSERT_OK(resolved);
        EXPECT_EQ(resolved->method, record.allocation.method) << record.path;
        EXPECT_EQ(resolved->extents, record.allocation.extents) << record.path;
        EXPECT_EQ(resolved->issues, record.allocation.issues) << record.path;
        EXPECT_EQ(resolved->residentData, record.allocation.residentData) << record.path;
        EXPECT_EQ(resolved->clusterCount, record.allocation.clusterCount) << record.path;
    }
    DirectoryEntry bogus;
    bogus.metadataOffset = 12345;
    RECOVERY_EXPECT_ERROR(m.fs->resolveAllocation(bogus), ErrorCode::InvalidInput);
}

TEST(NtfsFilesystemTest, DirectoryWithIndexClusters) {
    NtfsImageBuilder builder;
    const auto big = builder.addDirectory(kRoot, "big folder", 4);
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/big folder");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->allocation.method, AllocationMethod::RunList);
    EXPECT_TRUE(record->allocation.issues.empty()) << issuesOf(*record);
    ASSERT_EQ(record->allocation.fragmentCount(), 1u);
    EXPECT_EQ(record->allocation.extents[0], (Extent{builder.clusterOffset(big.clusters.front()), 4 * 512}));
    EXPECT_EQ(scan.referencedClusters, builder.allocatedClusters());
}

TEST(NtfsFilesystemTest, ReadRecordExposesTheParsedRecord) {
    NtfsImageBuilder builder;
    const auto file = builder.addFile(kRoot, "x.bin", bytesOf(5000, 1));
    const Mounted m = mount(builder.build());
    const Result<MftRecord> record = m.fs->readRecord(file.record);
    RECOVERY_ASSERT_OK(record);
    EXPECT_EQ(record->names.at(0).name, u"x.bin");
    const Result<MftRecord> free = m.fs->readRecord(100);  // formatted, never used
    RECOVERY_ASSERT_OK(free);
    EXPECT_FALSE(free->inUse());
    EXPECT_TRUE(free->names.empty());
    RECOVERY_EXPECT_ERROR(m.fs->readRecord(128), ErrorCode::InvalidInput);
}

// ---------------------------------------------------------------------------
// Clusters.
// ---------------------------------------------------------------------------

TEST(NtfsFilesystemTest, ClusterStates) {
    NtfsImageBuilder builder;
    const auto file = builder.addFileInClusters(kRoot, "f.bin", bytesOf(512, 1), {5000});
    builder.markBadClusters(7000, 4);
    const Mounted m = mount(builder.build());
    EXPECT_EQ(m.fs->clusterState(ClusterNumber{0}).value(), ClusterState::Allocated);  // $Boot
    EXPECT_EQ(m.fs->clusterState(ClusterNumber{file.clusters[0]}).value(), ClusterState::Allocated);
    EXPECT_EQ(m.fs->clusterState(ClusterNumber{5001}).value(), ClusterState::Free);
    EXPECT_EQ(m.fs->clusterState(ClusterNumber{7003}).value(), ClusterState::Bad);
    EXPECT_EQ(m.fs->clusterState(ClusterNumber{7004}).value(), ClusterState::Free);
    EXPECT_EQ(m.fs->clusterState(ClusterNumber{8191}).value(), ClusterState::Free);
    RECOVERY_EXPECT_ERROR(m.fs->clusterState(ClusterNumber{8192}), ErrorCode::InvalidInput);

    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->bad, 4u);
    EXPECT_EQ(usage->allocated, builder.allocatedClusters() - 4);
    EXPECT_EQ(usage->free + usage->allocated + usage->bad, usage->total);
    // $BadClus:$Bad owns the bad clusters.
    EXPECT_EQ(m.scan().referencedClusters, builder.allocatedClusters());
}

TEST(NtfsFilesystemTest, SparseCompressedAndEncryptedDataAreFlagged) {
    NtfsImageBuilder builder;
    const auto data = bytesOf(6 * 512, 30);
    (void)builder.addFileWithRuns(kRoot, "sparse.vhd", data, {{5000, 2}, {std::nullopt, 3}, {5010, 1}}, 0x8000);
    (void)builder.addFileWithRuns(kRoot, "compressed.bin", bytesOf(2 * 512, 31), {{5100, 2}}, 0x0001);
    (void)builder.addFileWithRuns(kRoot, "secret.doc", bytesOf(2 * 512, 32), {{5200, 2}}, 0x4000);
    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();

    const FileRecord* sparse = find(scan, "/sparse.vhd");
    ASSERT_NE(sparse, nullptr);
    EXPECT_TRUE(sparse->allocation.hasIssue(AllocationIssue::SparseRuns));
    EXPECT_FALSE(sparse->allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    ASSERT_EQ(sparse->allocation.fragmentCount(), 2u);  // never merged across the hole
    EXPECT_EQ(sparse->allocation.extents[0], (Extent{builder.clusterOffset(5000), 2 * 512}));
    EXPECT_EQ(sparse->allocation.extents[1], (Extent{builder.clusterOffset(5010), 512}));
    EXPECT_EQ(sparse->allocation.clusterCount, 3u);
    EXPECT_FALSE(sparse->entry.contiguousData);
    EXPECT_TRUE(find(scan, "/compressed.bin")->allocation.hasIssue(AllocationIssue::CompressedData));
    EXPECT_TRUE(find(scan, "/secret.doc")->allocation.hasIssue(AllocationIssue::EncryptedData));
}

// ---------------------------------------------------------------------------
// Geometry and layout.
// ---------------------------------------------------------------------------

TEST(NtfsFilesystemTest, DifferentGeometries) {
    struct Geometry {
        std::uint16_t bytesPerSector;
        std::uint32_t sectorsPerCluster;
        std::uint32_t recordSize;
        std::uint64_t clusterCount;
    };
    for (const Geometry g : {Geometry{512, 1, 1024, 8192}, Geometry{512, 2, 1024, 4096}, Geometry{512, 8, 1024, 2048},
                             Geometry{4096, 1, 4096, 2048}, Geometry{2048, 2, 4096, 1024},
                             Geometry{512, 128, 1024, 256}, Geometry{4096, 16, 4096, 128}}) {
        SCOPED_TRACE(std::to_string(g.bytesPerSector) + " x " + std::to_string(g.sectorsPerCluster) + ", records " +
                     std::to_string(g.recordSize));
        test::NtfsBuilderOptions options;
        options.bytesPerSector = g.bytesPerSector;
        options.sectorsPerCluster = g.sectorsPerCluster;
        options.recordSize = g.recordSize;
        options.clusterCount = g.clusterCount;
        NtfsImageBuilder builder(options);
        const std::uint32_t cluster = builder.clusterSize();
        const auto resident = bytesOf(300, 1);
        const auto contiguous = bytesOf(3 * cluster + 1, 2);
        const auto fragmented = bytesOf(3 * cluster - 5, 3);
        const auto deleted = bytesOf(cluster + 9, 4);
        const std::uint64_t base = g.clusterCount - 20;
        (void)builder.addFile(kRoot, "resident.txt", resident);
        (void)builder.addFile(kRoot, "contiguous.bin", contiguous);
        (void)builder.addFileInClusters(kRoot, "fragmented.bin", fragmented, {base + 6, base, base + 3});
        const auto gone = builder.addFileInClusters(kRoot, "deleted.bin", deleted, {base + 10, base + 12});
        builder.deleteEntry(gone);
        const Mounted m = mount(builder.build(), {}, g.bytesPerSector);
        ASSERT_NE(m.fs, nullptr);
        EXPECT_TRUE(m.fs->info().warnings.empty()) << m.fs->info().warnings.front();
        EXPECT_EQ(m.fs->info().clusterSize, cluster);
        EXPECT_EQ(m.fs->bootSector().recordSize, g.recordSize);
        const FileScan scan = m.scan();
        EXPECT_TRUE(scan.issues.empty()) << issuesOf(scan);
        const std::map<std::string, const std::vector<std::byte>*> expected = {{"/resident.txt", &resident},
                                                                                {"/contiguous.bin", &contiguous},
                                                                                {"/fragmented.bin", &fragmented},
                                                                                {"/deleted.bin", &deleted}};
        for (const auto& [path, data] : expected) {
            const FileRecord* record = find(scan, path);
            ASSERT_NE(record, nullptr) << path;
            EXPECT_TRUE(record->allocation.issues.empty()) << path << ": " << issuesOf(*record);
            EXPECT_EQ(m.content(record->allocation), *data) << path;
        }
        const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
        RECOVERY_ASSERT_OK(usage);
        EXPECT_EQ(usage->allocated, builder.allocatedClusters());
        EXPECT_EQ(scan.referencedClusters, usage->allocated);
    }
}

TEST(NtfsFilesystemTest, FragmentedMftIsFollowed) {
    test::NtfsBuilderOptions options;
    options.fragmentedMft = true;
    NtfsImageBuilder builder(options);
    const auto data = bytesOf(2000, 40);
    const auto file = builder.addFile(kRoot, "in the second MFT run.bin", data);
    const Mounted m = mount(builder.build());
    EXPECT_EQ(m.fs->recordCount(), 128u);
    EXPECT_EQ(m.fs->recordOffset(file.record), builder.recordOffset(file.record));
    EXPECT_NE(m.fs->recordOffset(file.record), m.fs->recordOffset(0) + file.record * 1024);  // not contiguous
    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.issues.empty()) << issuesOf(scan);
    const FileRecord* record = find(scan, "/in the second MFT run.bin");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->entry.metadataOffset, builder.recordOffset(file.record));
    EXPECT_EQ(m.content(record->allocation), data);
    EXPECT_EQ(find(scan, "/$MFT")->allocation.fragmentCount(), 2u);
}

TEST(NtfsFilesystemTest, DamagedMftRecordFallsBackToTheMirror) {
    NtfsImageBuilder builder;
    (void)builder.addFile(kRoot, "survivor.txt", bytesOf(3000, 41));
    std::vector<std::byte> image = builder.build();
    storeLe16(image, builder.recordOffset(0) + 510, 0xBEEF);  // torn write of $MFT's own record
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_TRUE(m.fs->usedMftMirror());
    EXPECT_TRUE(hasWarning(m.fs->info(), "$MFTMirr"));
    const FileScan scan = m.scan();
    EXPECT_NE(find(scan, "/survivor.txt"), nullptr);
    // The damaged record itself is reported, not listed.
    EXPECT_EQ(find(scan, "/$MFT"), nullptr);
    EXPECT_TRUE(std::any_of(scan.issues.begin(), scan.issues.end(), [](const ScanIssue& issue) {
        return issue.kind == ScanIssueKind::RecordInvalid && issue.detail.starts_with("MFT record 0:");
    }));
}

TEST(NtfsFilesystemTest, DamagedBootSectorFallsBackToTheBackup) {
    NtfsImageBuilder builder;
    (void)builder.addFile(kRoot, "a.txt", bytesOf(10, 42));
    std::vector<std::byte> image = builder.build();
    std::fill_n(image.begin(), 512, std::byte{0});
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_TRUE(hasWarning(m.fs->info(), "backup boot sector"));
    EXPECT_NE(find(m.scan(), "/a.txt"), nullptr);
}

TEST(NtfsFilesystemTest, OpensInsideMbrPartition) {
    NtfsImageBuilder builder;
    const auto data = bytesOf(2500, 43);
    (void)builder.addFile(kRoot, "in partition.jpg", data);
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
    ASSERT_TRUE(table->partitions[0].candidates.ntfs);

    partition::PartitionSource part(device, table->partitions[0]);
    RECOVERY_ASSERT_OK(part.open());
    Result<std::unique_ptr<NtfsFilesystem>> fs = NtfsFilesystem::open(part);
    RECOVERY_ASSERT_OK(fs);
    const Result<FileScan> scan = fs.value()->scan({}, {});
    RECOVERY_ASSERT_OK(scan);
    const FileRecord* record = find(*scan, "/in partition.jpg");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(test::readExtents(part, record->allocation.extents), data);
}

TEST(NtfsFilesystemTest, SuperfloppyVolume) {
    NtfsImageBuilder builder;
    test::MemoryStorageSource device(builder.build(), 512);
    RECOVERY_ASSERT_OK(device.open());
    const Result<partition::PartitionTable> table = partition::readPartitionTable(device);
    RECOVERY_ASSERT_OK(table);
    EXPECT_EQ(table->scheme, partition::PartitionScheme::Unpartitioned);
    ASSERT_EQ(table->partitions.size(), 1u);
    EXPECT_TRUE(table->partitions[0].candidates.ntfs);
}

TEST(NtfsFilesystemTest, OtherFilesystemsAreUnsupported) {
    test::Fat32ImageBuilder fat;
    test::MemoryStorageSource source(fat.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    RECOVERY_EXPECT_ERROR(NtfsFilesystem::open(source), ErrorCode::UnsupportedFilesystem);
    test::MemoryStorageSource tiny(std::vector<std::byte>(100), 512);
    RECOVERY_ASSERT_OK(tiny.open());
    RECOVERY_EXPECT_ERROR(NtfsFilesystem::open(tiny), ErrorCode::UnsupportedFilesystem);
    test::MemoryStorageSource closed(std::vector<std::byte>(4096), 512);
    RECOVERY_EXPECT_ERROR(NtfsFilesystem::open(closed), ErrorCode::InvalidInput);
}

// ---------------------------------------------------------------------------
// Traversal limits.
// ---------------------------------------------------------------------------

TEST(NtfsFilesystemTest, ScanLimits) {
    NtfsImageBuilder builder;
    std::uint64_t parent = kRoot;
    for (int depth = 0; depth < 5; ++depth) {
        parent = builder.addDirectory(parent, "level" + std::to_string(depth)).record;
    }
    (void)builder.addFile(parent, "deep.txt", bytesOf(10, 1));
    const Mounted m = mount(builder.build());

    ScanLimits limits;
    limits.maxDepth = 2;
    const FileScan shallow = m.scan(limits);
    EXPECT_FALSE(shallow.complete);
    EXPECT_TRUE(std::any_of(shallow.issues.begin(), shallow.issues.end(),
                            [](const ScanIssue& i) { return i.kind == ScanIssueKind::DepthLimit; }));
    // The directory at the limit is listed, but not entered.
    EXPECT_NE(find(shallow, "/level0/level1/level2"), nullptr);
    EXPECT_EQ(find(shallow, "/level0/level1/level2/level3"), nullptr);

    limits = {};
    limits.maxEntries = 3;
    const FileScan few = m.scan(limits);
    EXPECT_FALSE(few.complete);
    EXPECT_EQ(few.records.size(), 3u);
    EXPECT_TRUE(std::any_of(few.issues.begin(), few.issues.end(),
                            [](const ScanIssue& i) { return i.kind == ScanIssueKind::EntryLimit; }));

    EXPECT_NE(find(m.scan(), "/level0/level1/level2/level3/level4/deep.txt"), nullptr);
}

TEST(NtfsFilesystemTest, CancellationStopsWork) {
    NtfsImageBuilder builder;
    const Mounted m = mount(builder.build());
    CancellationSource cancel;
    cancel.requestCancellation();
    RECOVERY_EXPECT_ERROR(m.fs->scan({}, cancel.token()), ErrorCode::Cancelled);
    RECOVERY_EXPECT_ERROR(m.fs->analyzeClusters(cancel.token()), ErrorCode::Cancelled);
    RECOVERY_EXPECT_OK(m.fs->scan({}, {}));  // a cancelled scan leaves nothing half-built
}

TEST(NtfsFilesystemTest, LogsOpenAndScan) {
    NtfsImageBuilder builder;
    auto sink = std::make_shared<diagnostics::MemorySink>();
    diagnostics::Logger logger(diagnostics::LogLevel::Debug);
    logger.addSink(sink);
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    Result<std::unique_ptr<NtfsFilesystem>> fs = NtfsFilesystem::open(source, {}, &logger);
    RECOVERY_ASSERT_OK(fs);
    RECOVERY_ASSERT_OK(fs.value()->scan({}, {}));
    const auto records = sink->records();
    for (const std::string_view message : {"NTFS volume opened", "MFT read", "NTFS scan finished"}) {
        EXPECT_TRUE(std::any_of(records.begin(), records.end(), [&](const auto& r) { return r.message == message; }))
            << message;
    }
}

}  // namespace
}  // namespace recovery::filesystem::ntfs
