// What deletion takes from FAT32 entries besides their chains (after P20):
// the top 16 bits of a deleted entry's start cluster, which Windows clears
// (L7), placed again by a folder's "." and ".." entries, by the files around
// an entry, and by what starts at each place it may start; and a deleted
// folder's clusters after the first, whose chain is gone (L8), found where
// writers put them when the folder grew.

#include "filesystem/fat32/fat32_filesystem.hpp"

#include "recovery/byte_order.hpp"
#include "support/fat32_builder.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

namespace recovery::filesystem::fat32 {
namespace {

using test::Fat32ImageBuilder;
using Bytes = std::vector<std::byte>;

// What the cleared top 16 bits of a cluster number are worth.
constexpr std::uint32_t kStep = 0x10000;

Bytes bytesOf(std::size_t size, std::uint64_t seed) {
    return test::makePattern(size, seed);
}

// Data that starts like a JPEG image (what jpegCheck() looks for).
Bytes jpegLike(std::size_t size, std::uint64_t seed) {
    Bytes data = bytesOf(size, seed);
    data[0] = std::byte{0xFF};
    data[1] = std::byte{0xD8};
    data[2] = std::byte{0xFF};
    return data;
}

// Stands for the scan's formats: names ending in ".jpg" start with FF D8 FF.
std::optional<bool> jpegCheck(std::string_view name, std::span<const std::byte> head) {
    if (!name.ends_with(".jpg")) {
        return std::nullopt;
    }
    return head.size() >= 3 && head[0] == std::byte{0xFF} && head[1] == std::byte{0xD8} && head[2] == std::byte{0xFF};
}

// 70,000 clusters of 512 bytes: starts beyond cluster 65,535 lose their top
// bits when Windows deletes them. Entries are written before their data, as
// Windows and cameras do.
test::Fat32BuilderOptions bigVolume() {
    test::Fat32BuilderOptions options;
    options.clusterCount = 70'000;
    options.entriesBeforeData = true;
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

Mounted mount(std::vector<std::byte> image, StartCheck startsFile = {}) {
    Mounted m;
    m.source = std::make_unique<test::MemoryStorageSource>(std::move(image), 512);
    EXPECT_TRUE(m.source->open().ok());
    Fat32Options options;
    options.startsFile = std::move(startsFile);
    Result<std::unique_ptr<Fat32Filesystem>> fs = Fat32Filesystem::open(*m.source, options);
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

// The record of a file the test deleted: found, starting at `start`, with its data.
void expectDeletedFile(const Mounted& m, const FileScan& scan, std::string_view path, std::uint32_t start,
                       const Bytes& data, bool inferred) {
    const FileRecord* record = find(scan, path);
    ASSERT_NE(record, nullptr) << path;
    EXPECT_EQ(record->entry.state, EntryState::Deleted) << path;
    EXPECT_EQ(record->entry.firstCluster.value(), start) << path;
    EXPECT_EQ(record->entry.hasIssue(EntryIssue::StartClusterInferred), inferred) << path;
    EXPECT_EQ(test::readExtents(*m.source, record->allocation.extents), data) << path;
}

// ---------------------------------------------------------------------------
// L7: starts whose top 16 bits were cleared
// ---------------------------------------------------------------------------

TEST(Fat32DeletedStartTest, AFileStartsWhereTheFileBeforeItEnds) {
    Fat32ImageBuilder builder(bigVolume());
    builder.skipClusters(kStep);
    (void)builder.addFile(builder.root(), "before.bin", bytesOf(3000, 1));
    const Bytes data = bytesOf(5000, 2);
    const auto gone = builder.addFile(builder.root(), "deleted.bin", data);
    (void)builder.addFile(builder.root(), "after.bin", bytesOf(2000, 3));
    builder.deleteEntry(gone, true, true);
    ASSERT_GT(gone.clusters.front(), 0xFFFFu);
    EXPECT_EQ(loadLe16(builder.shortEntry(gone), 20), 0u) << "the top bits are cleared";

    const Mounted m = mount(builder.build());
    expectDeletedFile(m, m.scan(), "/deleted.bin", gone.clusters.front(), data, true);
}

TEST(Fat32DeletedStartTest, AFileEndsWhereTheFileAfterItStarts) {
    Fat32ImageBuilder builder(bigVolume());
    builder.skipClusters(kStep);
    (void)builder.addFile(builder.root(), "before.bin", bytesOf(3000, 1));
    builder.skipClusters(40);  // not after the file before it
    const Bytes data = bytesOf(5000, 2);
    const auto gone = builder.addFile(builder.root(), "deleted.bin", data);
    (void)builder.addFile(builder.root(), "after.bin", bytesOf(2000, 3));
    builder.deleteEntry(gone, true, true);

    const Mounted m = mount(builder.build());
    expectDeletedFile(m, m.scan(), "/deleted.bin", gone.clusters.front(), data, true);
}

TEST(Fat32DeletedStartTest, DeletedFilesOfAFolderFollowTheFolderOneAfterTheOther) {
    Fat32ImageBuilder builder(bigVolume());
    builder.skipClusters(kStep);
    const auto folder = builder.addDirectory(builder.root(), "DCIM");
    std::vector<Fat32ImageBuilder::Entry> photos;
    std::vector<Bytes> data;
    for (int i = 0; i < 5; ++i) {
        data.push_back(bytesOf(700 + 300 * static_cast<std::size_t>(i), 10 + static_cast<std::uint64_t>(i)));
        photos.push_back(builder.addFile(folder.clusters.front(), "IMG_000" + std::to_string(i) + ".JPG", data.back()));
    }
    for (const auto& photo : photos) {
        builder.deleteEntry(photo, true, true);
    }

    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    for (std::size_t i = 0; i < photos.size(); ++i) {
        expectDeletedFile(m, scan, "/DCIM/_MG_000" + std::to_string(i) + ".JPG", photos[i].clusters.front(), data[i],
                          true);
    }
}

TEST(Fat32DeletedStartTest, ADeletedFolderIsPlacedByItsDotEntries) {
    Fat32ImageBuilder builder(bigVolume());
    builder.skipClusters(kStep);
    (void)builder.addFile(builder.root(), "KEEP.TXT", bytesOf(100, 1));
    builder.skipClusters(25);
    const auto trip = builder.addDirectory(builder.root(), "Old trip");
    const Bytes beach = bytesOf(2600, 2);
    const auto e1 = builder.addFile(trip.clusters.front(), "Beach day.jpg", beach);
    const Bytes sea = bytesOf(1300, 3);
    const auto e2 = builder.addFile(trip.clusters.front(), "Sea.png", sea);
    // Windows deletes a folder's files before the folder.
    for (const auto& entry : {e1, e2, trip}) {
        builder.deleteEntry(entry, true, true);
    }

    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* folder = find(scan, "/Old trip");
    ASSERT_NE(folder, nullptr);
    EXPECT_TRUE(folder->entry.isDirectory);
    EXPECT_EQ(folder->entry.firstCluster.value(), trip.clusters.front());
    EXPECT_TRUE(folder->entry.hasIssue(EntryIssue::StartClusterInferred));
    EXPECT_TRUE(scan.issues.empty()) << scan.issues.front().detail;
    expectDeletedFile(m, scan, "/Old trip/Beach day.jpg", e1.clusters.front(), beach, true);
    expectDeletedFile(m, scan, "/Old trip/Sea.png", e2.clusters.front(), sea, true);
}

TEST(Fat32DeletedStartTest, WhatStartsThereDecidesWhenNoNeighbourTells) {
    Fat32ImageBuilder builder(bigVolume());
    builder.skipClusters(kStep);
    (void)builder.addFile(builder.root(), "first.bin", bytesOf(3000, 1));
    builder.skipClusters(100);  // neither after the file before it nor before one after it
    const Bytes data = jpegLike(4000, 2);
    const auto photo = builder.addFile(builder.root(), "photo.jpg", data);
    builder.deleteEntry(photo, true, true);
    const Bytes image = builder.build();

    // The one place 65,536 clusters apart where a JPEG starts.
    const Mounted checked = mount(image, jpegCheck);
    expectDeletedFile(checked, checked.scan(), "/photo.jpg", photo.clusters.front(), data, true);

    // Without knowing what starts a file of that name, the start stays as recorded.
    const Mounted unchecked = mount(image);
    const FileScan scan = unchecked.scan();
    const FileRecord* record = find(scan, "/photo.jpg");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->entry.firstCluster.value(), photo.clusters.front() - kStep);
    EXPECT_FALSE(record->entry.hasIssue(EntryIssue::StartClusterInferred));
}

TEST(Fat32DeletedStartTest, ARunOfFilesIsPlacedTogether) {
    Fat32ImageBuilder builder(bigVolume());
    builder.skipClusters(kStep + 200);
    std::vector<Fat32ImageBuilder::Entry> photos;
    std::vector<Bytes> data;
    for (int i = 0; i < 6; ++i) {
        data.push_back(jpegLike(900 + 200 * static_cast<std::size_t>(i), 20 + static_cast<std::uint64_t>(i)));
        photos.push_back(builder.addFile(builder.root(), "holiday " + std::to_string(i) + ".jpg", data.back()));
    }
    for (const auto& photo : photos) {
        builder.deleteEntry(photo, true, true);
    }
    // A JPEG starts at the place the first one was recorded at too, but the
    // others' places there start nothing: the run is placed where all do.
    const std::uint32_t recorded = photos.front().clusters.front() - kStep;
    const Bytes decoy = jpegLike(512, 99);
    std::memcpy(builder.raw().data() + builder.clusterOffset(recorded), decoy.data(), decoy.size());

    const Mounted m = mount(builder.build(), jpegCheck);
    const FileScan scan = m.scan();
    for (std::size_t i = 0; i < photos.size(); ++i) {
        expectDeletedFile(m, scan, "/holiday " + std::to_string(i) + ".jpg", photos[i].clusters.front(), data[i],
                          true);
    }
}

TEST(Fat32DeletedStartTest, AStartTheEvidenceDoesNotSingleOutStaysAsRecorded) {
    Fat32ImageBuilder builder(bigVolume());
    builder.skipClusters(kStep);
    (void)builder.addFile(builder.root(), "first.bin", bytesOf(3000, 1));
    builder.skipClusters(100);
    const Bytes data = jpegLike(4000, 2);
    const auto photo = builder.addFile(builder.root(), "photo.jpg", data);
    builder.deleteEntry(photo, true, true);
    // A JPEG starts at the place recorded as well: the start is kept there.
    const std::uint32_t recorded = photo.clusters.front() - kStep;
    const Bytes other = jpegLike(512, 3);
    std::memcpy(builder.raw().data() + builder.clusterOffset(recorded), other.data(), other.size());

    const Mounted m = mount(builder.build(), jpegCheck);
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/photo.jpg");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->entry.firstCluster.value(), recorded);
    EXPECT_FALSE(record->entry.hasIssue(EntryIssue::StartClusterInferred));
}

TEST(Fat32DeletedStartTest, AStartThatKeptItsTopBitsIsTrusted) {
    Fat32ImageBuilder builder(bigVolume());
    builder.skipClusters(kStep);
    const Bytes data = bytesOf(5000, 2);
    const auto gone = builder.addFile(builder.root(), "deleted.bin", data);
    builder.deleteEntry(gone);  // as other systems delete: the start is whole

    const Mounted m = mount(builder.build());
    expectDeletedFile(m, m.scan(), "/deleted.bin", gone.clusters.front(), data, false);
}

TEST(Fat32DeletedStartTest, AFileThatStartsBelowTheFirstStepStaysThere) {
    Fat32ImageBuilder builder(bigVolume());
    (void)builder.addFile(builder.root(), "before.bin", bytesOf(3000, 1));
    const Bytes data = bytesOf(5000, 2);
    const auto gone = builder.addFile(builder.root(), "deleted.bin", data);
    (void)builder.addFile(builder.root(), "after.bin", bytesOf(2000, 3));
    builder.deleteEntry(gone, true, true);
    ASSERT_LT(gone.clusters.front(), kStep);

    const Mounted m = mount(builder.build());
    expectDeletedFile(m, m.scan(), "/deleted.bin", gone.clusters.front(), data, false);
}

TEST(Fat32DeletedStartTest, ClusterOneIsWhatIsLeftOfAStartPastTheFirstStep) {
    Fat32ImageBuilder builder(bigVolume());
    std::vector<std::uint32_t> before;
    for (std::uint32_t c = kStep - 6; c <= kStep; ++c) {
        before.push_back(c);
    }
    (void)builder.addFileInClusters(builder.root(), "before.bin", bytesOf(7 * 512, 1), before);
    const Bytes data = bytesOf(4 * 512, 2);
    const auto gone = builder.addFileInClusters(builder.root(), "deleted.bin", data,
                                                {kStep + 1, kStep + 2, kStep + 3, kStep + 4});
    builder.deleteEntry(gone, true, true);  // its start now reads as cluster 1

    const Mounted m = mount(builder.build());
    expectDeletedFile(m, m.scan(), "/deleted.bin", kStep + 1, data, true);
}

TEST(Fat32DeletedStartTest, SmallVolumesHaveNoTopBitsToLose) {
    Fat32ImageBuilder builder;  // 2,048 clusters
    const Bytes data = bytesOf(3000, 2);
    const auto gone = builder.addFile(builder.root(), "deleted.bin", data);
    builder.deleteEntry(gone, true, true);
    const Mounted m = mount(builder.build(), jpegCheck);
    EXPECT_FALSE(m.fs->bootSector().deletedStartsAmbiguous());
    expectDeletedFile(m, m.scan(), "/deleted.bin", gone.clusters.front(), data, false);
}

// ---------------------------------------------------------------------------
// L8: a deleted folder's clusters after the first
// ---------------------------------------------------------------------------

// A folder of `count` files with long names (three slots each), written the
// way Windows writes them: 512-byte clusters hold 16 slots, so the folder grows
// every few files, each time after the data of the file written before.
struct GrownFolder {
    Fat32ImageBuilder::Entry folder;
    std::vector<Fat32ImageBuilder::Entry> files;
    std::vector<Bytes> data;
    std::vector<std::string> paths;
};

GrownFolder grownFolder(Fat32ImageBuilder& builder, std::string_view name, int count, std::uint64_t seed) {
    GrownFolder grown;
    grown.folder = builder.addDirectory(builder.root(), name);
    for (int i = 0; i < count; ++i) {
        const std::string file = "photo number " + std::to_string(i) + ".jpg";
        grown.data.push_back(bytesOf(700 + 37 * static_cast<std::size_t>(i), seed + static_cast<std::uint64_t>(i)));
        grown.files.push_back(builder.addFile(grown.folder.clusters.front(), file, grown.data.back()));
        grown.paths.push_back("/" + std::string(name) + "/" + file);
    }
    return grown;
}

TEST(Fat32DeletedFolderTest, ADeletedFolderGoesOnAfterTheDataOfItsLastFile) {
    test::Fat32BuilderOptions options;
    options.entriesBeforeData = true;
    Fat32ImageBuilder builder(options);
    const GrownFolder grown = grownFolder(builder, "Many photos", 12, 100);
    const std::vector<std::uint32_t>& clusters = builder.directoryClusters(grown.folder.clusters.front());
    ASSERT_GE(clusters.size(), 3u);
    EXPECT_NE(clusters[1], clusters[0] + 1) << "the folder grew after the data written meanwhile";
    for (const auto& file : grown.files) {
        builder.deleteEntry(file);
    }
    builder.deleteEntry(grown.folder);

    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.issues.empty()) << scan.issues.front().detail;
    for (std::size_t i = 0; i < grown.files.size(); ++i) {
        expectDeletedFile(m, scan, grown.paths[i], grown.files[i].clusters.front(), grown.data[i], false);
    }
}

TEST(Fat32DeletedFolderTest, ADeletedFolderGoesOnRightAfterItsPreviousCluster) {
    test::Fat32BuilderOptions options;
    options.entriesBeforeData = true;
    Fat32ImageBuilder builder(options);
    const auto folder = builder.addDirectory(builder.root(), "Notes");
    std::vector<Fat32ImageBuilder::Entry> entries;
    // Empty files fill the first cluster: the folder grows into the next one.
    for (int i = 0; i < 5; ++i) {
        entries.push_back(builder.addFile(folder.clusters.front(), "empty note " + std::to_string(i) + ".txt", {}));
    }
    const Bytes data = bytesOf(1500, 7);
    const auto note = builder.addFile(folder.clusters.front(), "kept note.txt", data);
    const std::vector<std::uint32_t>& clusters = builder.directoryClusters(folder.clusters.front());
    ASSERT_GE(clusters.size(), 2u);
    ASSERT_EQ(clusters[1], clusters[0] + 1);
    builder.deleteEntry(note);
    for (const auto& entry : entries) {
        builder.deleteEntry(entry);
    }
    builder.deleteEntry(folder);

    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    expectDeletedFile(m, scan, "/Notes/kept note.txt", note.clusters.front(), data, false);
    EXPECT_NE(find(scan, "/Notes/empty note 4.txt"), nullptr);
}

TEST(Fat32DeletedFolderTest, AFolderWhoseNextClusterWasReusedEndsWhereItCan) {
    test::Fat32BuilderOptions options;
    options.entriesBeforeData = true;
    Fat32ImageBuilder builder(options);
    const GrownFolder grown = grownFolder(builder, "Many photos", 12, 100);
    const std::vector<std::uint32_t> clusters = builder.directoryClusters(grown.folder.clusters.front());
    for (const auto& file : grown.files) {
        builder.deleteEntry(file);
    }
    builder.deleteEntry(grown.folder);
    // A new file took the folder's second cluster.
    (void)builder.addFileInClusters(builder.root(), "NEW.BIN", bytesOf(512, 9), {clusters[1]});

    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    // The files listed in the first cluster are found; nothing else is made up.
    EXPECT_NE(find(scan, grown.paths[0]), nullptr);
    std::size_t found = 0;
    for (const FileRecord& record : scan.records) {
        found += record.path.starts_with("/Many photos/") ? 1 : 0;
        EXPECT_TRUE(record.path == "/Many photos" || record.path == "/NEW.BIN" ||
                    std::find(grown.paths.begin(), grown.paths.end(), record.path) != grown.paths.end())
            << record.path;
    }
    EXPECT_LT(found, grown.files.size());
}

TEST(Fat32DeletedFolderTest, AFolderWhoseClustersLostTheirTopBitsIsReadWhole) {
    Fat32ImageBuilder builder(bigVolume());
    builder.skipClusters(kStep);
    const GrownFolder grown = grownFolder(builder, "Many photos", 12, 200);
    ASSERT_GE(builder.directoryClusters(grown.folder.clusters.front()).size(), 3u);
    for (const auto& file : grown.files) {
        builder.deleteEntry(file, true, true);
    }
    builder.deleteEntry(grown.folder, true, true);

    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    EXPECT_TRUE(scan.issues.empty()) << scan.issues.front().detail;
    for (std::size_t i = 0; i < grown.files.size(); ++i) {
        expectDeletedFile(m, scan, grown.paths[i], grown.files[i].clusters.front(), grown.data[i], true);
    }
}

}  // namespace
}  // namespace recovery::filesystem::fat32
