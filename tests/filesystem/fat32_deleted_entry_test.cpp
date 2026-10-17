// What deletion takes from FAT32 entries besides their chains (after P20):
// the top 16 bits of a deleted entry's start cluster, which Windows clears
// (L7), placed again by a folder's "." and ".." entries, by the files around
// an entry, by what starts at each place it may start and where it ends, and
// by what the place recorded and the other places hold (L242); and a deleted
// folder's clusters after the first, whose chain is gone (L8), found where
// writers put them when the folder grew, or over the volume after a scan,
// in their folder or under "/$OrphanFiles" (L243); and the folders nothing
// leads to, as a format leaves them, found in the free space (L249).

#include "filesystem/fat32/fat32_filesystem.hpp"

#include "recovery/byte_order.hpp"
#include "support/fat32_builder.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <string>

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

// Data that starts and ends like a JPEG image (what jpegEnds() looks for).
Bytes jpegWhole(std::size_t size, std::uint64_t seed) {
    Bytes data = jpegLike(size, seed);
    data[size - 2] = std::byte{0xFF};
    data[size - 1] = std::byte{0xD9};
    return data;
}

// Text of `size` bytes: lines of words.
Bytes textOf(std::size_t size, std::uint64_t seed) {
    const Bytes noise = bytesOf(size, seed);
    Bytes text(size);
    for (std::size_t i = 0; i < size; ++i) {
        const auto n = static_cast<std::uint8_t>(noise[i]);
        text[i] = i % 64 == 63 ? std::byte{'\n'} : static_cast<std::byte>(n % 7 == 0 ? ' ' : 'a' + n % 26);
    }
    return text;
}

std::vector<std::uint32_t> run(std::uint32_t first, std::uint32_t count) {
    std::vector<std::uint32_t> clusters;
    for (std::uint32_t i = 0; i < count; ++i) {
        clusters.push_back(first + i);
    }
    return clusters;
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

// `checkEnds`: stands for the scan's formats as to where files end too: a
// file named ".jpg" ends with FF D9 at its size. `searchFolders`: as a deep
// scan, the free space is searched for folders no listing reaches (L249).
Mounted mount(std::vector<std::byte> image, StartCheck startsFile = {}, bool checkEnds = false,
              bool searchFolders = false) {
    Mounted m;
    m.source = std::make_unique<test::MemoryStorageSource>(std::move(image), 512);
    EXPECT_TRUE(m.source->open().ok());
    Fat32Options options;
    options.startsFile = std::move(startsFile);
    options.searchFreeSpaceForFolders = searchFolders;
    if (checkEnds) {
        options.endsFile = [source = m.source.get()](std::string_view name, std::uint64_t offset, std::uint64_t size) {
            std::array<std::byte, 2> end{};
            return name.ends_with(".jpg") && size >= 2 && source->readExact(ByteOffset{offset + size - 2}, end).ok() &&
                   end[0] == std::byte{0xFF} && end[1] == std::byte{0xD9};
        };
    }
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
// L242: more evidence for a start no neighbour places
// ---------------------------------------------------------------------------

TEST(Fat32DeletedStartTest, AFileWhoseRecordedPlaceHoldsOtherDataGoesWhereItAloneCanBe) {
    Fat32ImageBuilder builder(bigVolume());
    // A newer file covers the place its start records, next to nothing of it.
    (void)builder.addFileInClusters(builder.root(), "newer.bin", bytesOf(20 * 512, 1), run(290, 20));
    const std::uint32_t start = kStep + 300;
    const Bytes data = bytesOf(1800, 2);  // a type no format knows
    const auto gone = builder.addFileInClusters(builder.root(), "settings.dat", data, run(start, 4));
    builder.deleteEntry(gone, true, true);

    const Mounted m = mount(builder.build());
    expectDeletedFile(m, m.scan(), "/settings.dat", start, data, true);
}

TEST(Fat32DeletedStartTest, ATextFileGoesWhereTextIsWrittenNotWhereOtherDataIs) {
    Fat32ImageBuilder builder(bigVolume());
    const std::uint32_t start = kStep + 400;
    const Bytes notes = textOf(3000, 3);
    const auto gone = builder.addFileInClusters(builder.root(), "notes.txt", notes, run(start, 6));
    builder.deleteEntry(gone, true, true);
    // Where its start records: free, but no text (a photo deleted long ago).
    const Bytes photo = jpegLike(6 * 512, 4);
    std::memcpy(builder.raw().data() + builder.clusterOffset(400), photo.data(), photo.size());

    const Mounted m = mount(builder.build());
    expectDeletedFile(m, m.scan(), "/notes.txt", start, notes, true);
}

TEST(Fat32DeletedStartTest, AFileThatMayBeWhereItsStartRecordsStaysThere) {
    Fat32ImageBuilder builder(bigVolume());
    const Bytes notes = textOf(3000, 5);
    const auto gone = builder.addFileInClusters(builder.root(), "notes.txt", notes, run(500, 6));
    builder.deleteEntry(gone, true, true);
    // Free text 65,536 clusters on as well: it could be there too.
    const Bytes other = textOf(3000, 6);
    std::memcpy(builder.raw().data() + builder.clusterOffset(kStep + 500), other.data(), other.size());

    const Mounted m = mount(builder.build());
    expectDeletedFile(m, m.scan(), "/notes.txt", 500, notes, false);
}

TEST(Fat32DeletedStartTest, AFileMostlyStillWhereItsStartRecordsStaysThere) {
    Fat32ImageBuilder builder(bigVolume());
    const Bytes log = textOf(8 * 512, 7);
    const auto gone = builder.addFileInClusters(builder.root(), "app.log", log, run(600, 8));
    builder.deleteEntry(gone, true, true);
    // A newer file took its first cluster only; text 65,536 clusters on as well.
    (void)builder.addFileInClusters(builder.root(), "NEW.BIN", bytesOf(512, 8), {600});
    const Bytes other = textOf(8 * 512, 9);
    std::memcpy(builder.raw().data() + builder.clusterOffset(kStep + 600), other.data(), other.size());

    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    const FileRecord* record = find(scan, "/app.log");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->entry.firstCluster.value(), 600u) << "the rest of it is still there";
    EXPECT_FALSE(record->entry.hasIssue(EntryIssue::StartClusterInferred));
}

TEST(Fat32DeletedStartTest, OfPlacesWhereItCanBeTheOneBetweenTheFilesAroundItIsTaken) {
    test::Fat32BuilderOptions options = bigVolume();
    options.clusterCount = 140'000;  // a start's low bits name three places
    Fat32ImageBuilder builder(options);
    // Written one after the other, with gaps: a file, the deleted one, a file.
    (void)builder.addFileInClusters(builder.root(), "before.bin", bytesOf(10 * 512, 1), run(kStep + 600, 10));
    const Bytes data = bytesOf(4 * 512, 2);
    const auto gone = builder.addFileInClusters(builder.root(), "between.dat", data, run(kStep + 700, 4));
    (void)builder.addFileInClusters(builder.root(), "after.bin", bytesOf(10 * 512, 3), run(kStep + 800, 10));
    builder.deleteEntry(gone, true, true);
    // A newer file holds the place recorded; old data lies 131,072 clusters on.
    (void)builder.addFileInClusters(builder.root(), "NEWER.BIN", bytesOf(4 * 512, 4), run(700, 4));
    const Bytes old = bytesOf(4 * 512, 5);
    std::memcpy(builder.raw().data() + builder.clusterOffset(2 * kStep + 700), old.data(), old.size());

    const Mounted m = mount(builder.build());
    expectDeletedFile(m, m.scan(), "/between.dat", kStep + 700, data, true);
}

TEST(Fat32DeletedStartTest, OfPlacesThatStartAlikeTheOneWhereTheFileEndsAtItsSizeIsTaken) {
    Fat32ImageBuilder builder(bigVolume());
    builder.skipClusters(kStep);
    (void)builder.addFile(builder.root(), "first.bin", bytesOf(3000, 1));
    builder.skipClusters(100);
    const Bytes data = jpegWhole(4000, 2);
    const auto photo = builder.addFile(builder.root(), "photo.jpg", data);
    builder.deleteEntry(photo, true, true);
    // A JPEG starts at the place recorded as well (a larger one: it ends further on).
    const std::uint32_t recorded = photo.clusters.front() - kStep;
    const Bytes other = jpegLike(9 * 512, 3);
    ASSERT_FALSE(other[3998] == std::byte{0xFF} && other[3999] == std::byte{0xD9});
    std::memcpy(builder.raw().data() + builder.clusterOffset(recorded), other.data(), other.size());
    const Bytes image = builder.build();

    // Its end tells the places apart.
    const Mounted checked = mount(image, jpegCheck, true);
    expectDeletedFile(checked, checked.scan(), "/photo.jpg", photo.clusters.front(), data, true);
    // Without it, the start stays as recorded (AStartTheEvidenceDoesNotSingleOutStaysAsRecorded).
    const Mounted unchecked = mount(image, jpegCheck);
    const FileScan scan = unchecked.scan();
    const FileRecord* record = find(scan, "/photo.jpg");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->entry.firstCluster.value(), recorded);
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

// ---------------------------------------------------------------------------
// L243: a deleted folder's clusters the search after its last file misses
// ---------------------------------------------------------------------------

bool listsOrphans(const FileScan& scan) {
    return std::any_of(scan.records.begin(), scan.records.end(),
                       [](const FileRecord& record) { return record.path.starts_with("/$OrphanFiles"); });
}

// Files written into a folder one by one; between the fourth and the fifth,
// which no longer fit into its first cluster, another program wrote a file
// elsewhere, and the folder grew after that file's data. The fifth file's
// long name begins in the first cluster's last two slots.
struct FolderGrownAfterAnother {
    Fat32ImageBuilder builder;
    Fat32ImageBuilder::Entry folder;
    std::vector<Fat32ImageBuilder::Entry> files;
    std::vector<Bytes> data;
    std::vector<std::string> paths;

    FolderGrownAfterAnother() : builder(test::Fat32BuilderOptions{.entriesBeforeData = true}) {
        folder = builder.addDirectory(builder.root(), "Trip");
        for (int i = 0; i < 8; ++i) {
            if (i == 4) {
                (void)builder.addFile(builder.root(), "log.bin", bytesOf(3000, 50));
            }
            const std::string name = "photo number " + std::to_string(i) + ".jpg";
            data.push_back(bytesOf(700 + 37 * static_cast<std::size_t>(i), 60 + static_cast<std::uint64_t>(i)));
            files.push_back(builder.addFile(folder.clusters.front(), name, data.back()));
            paths.push_back("/Trip/" + name);
        }
        for (const auto& file : files) {
            builder.deleteEntry(file);
        }
        builder.deleteEntry(folder);
    }
};

TEST(Fat32DeletedFolderTest, AFolderThatGrewAfterAFileWrittenMeanwhileGoesOnThere) {
    FolderGrownAfterAnother grown;
    const std::vector<std::uint32_t>& clusters = grown.builder.directoryClusters(grown.folder.clusters.front());
    ASSERT_GE(clusters.size(), 2u);
    const Mounted m = mount(grown.builder.build());
    const FileScan scan = m.scan();
    for (std::size_t i = 0; i < grown.files.size(); ++i) {
        expectDeletedFile(m, scan, grown.paths[i], grown.files[i].clusters.front(), grown.data[i], false);
    }
    EXPECT_FALSE(listsOrphans(scan));
}

TEST(Fat32DeletedFolderTest, APartOfAFolderNoFolderLeadsToIsListedUnderOrphanFiles) {
    test::Fat32BuilderOptions options;
    options.entriesBeforeData = true;
    Fat32ImageBuilder builder(options);
    const auto folder = builder.addDirectory(builder.root(), "Camera");
    std::vector<Fat32ImageBuilder::Entry> files;
    std::vector<Bytes> data;
    // Fourteen 8.3 names fill its first cluster after "." and "..".
    for (int i = 0; i < 18; ++i) {
        if (i == 14) {
            (void)builder.addFile(builder.root(), "LOG.BIN", bytesOf(3000, 70));
        }
        data.push_back(bytesOf(600, 80 + static_cast<std::uint64_t>(i)));
        files.push_back(builder.addFile(folder.clusters.front(), "IMG_00" + std::to_string(10 + i) + ".JPG", data.back()));
    }
    for (const auto& file : files) {
        builder.deleteEntry(file);
    }
    builder.deleteEntry(folder);
    // A newer file took the folder's first cluster: the folder cannot be listed.
    (void)builder.addFileInClusters(builder.root(), "NEWER.BIN", bytesOf(512, 71), {folder.clusters.front()});

    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    for (std::size_t i = 14; i < files.size(); ++i) {
        const std::string path = "/$OrphanFiles/_MG_00" + std::to_string(10 + i) + ".JPG";
        expectDeletedFile(m, scan, path, files[i].clusters.front(), data[i], false);
        const FileRecord* record = find(scan, path);
        ASSERT_NE(record, nullptr);
        EXPECT_TRUE(record->entry.hasIssue(EntryIssue::ParentMissing));
        EXPECT_TRUE(record->parentDeleted);
    }
    EXPECT_EQ(find(scan, "/$OrphanFiles/_MG_0010.JPG"), nullptr) << "the first cluster's entries are gone";
}

TEST(Fat32DeletedFolderTest, APartOfAFolderGoesWhereItsSubfolderSaysItWas) {
    test::Fat32BuilderOptions options;
    options.entriesBeforeData = true;
    Fat32ImageBuilder builder(options);
    const auto folder = builder.addDirectory(builder.root(), "Album");
    std::vector<Fat32ImageBuilder::Entry> gone;
    for (int i = 0; i < 14; ++i) {
        gone.push_back(builder.addFile(folder.clusters.front(), "PIC" + std::to_string(10 + i) + ".JPG",
                                       bytesOf(600, 90 + static_cast<std::uint64_t>(i))));
    }
    // Then nothing the scan knows: free space, then another folder, after which the album grew.
    builder.skipClusters(20);
    (void)builder.addDirectory(builder.root(), "Other");
    const Bytes last = bytesOf(900, 110);
    gone.push_back(builder.addFile(folder.clusters.front(), "LAST.JPG", last));
    const auto sub = builder.addDirectory(folder.clusters.front(), "SUB");
    const Bytes inner = bytesOf(1200, 111);
    const auto innerEntry = builder.addFile(sub.clusters.front(), "INNER.JPG", inner);
    ASSERT_GE(builder.directoryClusters(folder.clusters.front()).size(), 2u);
    builder.deleteEntry(innerEntry);
    builder.deleteEntry(sub);
    for (const auto& file : gone) {
        builder.deleteEntry(file);
    }
    builder.deleteEntry(folder);

    const Mounted m = mount(builder.build());
    const FileScan scan = m.scan();
    expectDeletedFile(m, scan, "/Album/_AST.JPG", gone.back().clusters.front(), last, false);
    const FileRecord* subfolder = find(scan, "/Album/_UB");
    ASSERT_NE(subfolder, nullptr);
    EXPECT_TRUE(subfolder->entry.isDirectory);
    EXPECT_FALSE(subfolder->entry.hasIssue(EntryIssue::ParentMissing));
    expectDeletedFile(m, scan, "/Album/_UB/_NNER.JPG", innerEntry.clusters.front(), inner, false);
    EXPECT_FALSE(listsOrphans(scan));
}

TEST(Fat32DeletedFolderTest, NoPartsAreLookedForWhenDeletedFoldersAreNotSearched) {
    FolderGrownAfterAnother grown;
    const Mounted m = mount(grown.builder.build());
    ScanLimits limits;
    limits.recurseIntoDeletedDirectories = false;
    Result<FileScan> scan = m.fs->scan(limits, {});
    RECOVERY_ASSERT_OK(scan);
    EXPECT_FALSE(listsOrphans(*scan));
    EXPECT_EQ(find(*scan, grown.paths[4]), nullptr);
}

// ---------------------------------------------------------------------------
// L249: folders no listing reaches, as a format leaves them, found in the
// free space
// ---------------------------------------------------------------------------

// What a quick format leaves of `old`: a new boot sector, FATs and root folder
// (cluster 2), and in every other cluster what the old volume held there.
Fat32ImageBuilder formatOf(Fat32ImageBuilder& old, const test::Fat32BuilderOptions& options) {
    const Bytes image = old.build();
    Fat32ImageBuilder fresh(options);
    const auto from = static_cast<std::ptrdiff_t>(fresh.clusterOffset(3));
    std::copy(image.begin() + from, image.end(), fresh.raw().begin() + from);
    return fresh;
}

// A card used, then quick-formatted, then written to: its old folders are in
// its free space, with nothing leading to them, the folder of a camera's
// photos in its own folder, and a folder of songs.
struct FormattedCard {
    test::Fat32BuilderOptions options{.entriesBeforeData = true};
    Fat32ImageBuilder old{options};
    Fat32ImageBuilder::Entry dcim;
    Fat32ImageBuilder::Entry camera;
    Fat32ImageBuilder::Entry photo;
    Fat32ImageBuilder::Entry music;
    Fat32ImageBuilder::Entry song;
    Bytes photoData = bytesOf(5000, 120);
    Bytes songData = bytesOf(4000, 121);

    FormattedCard() {
        // Written first: the file written after the format takes its clusters only.
        (void)old.addFile(old.root(), "FIRST.BIN", bytesOf(3000, 122));
        dcim = old.addDirectory(old.root(), "DCIM");
        camera = old.addDirectory(dcim.clusters.front(), "Camera");
        photo = old.addFile(camera.clusters.front(), "IMG_20240517_134530.jpg", photoData);
        music = old.addDirectory(old.root(), "Music");
        song = old.addFile(music.clusters.front(), "Song of the day.mp3", songData);
    }

    [[nodiscard]] Bytes image() {
        Fat32ImageBuilder fresh = formatOf(old, options);
        (void)fresh.addFile(fresh.root(), "NEW.BIN", bytesOf(1000, 123));
        return fresh.build();
    }

    [[nodiscard]] static std::string folder(const Fat32ImageBuilder::Entry& entry) {
        return "/$OrphanFiles/Folder " + std::to_string(entry.clusters.front());
    }
};

TEST(Fat32FoundFolderTest, TheFoldersOfAFormattedVolumeAreFoundWithTheirNamesAndFiles) {
    FormattedCard card;
    const Mounted m = mount(card.image(), {}, false, true);
    const FileScan scan = m.scan();

    // The top folders' names were in the old root folder: they are named by
    // their clusters. What they hold has its names.
    const FileRecord* camera = find(scan, FormattedCard::folder(card.dcim) + "/Camera");
    ASSERT_NE(camera, nullptr);
    EXPECT_TRUE(camera->entry.isDirectory);
    EXPECT_TRUE(camera->entry.hasIssue(EntryIssue::ParentMissing));
    const std::string photoPath = FormattedCard::folder(card.dcim) + "/Camera/IMG_20240517_134530.jpg";
    expectDeletedFile(m, scan, photoPath, card.photo.clusters.front(), card.photoData, false);
    EXPECT_FALSE(find(scan, photoPath)->entry.hasIssue(EntryIssue::ParentMissing));
    EXPECT_TRUE(find(scan, photoPath)->parentDeleted);
    EXPECT_EQ(find(scan, photoPath)->allocation.method, AllocationMethod::ContiguousGuess);
    // Every entry below a folder found says so; the file written since does not.
    EXPECT_TRUE(find(scan, photoPath)->entry.hasIssue(EntryIssue::FolderFound));
    EXPECT_TRUE(camera->entry.hasIssue(EntryIssue::FolderFound));
    EXPECT_FALSE(find(scan, "/NEW.BIN")->entry.hasIssue(EntryIssue::FolderFound));
    const std::string songPath = FormattedCard::folder(card.music) + "/Song of the day.mp3";
    expectDeletedFile(m, scan, songPath, card.song.clusters.front(), card.songData, false);
    EXPECT_TRUE(find(scan, songPath)->entry.hasIssue(EntryIssue::ParentMissing));
    // Nothing else: the new file, and what the folders found hold, once each.
    std::set<std::string> paths;
    for (const FileRecord& record : scan.records) {
        EXPECT_TRUE(record.path == "/NEW.BIN" || record.path.starts_with("/$OrphanFiles/Folder ")) << record.path;
        EXPECT_TRUE(paths.insert(record.path).second) << record.path;
    }
    EXPECT_EQ(paths.size(), 4u);
}

TEST(Fat32FoundFolderTest, TheFreeSpaceIsSearchedOnlyWhenAsked) {
    FormattedCard card;
    const Mounted m = mount(card.image());
    const FileScan scan = m.scan();
    EXPECT_FALSE(listsOrphans(scan));
    ASSERT_EQ(scan.records.size(), 1u);
    EXPECT_EQ(scan.records.front().path, "/NEW.BIN");
}

TEST(Fat32FoundFolderTest, NoFolderIsSearchedForWhenDeletedFilesAreLeftOut) {
    FormattedCard card;
    const Mounted m = mount(card.image(), {}, false, true);
    ScanLimits limits;
    limits.includeDeleted = false;
    limits.recurseIntoDeletedDirectories = false;
    Result<FileScan> scan = m.fs->scan(limits, {});
    RECOVERY_ASSERT_OK(scan);
    EXPECT_FALSE(listsOrphans(*scan));
}

TEST(Fat32FoundFolderTest, AFolderTheListingReachesIsNotListedAgain) {
    // A deleted folder its parent's entry leads to, and the parts of it found apart (L243).
    FolderGrownAfterAnother grown;
    const Mounted m = mount(grown.builder.build(), {}, false, true);
    const FileScan scan = m.scan();
    for (std::size_t i = 0; i < grown.files.size(); ++i) {
        expectDeletedFile(m, scan, grown.paths[i], grown.files[i].clusters.front(), grown.data[i], false);
    }
    EXPECT_FALSE(listsOrphans(scan));
}

TEST(Fat32FoundFolderTest, AFolderWhoseEntryIsLostGoesIntoTheFolderItsDotDotNames) {
    FormattedCard card;
    // The Camera entry in DCIM no longer names Camera's cluster.
    const std::span<std::byte> entry = card.old.shortEntry(card.camera);
    storeLe16(entry, 20, 0);
    storeLe16(entry, 26, 0);
    const Mounted m = mount(card.image(), {}, false, true);
    const FileScan scan = m.scan();
    const std::string folder =
        FormattedCard::folder(card.dcim) + "/Folder " + std::to_string(card.camera.clusters.front());
    expectDeletedFile(m, scan, folder + "/IMG_20240517_134530.jpg", card.photo.clusters.front(), card.photoData,
                      false);
    EXPECT_FALSE(find(scan, folder + "/IMG_20240517_134530.jpg")->entry.hasIssue(EntryIssue::ParentMissing));
}

TEST(Fat32FoundFolderTest, AFolderIsNotPutIntoAFolderWrittenWhereItsParentWas) {
    FormattedCard card;
    Fat32ImageBuilder fresh = formatOf(card.old, card.options);
    (void)fresh.addFile(fresh.root(), "NEW.BIN", bytesOf(1000, 123));
    // A folder written after the format takes the old DCIM's cluster, which Camera's ".." names.
    fresh.skipClusters(card.dcim.clusters.front() - fresh.cursor());
    const auto pictures = fresh.addDirectory(fresh.root(), "Pictures");
    ASSERT_EQ(pictures.clusters.front(), card.dcim.clusters.front());
    const Mounted m = mount(fresh.build(), {}, false, true);
    const FileScan scan = m.scan();
    const std::string path = FormattedCard::folder(card.camera) + "/IMG_20240517_134530.jpg";
    expectDeletedFile(m, scan, path, card.photo.clusters.front(), card.photoData, false);
    EXPECT_TRUE(find(scan, path)->entry.hasIssue(EntryIssue::ParentMissing));
    for (const FileRecord& record : scan.records) {
        EXPECT_FALSE(record.path.starts_with("/Pictures/")) << record.path;
    }
}

TEST(Fat32FoundFolderTest, AClusterWhoseDotEntryNamesAnotherClusterIsNoFolder) {
    FormattedCard card;
    // A copy of DCIM's first cluster elsewhere: its "." entry names DCIM's cluster.
    const std::uint32_t copy = card.old.lastCluster() - 10;
    const auto from = static_cast<std::ptrdiff_t>(card.old.clusterOffset(card.dcim.clusters.front()));
    std::copy_n(card.old.raw().begin() + from, card.old.clusterSize(),
                card.old.raw().begin() + static_cast<std::ptrdiff_t>(card.old.clusterOffset(copy)));
    const Mounted m = mount(card.image(), {}, false, true);
    const FileScan scan = m.scan();
    EXPECT_NE(find(scan, FormattedCard::folder(card.dcim) + "/Camera"), nullptr);
    // Not taken for a folder at all: neither listed nor tried.
    const std::string copied = "/$OrphanFiles/Folder " + std::to_string(copy);
    for (const FileRecord& record : scan.records) {
        EXPECT_FALSE(record.path.starts_with(copied)) << record.path;
    }
    for (const ScanIssue& issue : scan.issues) {
        EXPECT_FALSE(issue.path.starts_with(copied)) << issue.path << ": " << issue.detail;
    }
}

}  // namespace
}  // namespace recovery::filesystem::fat32
