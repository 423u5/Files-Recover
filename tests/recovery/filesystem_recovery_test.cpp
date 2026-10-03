// Filesystem-based recovery: candidates built from FAT32, exFAT and NTFS
// metadata on generated images, and their reconstructed data compared with
// the original files. Covers contiguous, fragmented, deleted, partially
// overwritten, sparse and resident files, Unicode and duplicate names, and
// volumes inside partitions.

#include "recovery/filesystem_recovery.hpp"

#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/candidate_reader.hpp"
#include "support/candidate_helpers.hpp"
#include "support/exfat_builder.hpp"
#include "support/fat32_builder.hpp"
#include "support/memory_source.hpp"
#include "support/ntfs_builder.hpp"
#include "support/partition_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>

namespace recovery {
namespace {

using filesystem::AllocationIssue;
using filesystem::AllocationMethod;
using filesystem::EntryState;
using filesystem::FilesystemType;
using test::candidateAt;
using test::candidatesAt;
using test::describeCandidate;

constexpr std::uint32_t kClusterSize = 512;  // every builder's default geometry

std::vector<std::byte> bytesOf(std::size_t size, std::uint64_t seed) {
    return test::makePattern(size, seed);
}

// A volume opened for recovery, with its candidates.
struct Opened {
    std::unique_ptr<test::MemoryStorageSource> source;
    std::unique_ptr<FilesystemRecovery> recovery;
    CandidateScan scan;

    [[nodiscard]] test::Reconstructed reconstruct(const RecoveryCandidate& candidate) const {
        return test::reconstructToMemory(*source, candidate);
    }
};

Opened openImage(std::vector<std::byte> image, const CandidateOptions& options = {}) {
    Opened opened;
    opened.source = std::make_unique<test::MemoryStorageSource>(std::move(image), 512);
    EXPECT_TRUE(opened.source->open().ok());
    Result<std::unique_ptr<FilesystemRecovery>> recovery = openFilesystemRecovery(*opened.source, 0);
    EXPECT_TRUE(recovery.ok()) << (recovery.ok() ? "" : describe(recovery.error()));
    if (!recovery.ok()) {
        return opened;
    }
    opened.recovery = std::move(recovery).value();
    Result<CandidateScan> scan = opened.recovery->findCandidates(options, {});
    EXPECT_TRUE(scan.ok()) << (scan.ok() ? "" : describe(scan.error()));
    if (scan.ok()) {
        opened.scan = std::move(scan).value();
    }
    for (const RecoveryCandidate& candidate : opened.scan.candidates) {
        test::expectWellFormed(candidate, 0, opened.source->size());
    }
    return opened;
}

// The reconstruction of `candidate` is exactly `original`, read in full.
void expectRecovered(const Opened& opened, const RecoveryCandidate* candidate, const std::vector<std::byte>& original) {
    ASSERT_NE(candidate, nullptr);
    const test::Reconstructed rebuilt = opened.reconstruct(*candidate);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_TRUE(rebuilt.report.allBytesRead()) << describeCandidate(*candidate);
    EXPECT_EQ(rebuilt.report.outputSize, original.size());
    EXPECT_TRUE(rebuilt.data == original) << describeCandidate(*candidate);
}

// ===========================================================================
// FAT32
// ===========================================================================

TEST(Fat32RecoveryTest, ContiguousFile) {
    test::Fat32ImageBuilder builder;
    const auto data = bytesOf(5000, 1);
    const auto entry = builder.addFile(test::Fat32ImageBuilder::root(), "Photo.JPG", data);
    const Opened opened = openImage(builder.build());
    ASSERT_NE(opened.recovery, nullptr);
    EXPECT_EQ(opened.scan.filesystemInfo.type, FilesystemType::Fat32);

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/Photo.JPG");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->method, RecoveryMethod::Filesystem);
    EXPECT_EQ(candidate->filename, "Photo.JPG");
    EXPECT_EQ(candidate->extension, "jpg");
    EXPECT_EQ(candidate->expectedSize, 5000u);
    EXPECT_FALSE(candidate->isDeleted());
    EXPECT_EQ(candidate->filesystemEvidence.type, FilesystemType::Fat32);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.method, AllocationMethod::ClusterChain);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::Recorded);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.clusterCount, 10u);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.clusterSize, kClusterSize);
    EXPECT_EQ(candidate->fragmentation.fragmentCount, 1u);
    EXPECT_TRUE(candidate->fragmentation.known);
    EXPECT_TRUE(candidate->warnings.empty()) << describeCandidate(*candidate);
    ASSERT_EQ(candidate->sourceRegions.size(), 1u);
    EXPECT_EQ(candidate->sourceRegions[0],
              (SourceRegion{0, 5000, RegionKind::Stored, builder.clusterOffset(entry.clusters.front()), false}));
    EXPECT_EQ(candidate->sourceOffset(), builder.clusterOffset(entry.clusters.front()));
    EXPECT_EQ(candidate->filesystemEvidence.metadataOffset, builder.shortEntryOffset(entry));
    ASSERT_TRUE(candidate->filesystemEvidence.modified.has_value());
    EXPECT_TRUE(candidate->filesystemEvidence.modified->local);
    expectRecovered(opened, candidate, data);
}

TEST(Fat32RecoveryTest, FragmentedFileFollowsItsChain) {
    test::Fat32ImageBuilder builder;
    const auto data = bytesOf(5 * kClusterSize - 100, 2);
    (void)builder.addFileInClusters(test::Fat32ImageBuilder::root(), "frag.bin", data, {100, 101, 200, 150, 151});
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/frag.bin");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->fragmentation.fragmentCount, 3u);
    EXPECT_TRUE(candidate->fragmentation.fragmented());
    EXPECT_TRUE(candidate->fragmentation.known);
    ASSERT_EQ(candidate->sourceRegions.size(), 3u) << describeCandidate(*candidate);
    EXPECT_EQ(candidate->sourceRegions[1].sourceOffset, builder.clusterOffset(200));
    EXPECT_EQ(candidate->sourceRegions[2].fileOffset, 3u * kClusterSize);
    expectRecovered(opened, candidate, data);
}

TEST(Fat32RecoveryTest, DeletedContiguousFileIsAGuessThatHolds) {
    test::Fat32ImageBuilder builder;
    const auto data = bytesOf(3000, 3);
    const auto entry = builder.addFile(test::Fat32ImageBuilder::root(), "Holiday video.mp4", data);
    builder.deleteEntry(entry);
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/Holiday video.mp4");
    ASSERT_NE(candidate, nullptr);
    EXPECT_TRUE(candidate->isDeleted());
    EXPECT_EQ(candidate->filesystemEvidence.state, EntryState::Deleted);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.method, AllocationMethod::ContiguousGuess);
    // Windows cleared the chain: the contiguous layout is an assumption, and says so.
    EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::Guessed);
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::LayoutGuessed));
    EXPECT_FALSE(candidate->fragmentation.known);
    EXPECT_FALSE(candidate->hasWarning(CandidateWarning::ClustersReallocated));
    expectRecovered(opened, candidate, data);
}

TEST(Fat32RecoveryTest, DeletedSingleClusterFileHasAnExactLayout) {
    test::Fat32ImageBuilder builder;
    const auto data = bytesOf(400, 4);
    builder.deleteEntry(builder.addFile(test::Fat32ImageBuilder::root(), "small note.txt", data));
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/small note.txt");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.method, AllocationMethod::ContiguousGuess);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::Recorded);
    EXPECT_TRUE(candidate->warnings.empty()) << describeCandidate(*candidate);
    expectRecovered(opened, candidate, data);
}

TEST(Fat32RecoveryTest, DeletedFragmentedFileIsNeverPresentedAsExact) {
    test::Fat32ImageBuilder builder;
    const auto root = test::Fat32ImageBuilder::root();
    const auto data = bytesOf(4 * kClusterSize, 5);
    const auto frag = builder.addFileInClusters(root, "frag.jpg", data, {300, 301, 310, 311});
    const auto keep = bytesOf(4 * kClusterSize, 6);
    (void)builder.addFileInClusters(root, "keep.bin", keep, {302, 303, 304, 305});
    builder.deleteEntry(frag);
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/frag.jpg");
    ASSERT_NE(candidate, nullptr) << describeCandidate(opened.scan.candidates.front());
    // The guess runs 300-303, and 302-303 belong to keep.bin: flagged, not silently wrong.
    EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::Guessed);
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::LayoutGuessed));
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::ClustersReallocated));
    ASSERT_EQ(candidate->sourceRegions.size(), 2u) << describeCandidate(*candidate);
    EXPECT_EQ(candidate->sourceRegions[0], (SourceRegion{0, 2 * kClusterSize, RegionKind::Stored,
                                                         builder.clusterOffset(300), false}));
    EXPECT_EQ(candidate->sourceRegions[1], (SourceRegion{2 * kClusterSize, 2 * kClusterSize, RegionKind::Stored,
                                                         builder.clusterOffset(302), true}));

    const test::Reconstructed rebuilt = opened.reconstruct(*candidate);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_EQ(rebuilt.report.reallocatedBytes, 2u * kClusterSize);
    // The first fragment is intact; the guessed rest is keep.bin's data.
    EXPECT_TRUE(std::equal(data.begin(), data.begin() + 2 * kClusterSize, rebuilt.data.begin()));
    EXPECT_TRUE(std::equal(keep.begin(), keep.begin() + 2 * kClusterSize, rebuilt.data.begin() + 2 * kClusterSize));
}

TEST(Fat32RecoveryTest, PartiallyOverwrittenFileShowsWhichPartsWereReused) {
    test::Fat32ImageBuilder builder;
    const auto root = test::Fat32ImageBuilder::root();
    const auto movie = bytesOf(8 * kClusterSize, 7);
    const auto entry = builder.addFileInClusters(root, "movie.mp4", movie, {400, 401, 402, 403, 404, 405, 406, 407});
    builder.deleteEntry(entry);
    const auto later = bytesOf(2 * kClusterSize, 8);
    (void)builder.addFileInClusters(root, "later.txt", later, {403, 404});
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/movie.mp4");
    ASSERT_NE(candidate, nullptr);
    ASSERT_EQ(candidate->sourceRegions.size(), 3u) << describeCandidate(*candidate);
    EXPECT_FALSE(candidate->sourceRegions[0].reallocated);
    EXPECT_EQ(candidate->sourceRegions[1], (SourceRegion{3 * kClusterSize, 2 * kClusterSize, RegionKind::Stored,
                                                         builder.clusterOffset(403), true}));
    EXPECT_FALSE(candidate->sourceRegions[2].reallocated);
    EXPECT_EQ(candidate->reallocatedBytes(), 2u * kClusterSize);

    const test::Reconstructed rebuilt = opened.reconstruct(*candidate);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_EQ(rebuilt.report.reallocatedBytes, 2u * kClusterSize);
    ASSERT_EQ(rebuilt.data.size(), movie.size());
    // Intact where the clusters are still free, the later file's data where they were reused.
    for (std::size_t i = 0; i < movie.size(); ++i) {
        const bool reused = i >= 3 * kClusterSize && i < 5 * kClusterSize;
        const std::byte expected = reused ? later[i - 3 * kClusterSize] : movie[i];
        ASSERT_EQ(rebuilt.data[i], expected) << "byte " << i;
    }
}

TEST(Fat32RecoveryTest, UnicodeNames) {
    test::Fat32ImageBuilder builder;
    const auto root = test::Fat32ImageBuilder::root();
    const auto active = bytesOf(1500, 9);
    const auto deleted = bytesOf(900, 10);
    (void)builder.addFile(root, "Фото из отпуска 😀.jpg", active);
    builder.deleteEntry(builder.addFile(root, "日本の写真.PNG", deleted));
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* photo = candidateAt(opened.scan, "/Фото из отпуска 😀.jpg");
    ASSERT_NE(photo, nullptr);
    EXPECT_EQ(photo->filename, "Фото из отпуска 😀.jpg");
    EXPECT_EQ(photo->extension, "jpg");
    expectRecovered(opened, photo, active);

    const RecoveryCandidate* japanese = candidateAt(opened.scan, "/日本の写真.PNG");
    ASSERT_NE(japanese, nullptr);
    EXPECT_TRUE(japanese->isDeleted());
    EXPECT_EQ(japanese->extension, "png");
    expectRecovered(opened, japanese, deleted);
}

TEST(Fat32RecoveryTest, DuplicateNamesAreSeparateCandidates) {
    test::Fat32ImageBuilder builder;
    const auto root = test::Fat32ImageBuilder::root();
    const auto first = bytesOf(2000, 11);
    const auto second = bytesOf(2500, 12);
    builder.deleteEntry(builder.addFile(root, "My Photo.jpg", first));
    (void)builder.addFile(root, "My Photo.jpg", second);
    const auto a = builder.addDirectory(root, "A");
    const auto b = builder.addDirectory(root, "B");
    const auto inA = bytesOf(700, 13);
    const auto inB = bytesOf(800, 14);
    (void)builder.addFile(a.clusters.front(), "same.txt", inA);
    (void)builder.addFile(b.clusters.front(), "same.txt", inB);
    const Opened opened = openImage(builder.build());

    const auto photos = candidatesAt(opened.scan, "/My Photo.jpg");
    ASSERT_EQ(photos.size(), 2u);
    EXPECT_NE(photos[0]->id, photos[1]->id);
    const RecoveryCandidate* deletedPhoto = photos[0]->isDeleted() ? photos[0] : photos[1];
    const RecoveryCandidate* activePhoto = photos[0]->isDeleted() ? photos[1] : photos[0];
    ASSERT_TRUE(deletedPhoto->isDeleted());
    ASSERT_FALSE(activePhoto->isDeleted());
    expectRecovered(opened, deletedPhoto, first);
    expectRecovered(opened, activePhoto, second);
    expectRecovered(opened, candidateAt(opened.scan, "/A/same.txt"), inA);
    expectRecovered(opened, candidateAt(opened.scan, "/B/same.txt"), inB);

    std::set<std::uint64_t> ids;
    for (const RecoveryCandidate& candidate : opened.scan.candidates) {
        EXPECT_TRUE(ids.insert(candidate.id.value()).second);
    }
}

TEST(Fat32RecoveryTest, FilesInsideDeletedDirectory) {
    test::Fat32ImageBuilder builder;
    const auto dir = builder.addDirectory(test::Fat32ImageBuilder::root(), "Old Pictures");
    const auto data = bytesOf(1200, 15);
    const auto file = builder.addFile(dir.clusters.front(), "beach.jpg", data);
    builder.deleteEntry(file);
    builder.deleteEntry(dir);
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/Old Pictures/beach.jpg");
    ASSERT_NE(candidate, nullptr);
    EXPECT_TRUE(candidate->isDeleted());
    EXPECT_TRUE(candidate->filesystemEvidence.parentDeleted);
    EXPECT_EQ(opened.scan.directories, 1u);
    expectRecovered(opened, candidate, data);
}

TEST(Fat32RecoveryTest, EmptyFile) {
    test::Fat32ImageBuilder builder;
    (void)builder.addFile(test::Fat32ImageBuilder::root(), "empty.txt", {});
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/empty.txt");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->expectedSize, 0u);
    EXPECT_TRUE(candidate->sourceRegions.empty());
    EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::None);
    EXPECT_EQ(candidate->fragmentation.fragmentCount, 0u);
    EXPECT_FALSE(candidate->sourceOffset().has_value());
    EXPECT_TRUE(candidate->warnings.empty());
    expectRecovered(opened, candidate, {});
}

TEST(Fat32RecoveryTest, TruncatedChainLeavesTheRestMissing) {
    test::Fat32ImageBuilder builder;
    const auto data = bytesOf(6 * kClusterSize, 16);
    const auto entry = builder.addFile(test::Fat32ImageBuilder::root(), "cut.bin", data);
    builder.setFat(entry.clusters[2], 0x0FFFFFFF);  // chain ends after three clusters
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/cut.bin");
    ASSERT_NE(candidate, nullptr);
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::DataMissing));
    ASSERT_EQ(candidate->sourceRegions.size(), 2u);
    EXPECT_EQ(candidate->sourceRegions[1],
              (SourceRegion{3 * kClusterSize, 3 * kClusterSize, RegionKind::Missing, 0, false}));
    EXPECT_EQ(candidate->bytes(RegionKind::Missing), 3u * kClusterSize);

    const test::Reconstructed rebuilt = opened.reconstruct(*candidate);
    ASSERT_TRUE(rebuilt.ok);
    // Missing data at the end is left out, not zero-filled.
    EXPECT_EQ(rebuilt.report.outputSize, 3u * kClusterSize);
    EXPECT_EQ(rebuilt.report.missingBytes, 3u * kClusterSize);
    EXPECT_FALSE(rebuilt.report.allBytesRead());
    EXPECT_TRUE(std::equal(rebuilt.data.begin(), rebuilt.data.end(), data.begin()));
}

TEST(Fat32RecoveryTest, DeletedEntryWithoutAStartClusterHasNothingLocated) {
    test::Fat32ImageBuilder builder;
    const auto entry = builder.addFile(test::Fat32ImageBuilder::root(), "lost.jpg", bytesOf(2000, 17));
    builder.deleteEntry(entry);
    std::span<std::byte> slot = builder.shortEntry(entry);
    storeLe16(slot, 20, 0);
    storeLe16(slot, 26, 0);
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/lost.jpg");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::None);
    ASSERT_EQ(candidate->sourceRegions.size(), 1u);
    EXPECT_EQ(candidate->sourceRegions[0].kind, RegionKind::Missing);
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::DataMissing));
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::AllocationDamaged));
    const test::Reconstructed rebuilt = opened.reconstruct(*candidate);
    EXPECT_EQ(rebuilt.report.outputSize, 0u);
    EXPECT_EQ(rebuilt.report.missingBytes, 2000u);
}

TEST(Fat32RecoveryTest, ActiveAndDeletedCanBeSelected) {
    test::Fat32ImageBuilder builder;
    const auto root = test::Fat32ImageBuilder::root();
    (void)builder.addFile(root, "active.txt", bytesOf(100, 18));
    builder.deleteEntry(builder.addFile(root, "deleted file.txt", bytesOf(100, 19)));
    const std::vector<std::byte> image = builder.build();

    CandidateOptions deletedOnly;
    deletedOnly.includeActive = false;
    const Opened deleted = openImage(image, deletedOnly);
    ASSERT_EQ(deleted.scan.candidates.size(), 1u);
    EXPECT_EQ(deleted.scan.candidates[0].filename, "deleted file.txt");

    CandidateOptions activeOnly;
    activeOnly.includeDeleted = false;
    const Opened active = openImage(image, activeOnly);
    ASSERT_EQ(active.scan.candidates.size(), 1u);
    EXPECT_EQ(active.scan.candidates[0].filename, "active.txt");
}

TEST(Fat32RecoveryTest, IdsFollowScanOrderFromFirstId) {
    test::Fat32ImageBuilder builder;
    const auto root = test::Fat32ImageBuilder::root();
    for (int i = 0; i < 5; ++i) {
        (void)builder.addFile(root, "file" + std::to_string(i) + ".bin", bytesOf(10, i));
    }
    CandidateOptions options;
    options.firstId = 1000;
    const Opened opened = openImage(builder.build(), options);
    ASSERT_EQ(opened.scan.candidates.size(), 5u);
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(opened.scan.candidates[i].id.value(), 1000u + i);
    }
}

// ===========================================================================
// exFAT
// ===========================================================================

TEST(ExFatRecoveryTest, ContiguousFileWithoutFatChain) {
    test::ExFatImageBuilder builder;
    const auto data = bytesOf(4000, 20);
    const auto entry = builder.addFile(builder.root(), "clip.MP4", data);
    const Opened opened = openImage(builder.build());
    EXPECT_EQ(opened.scan.filesystemInfo.type, FilesystemType::ExFat);

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/clip.MP4");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->extension, "mp4");
    EXPECT_EQ(candidate->filesystemEvidence.allocation.method, AllocationMethod::Contiguous);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::Recorded);
    EXPECT_EQ(candidate->filesystemEvidence.validDataLength, 4000u);
    EXPECT_FALSE(candidate->filesystemEvidence.modified->local);  // exFAT records a UTC offset
    ASSERT_EQ(candidate->sourceRegions.size(), 1u);
    EXPECT_EQ(candidate->sourceRegions[0].sourceOffset, builder.clusterOffset(entry.clusters.front()));
    EXPECT_TRUE(candidate->warnings.empty()) << describeCandidate(*candidate);
    expectRecovered(opened, candidate, data);
}

TEST(ExFatRecoveryTest, FragmentedFileFollowsItsChain) {
    test::ExFatImageBuilder builder;
    const auto data = bytesOf(4 * kClusterSize, 21);
    (void)builder.addFileInClusters(builder.root(), "frag.wav", data, {200, 210, 201, 300});
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/frag.wav");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.method, AllocationMethod::ClusterChain);
    EXPECT_EQ(candidate->fragmentation.fragmentCount, 4u);
    expectRecovered(opened, candidate, data);
}

TEST(ExFatRecoveryTest, DeletedContiguousFileKeepsItsExactRun) {
    test::ExFatImageBuilder builder;
    const auto data = bytesOf(5000, 22);
    builder.deleteEntry(builder.addFile(builder.root(), "deleted.jpg", data));
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/deleted.jpg");
    ASSERT_NE(candidate, nullptr);
    EXPECT_TRUE(candidate->isDeleted());
    EXPECT_EQ(candidate->filesystemEvidence.allocation.method, AllocationMethod::Contiguous);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::Recorded);
    EXPECT_TRUE(candidate->warnings.empty()) << describeCandidate(*candidate);
    expectRecovered(opened, candidate, data);
}

TEST(ExFatRecoveryTest, DeletedFragmentedFileUsesItsSurvivingChain) {
    test::ExFatImageBuilder builder;
    const auto data = bytesOf(4 * kClusterSize - 7, 23);
    builder.deleteEntry(builder.addFileInClusters(builder.root(), "frag.jpg", data, {400, 402, 404, 401}));
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/frag.jpg");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.method, AllocationMethod::ClusterChain);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::Recorded);
    EXPECT_EQ(candidate->fragmentation.fragmentCount, 4u);
    EXPECT_TRUE(candidate->fragmentation.known);
    expectRecovered(opened, candidate, data);
}

TEST(ExFatRecoveryTest, DeletedFragmentedFileWithClearedChainIsAGuess) {
    test::ExFatBuilderOptions options;
    options.clearFatOnDelete = true;
    test::ExFatImageBuilder builder(options);
    const auto data = bytesOf(4 * kClusterSize, 24);
    builder.deleteEntry(builder.addFileInClusters(builder.root(), "frag.jpg", data, {400, 402, 404, 401}));
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/frag.jpg");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.method, AllocationMethod::ContiguousGuess);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::Guessed);
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::LayoutGuessed));
    EXPECT_FALSE(candidate->fragmentation.known);
}

TEST(ExFatRecoveryTest, BytesBeyondTheValidDataLengthAreZeros) {
    test::ExFatImageBuilder builder;
    const auto data = bytesOf(3000, 25);
    const auto file = builder.addFile(builder.root(), "preallocated.bin", data);
    storeLe64(builder.slot(file, 1), 8, 1000);  // only the first 1000 bytes were written
    builder.rechecksum(file);
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/preallocated.bin");
    ASSERT_NE(candidate, nullptr);
    ASSERT_EQ(candidate->sourceRegions.size(), 2u) << describeCandidate(*candidate);
    EXPECT_EQ(candidate->sourceRegions[0].kind, RegionKind::Stored);
    EXPECT_EQ(candidate->sourceRegions[0].length, 1000u);
    EXPECT_EQ(candidate->sourceRegions[1], (SourceRegion{1000, 2000, RegionKind::Zeros, 0, false}));
    EXPECT_TRUE(candidate->warnings.empty());

    std::vector<std::byte> expected(data.begin(), data.begin() + 1000);
    expected.resize(3000);  // what Windows returns beyond the valid data length
    expectRecovered(opened, candidate, expected);
    const test::Reconstructed rebuilt = opened.reconstruct(*candidate);
    EXPECT_EQ(rebuilt.report.storedBytes, 1000u);
    EXPECT_EQ(rebuilt.report.zeroBytes, 2000u);
}

TEST(ExFatRecoveryTest, PartiallyOverwrittenFileShowsWhichPartsWereReused) {
    test::ExFatImageBuilder builder;
    const auto data = bytesOf(6 * kClusterSize, 26);
    const auto entry = builder.addFile(builder.root(), "movie.mp4", data);
    builder.deleteEntry(entry);
    const auto later = bytesOf(kClusterSize, 27);
    (void)builder.addFileInClusters(builder.root(), "later.txt", later, {entry.clusters[4]});
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/movie.mp4");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::Recorded);
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::ClustersReallocated));
    ASSERT_EQ(candidate->sourceRegions.size(), 3u) << describeCandidate(*candidate);
    EXPECT_EQ(candidate->sourceRegions[1], (SourceRegion{4 * kClusterSize, kClusterSize, RegionKind::Stored,
                                                         builder.clusterOffset(entry.clusters[4]), true}));
    const test::Reconstructed rebuilt = opened.reconstruct(*candidate);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_TRUE(std::equal(data.begin(), data.begin() + 4 * kClusterSize, rebuilt.data.begin()));
    EXPECT_TRUE(std::equal(later.begin(), later.end(), rebuilt.data.begin() + 4 * kClusterSize));
    EXPECT_TRUE(std::equal(data.begin() + 5 * kClusterSize, data.end(), rebuilt.data.begin() + 5 * kClusterSize));
}

TEST(ExFatRecoveryTest, UnicodeAndDuplicateNames) {
    test::ExFatImageBuilder builder;
    const auto greek = bytesOf(1000, 28);
    const auto first = bytesOf(1100, 29);
    const auto second = bytesOf(1200, 30);
    (void)builder.addFile(builder.root(), "Φωτογραφία Ёлка.jpeg", greek);
    builder.deleteEntry(builder.addFile(builder.root(), "IMG_0001.JPG", first));
    (void)builder.addFile(builder.root(), "IMG_0001.JPG", second);
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* unicode = candidateAt(opened.scan, "/Φωτογραφία Ёлка.jpeg");
    ASSERT_NE(unicode, nullptr);
    EXPECT_EQ(unicode->extension, "jpeg");
    EXPECT_FALSE(unicode->hasWarning(CandidateWarning::NameUncertain));
    expectRecovered(opened, unicode, greek);

    const auto duplicates = candidatesAt(opened.scan, "/IMG_0001.JPG");
    ASSERT_EQ(duplicates.size(), 2u);
    expectRecovered(opened, duplicates[0]->isDeleted() ? duplicates[0] : duplicates[1], first);
    expectRecovered(opened, duplicates[0]->isDeleted() ? duplicates[1] : duplicates[0], second);
}

TEST(ExFatRecoveryTest, OrphanedEntriesAreFlagged) {
    test::ExFatImageBuilder builder;
    const auto data = bytesOf(1500, 31);
    const auto file = builder.addFile(builder.root(), "A long orphaned file name.jpg", data);
    builder.deleteEntry(file);
    // A broken new entry set reused the File entry: only the Stream Extension and names survive.
    builder.slot(file, 0)[0] = std::byte{0x85};
    builder.slot(file, 0)[1] = std::byte{0x00};
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/A long orphaned file name.jpg");
    ASSERT_NE(candidate, nullptr) << opened.scan.candidates.size();
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::MetadataDamaged)) << describeCandidate(*candidate);
    expectRecovered(opened, candidate, data);
}

// ===========================================================================
// NTFS
// ===========================================================================

constexpr std::uint64_t kNtfsRoot = test::NtfsImageBuilder::root();

TEST(NtfsRecoveryTest, ResidentFileIsEmbedded) {
    test::NtfsImageBuilder builder;
    const auto data = bytesOf(300, 40);
    (void)builder.addFile(kNtfsRoot, "note.txt", data);
    const Opened opened = openImage(builder.build());
    EXPECT_EQ(opened.scan.filesystemInfo.type, FilesystemType::Ntfs);

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/note.txt");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.method, AllocationMethod::Resident);
    EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::Recorded);
    ASSERT_EQ(candidate->sourceRegions.size(), 1u);
    EXPECT_EQ(candidate->sourceRegions[0], (SourceRegion{0, 300, RegionKind::Embedded, 0, false}));
    EXPECT_EQ(candidate->embeddedData, data);
    EXPECT_FALSE(candidate->sourceOffset().has_value());
    EXPECT_EQ(candidate->fragmentation.fragmentCount, 0u);
    EXPECT_FALSE(candidate->filesystemEvidence.modified->local);
    expectRecovered(opened, candidate, data);
    EXPECT_EQ(opened.reconstruct(*candidate).report.embeddedBytes, 300u);
}

TEST(NtfsRecoveryTest, NonResidentAndFragmentedFiles) {
    test::NtfsImageBuilder builder;
    const auto plain = bytesOf(7000, 41);
    const auto fragmented = bytesOf(4 * kClusterSize - 3, 42);
    (void)builder.addFile(kNtfsRoot, "plain.bin", plain);
    (void)builder.addFileInClusters(kNtfsRoot, "frag.bin", fragmented, {5000, 5001, 5050, 5020});
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* single = candidateAt(opened.scan, "/plain.bin");
    ASSERT_NE(single, nullptr);
    EXPECT_EQ(single->filesystemEvidence.allocation.method, AllocationMethod::RunList);
    EXPECT_EQ(single->fragmentation.fragmentCount, 1u);
    expectRecovered(opened, single, plain);

    const RecoveryCandidate* frag = candidateAt(opened.scan, "/frag.bin");
    ASSERT_NE(frag, nullptr);
    EXPECT_EQ(frag->fragmentation.fragmentCount, 3u);
    EXPECT_EQ(frag->sourceRegions.size(), 3u);
    expectRecovered(opened, frag, fragmented);
}

TEST(NtfsRecoveryTest, DeletedFilesKeepTheirExactRuns) {
    test::NtfsImageBuilder builder;
    const auto resident = bytesOf(200, 43);
    const auto fragmented = bytesOf(5 * kClusterSize, 44);
    // Both are added before either is deleted: a new file would take the freed record.
    const auto note = builder.addFile(kNtfsRoot, "deleted note.txt", resident);
    const auto frag =
        builder.addFileInClusters(kNtfsRoot, "deleted frag.jpg", fragmented, {6000, 6100, 6001, 6200, 6201});
    builder.deleteEntry(note);
    builder.deleteEntry(frag);
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* deletedNote = candidateAt(opened.scan, "/deleted note.txt");
    ASSERT_NE(deletedNote, nullptr);
    EXPECT_TRUE(deletedNote->isDeleted());
    expectRecovered(opened, deletedNote, resident);

    const RecoveryCandidate* deletedFrag = candidateAt(opened.scan, "/deleted frag.jpg");
    ASSERT_NE(deletedFrag, nullptr);
    EXPECT_TRUE(deletedFrag->isDeleted());
    EXPECT_EQ(deletedFrag->filesystemEvidence.allocation.layout, LayoutEvidence::Recorded);
    EXPECT_EQ(deletedFrag->fragmentation.fragmentCount, 4u);
    EXPECT_TRUE(deletedFrag->warnings.empty()) << describeCandidate(*deletedFrag);
    expectRecovered(opened, deletedFrag, fragmented);
}

TEST(NtfsRecoveryTest, PartiallyOverwrittenFileShowsWhichPartsWereReused) {
    test::NtfsImageBuilder builder;
    // A new file takes the lowest free record, so a sacrificial record keeps
    // the deleted file's own record from being reused.
    const auto sacrificial = builder.addFile(kNtfsRoot, "tmp.bin", bytesOf(10, 45));
    const auto data = bytesOf(6 * kClusterSize, 46);
    const auto movie = builder.addFileInClusters(kNtfsRoot, "movie.mp4", data, {7000, 7001, 7002, 7003, 7004, 7005});
    builder.deleteEntry(sacrificial);
    builder.deleteEntry(movie);
    const auto later = bytesOf(2 * kClusterSize, 47);
    (void)builder.addFileInClusters(kNtfsRoot, "later.bin", later, {7002, 7003});
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/movie.mp4");
    ASSERT_NE(candidate, nullptr);
    ASSERT_EQ(candidate->sourceRegions.size(), 3u) << describeCandidate(*candidate);
    EXPECT_EQ(candidate->sourceRegions[1], (SourceRegion{2 * kClusterSize, 2 * kClusterSize, RegionKind::Stored,
                                                         builder.clusterOffset(7002), true}));
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::ClustersReallocated));
    const test::Reconstructed rebuilt = opened.reconstruct(*candidate);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_EQ(rebuilt.report.reallocatedBytes, 2u * kClusterSize);
    EXPECT_TRUE(std::equal(data.begin(), data.begin() + 2 * kClusterSize, rebuilt.data.begin()));
    EXPECT_TRUE(std::equal(later.begin(), later.end(), rebuilt.data.begin() + 2 * kClusterSize));
    EXPECT_TRUE(std::equal(data.begin() + 4 * kClusterSize, data.end(), rebuilt.data.begin() + 4 * kClusterSize));
}

TEST(NtfsRecoveryTest, SparseFileIsLaidOutWithItsHoles) {
    test::NtfsImageBuilder builder;
    const auto data = bytesOf(6 * kClusterSize, 48);
    (void)builder.addFileWithRuns(kNtfsRoot, "sparse.vhd", data,
                                  {{3000, 2}, {std::nullopt, 3}, {3100, 1}}, 0x8000);
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/sparse.vhd");
    ASSERT_NE(candidate, nullptr);
    ASSERT_EQ(candidate->sourceRegions.size(), 3u) << describeCandidate(*candidate);
    EXPECT_EQ(candidate->sourceRegions[0], (SourceRegion{0, 2 * kClusterSize, RegionKind::Stored,
                                                         builder.clusterOffset(3000), false}));
    EXPECT_EQ(candidate->sourceRegions[1],
              (SourceRegion{2 * kClusterSize, 3 * kClusterSize, RegionKind::Zeros, 0, false}));
    EXPECT_EQ(candidate->sourceRegions[2], (SourceRegion{5 * kClusterSize, kClusterSize, RegionKind::Stored,
                                                         builder.clusterOffset(3100), false}));
    EXPECT_EQ(candidate->fragmentation.fragmentCount, 2u);
    EXPECT_FALSE(candidate->hasWarning(CandidateWarning::DataMissing));

    std::vector<std::byte> expected = data;
    std::fill(expected.begin() + 2 * kClusterSize, expected.begin() + 5 * kClusterSize, std::byte{0});
    expectRecovered(opened, candidate, expected);
}

TEST(NtfsRecoveryTest, CompressedAndEncryptedDataIsNotDecoded) {
    test::NtfsImageBuilder builder;
    (void)builder.addFileWithRuns(kNtfsRoot, "packed.bin", bytesOf(4 * kClusterSize, 49), {{3200, 4}}, 0x0001);
    (void)builder.addFileWithRuns(kNtfsRoot, "secret.bin", bytesOf(4 * kClusterSize, 50), {{3300, 4}}, 0x4000);
    const Opened opened = openImage(builder.build());

    for (const std::string_view path : {"/packed.bin", "/secret.bin"}) {
        const RecoveryCandidate* candidate = candidateAt(opened.scan, path);
        ASSERT_NE(candidate, nullptr) << path;
        EXPECT_TRUE(candidate->hasWarning(CandidateWarning::DataNotDecoded)) << path;
        EXPECT_EQ(candidate->bytes(RegionKind::Missing), candidate->expectedSize) << path;
        EXPECT_EQ(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::None) << path;
    }
}

TEST(NtfsRecoveryTest, BytesBeyondTheInitializedSizeAreZeros) {
    test::NtfsImageBuilder builder;
    const auto data = bytesOf(3000, 51);
    const auto file = builder.addFile(kNtfsRoot, "growing.log", data);
    const std::size_t at = builder.attributeOffset(file.record, 0x80);
    storeLe64(builder.record(file.record), at + 0x38, 1200);  // initialized size
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* candidate = candidateAt(opened.scan, "/growing.log");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->filesystemEvidence.validDataLength, 1200u);
    std::vector<std::byte> expected(data.begin(), data.begin() + 1200);
    expected.resize(3000);
    expectRecovered(opened, candidate, expected);
}

TEST(NtfsRecoveryTest, MetadataFilesAreNotCandidates) {
    test::NtfsImageBuilder builder;
    (void)builder.addFile(kNtfsRoot, "user.txt", bytesOf(50, 52));
    const Opened opened = openImage(builder.build());

    ASSERT_EQ(opened.scan.candidates.size(), 1u);
    EXPECT_EQ(opened.scan.candidates[0].filesystemEvidence.path, "/user.txt");
    // $MFT, $MFTMirr, $LogFile, $Volume, $AttrDef, $Bitmap, $Boot, $BadClus, $Secure, $UpCase.
    EXPECT_EQ(opened.scan.systemFiles, 10u);
}

TEST(NtfsRecoveryTest, OrphansHardLinksUnicodeAndDuplicates) {
    test::NtfsImageBuilder builder;
    const auto gone = builder.addDirectory(kNtfsRoot, "Gone");
    const auto orphanData = bytesOf(2000, 53);
    (void)builder.addFile(gone.record, "orphan.jpg", orphanData);
    builder.deleteEntry(gone);
    (void)builder.addFile(kNtfsRoot, "reuses the directory record.bin", bytesOf(10, 54));  // takes record 64

    const auto linked = bytesOf(1800, 55);
    const auto original = builder.addFile(kNtfsRoot, "original.bin", linked);
    builder.addHardLink(original, kNtfsRoot, "link.bin");

    const auto emoji = bytesOf(2100, 56);
    (void)builder.addFile(kNtfsRoot, "Пляж 🏖️ 海.heic", emoji);

    const auto first = bytesOf(900, 57);
    const auto second = bytesOf(950, 58);
    const auto older = builder.addFile(kNtfsRoot, "dup.png", first);
    (void)builder.addFile(kNtfsRoot, "dup.png", second);
    builder.deleteEntry(older);
    const Opened opened = openImage(builder.build());

    const RecoveryCandidate* orphan = candidateAt(opened.scan, "/$OrphanFiles/orphan.jpg");
    ASSERT_NE(orphan, nullptr);
    EXPECT_TRUE(orphan->hasWarning(CandidateWarning::LocationUnknown));
    expectRecovered(opened, orphan, orphanData);

    expectRecovered(opened, candidateAt(opened.scan, "/original.bin"), linked);
    expectRecovered(opened, candidateAt(opened.scan, "/link.bin"), linked);

    const RecoveryCandidate* unicode = candidateAt(opened.scan, "/Пляж 🏖️ 海.heic");
    ASSERT_NE(unicode, nullptr);
    EXPECT_EQ(unicode->extension, "heic");
    expectRecovered(opened, unicode, emoji);

    const auto duplicates = candidatesAt(opened.scan, "/dup.png");
    ASSERT_EQ(duplicates.size(), 2u);
    expectRecovered(opened, duplicates[0]->isDeleted() ? duplicates[0] : duplicates[1], first);
    expectRecovered(opened, duplicates[0]->isDeleted() ? duplicates[1] : duplicates[0], second);
}

// ===========================================================================
// Detection, partitions, cancellation, logging
// ===========================================================================

TEST(FilesystemRecoveryTest, DetectsEachFilesystem) {
    test::Fat32ImageBuilder fat32;
    test::ExFatImageBuilder exfat;
    test::NtfsImageBuilder ntfs;
    EXPECT_EQ(openImage(fat32.build()).scan.filesystemInfo.type, FilesystemType::Fat32);
    EXPECT_EQ(openImage(exfat.build()).scan.filesystemInfo.type, FilesystemType::ExFat);
    EXPECT_EQ(openImage(ntfs.build()).scan.filesystemInfo.type, FilesystemType::Ntfs);
}

TEST(FilesystemRecoveryTest, DamagedPrimaryBootSectorFallsBackToTheBackup) {
    test::Fat32ImageBuilder builder;
    const auto data = bytesOf(1000, 60);
    (void)builder.addFile(test::Fat32ImageBuilder::root(), "kept.jpg", data);
    std::vector<std::byte> image = builder.build();
    std::fill(image.begin(), image.begin() + 512, std::byte{0});  // no signature left to recognise
    const Opened opened = openImage(std::move(image));
    ASSERT_NE(opened.recovery, nullptr);
    EXPECT_EQ(opened.scan.filesystemInfo.type, FilesystemType::Fat32);
    expectRecovered(opened, candidateAt(opened.scan, "/kept.jpg"), data);
}

TEST(FilesystemRecoveryTest, UnrecognisedVolume) {
    test::MemoryStorageSource blank(std::vector<std::byte>(1024 * 1024), 512);
    RECOVERY_ASSERT_OK(blank.open());
    RECOVERY_EXPECT_ERROR(openFilesystemRecovery(blank, 0), ErrorCode::UnsupportedFilesystem);

    test::MemoryStorageSource closed(std::vector<std::byte>(4096), 512);
    RECOVERY_EXPECT_ERROR(openFilesystemRecovery(closed, 0), ErrorCode::InvalidInput);
}

TEST(FilesystemRecoveryTest, CandidatesOfPartitionedDiskUseDiskOffsets) {
    test::Fat32ImageBuilder fat32;
    const auto fatData = bytesOf(3 * kClusterSize, 61);
    const auto fatDeleted = bytesOf(2 * kClusterSize + 5, 62);
    (void)fat32.addFileInClusters(test::Fat32ImageBuilder::root(), "a.jpg", fatData, {50, 60, 51});
    fat32.deleteEntry(fat32.addFile(test::Fat32ImageBuilder::root(), "gone.jpg", fatDeleted));
    const std::vector<std::byte> fatVolume = fat32.build();

    test::NtfsImageBuilder ntfs;
    const auto ntfsData = bytesOf(4 * kClusterSize, 63);
    const auto ntfsDeleted = bytesOf(3 * kClusterSize, 64);
    (void)ntfs.addFileInClusters(kNtfsRoot, "b.mp4", ntfsData, {6000, 6010, 6001, 6002});
    ntfs.deleteEntry(ntfs.addFileInClusters(kNtfsRoot, "gone.mp4", ntfsDeleted, {6100, 6050, 6101}));
    const std::vector<std::byte> ntfsVolume = ntfs.build();

    constexpr std::uint32_t kFirst = 2048;
    const auto fatSectors = static_cast<std::uint32_t>(fatVolume.size() / 512);
    const std::uint32_t second = kFirst + fatSectors + 2048;
    const auto ntfsSectors = static_cast<std::uint32_t>(ntfsVolume.size() / 512);
    std::vector<std::byte> disk((static_cast<std::size_t>(second) + ntfsSectors + 64) * 512);
    std::copy(fatVolume.begin(), fatVolume.end(), disk.begin() + std::size_t{kFirst} * 512);
    std::copy(ntfsVolume.begin(), ntfsVolume.end(), disk.begin() + std::size_t{second} * 512);
    test::writeMbrSector(disk, 512, 0, {{0x00, 0x0C, kFirst, fatSectors}, {0x00, 0x07, second, ntfsSectors}});

    test::MemoryStorageSource device(disk, 512);
    RECOVERY_ASSERT_OK(device.open());
    const Result<partition::PartitionTable> table = partition::readPartitionTable(device);
    RECOVERY_ASSERT_OK(table);
    ASSERT_EQ(table->partitions.size(), 2u);

    std::map<std::string, std::vector<std::byte>> expected{
        {"/a.jpg", fatData}, {"/gone.jpg", fatDeleted}, {"/b.mp4", ntfsData}, {"/gone.mp4", ntfsDeleted}};
    std::set<std::string> recovered;
    for (const partition::Partition& p : table->partitions) {
        partition::PartitionSource volume(device, p);
        RECOVERY_ASSERT_OK(volume.open());
        Result<std::unique_ptr<FilesystemRecovery>> recovery = openFilesystemRecovery(volume, p.offset);
        RECOVERY_ASSERT_OK(recovery);
        EXPECT_EQ(recovery.value()->volumeOffset(), p.offset);
        const Result<CandidateScan> scan = recovery.value()->findCandidates({}, {});
        RECOVERY_ASSERT_OK(scan);
        EXPECT_EQ(scan->volumeOffset, p.offset);
        for (const RecoveryCandidate& candidate : scan->candidates) {
            test::expectWellFormed(candidate, p.offset, p.offset + p.size);
            EXPECT_EQ(candidate.filesystemEvidence.volumeOffset, p.offset);
            EXPECT_GE(candidate.filesystemEvidence.metadataOffset, p.offset);
            // Reconstructed from the whole disk: the offsets are disk offsets.
            const test::Reconstructed rebuilt = test::reconstructToMemory(device, candidate);
            ASSERT_TRUE(rebuilt.ok);
            const auto original = expected.find(candidate.filesystemEvidence.path);
            ASSERT_NE(original, expected.end()) << candidate.filesystemEvidence.path;
            EXPECT_TRUE(rebuilt.data == original->second) << describeCandidate(candidate);
            recovered.insert(candidate.filesystemEvidence.path);
        }
    }
    EXPECT_EQ(recovered.size(), expected.size());
}

TEST(FilesystemRecoveryTest, FindingCandidatesCanBeCancelled) {
    test::Fat32ImageBuilder builder;
    (void)builder.addFile(test::Fat32ImageBuilder::root(), "a.bin", bytesOf(100, 65));
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    Result<std::unique_ptr<FilesystemRecovery>> recovery = openFilesystemRecovery(source, 0);
    RECOVERY_ASSERT_OK(recovery);
    CancellationSource cancel;
    cancel.requestCancellation();
    RECOVERY_EXPECT_ERROR(recovery.value()->findCandidates({}, cancel.token()), ErrorCode::Cancelled);
}

TEST(FilesystemRecoveryTest, LogsTheCandidateCount) {
    test::ExFatImageBuilder builder;
    (void)builder.addFile(builder.root(), "a.bin", bytesOf(100, 66));
    builder.deleteEntry(builder.addFile(builder.root(), "b.bin", bytesOf(100, 67)));
    auto sink = std::make_shared<diagnostics::MemorySink>();
    diagnostics::Logger logger(diagnostics::LogLevel::Info);
    logger.addSink(sink);
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    Result<std::unique_ptr<FilesystemRecovery>> recovery = openFilesystemRecovery(source, 0, {}, &logger);
    RECOVERY_ASSERT_OK(recovery);
    RECOVERY_ASSERT_OK(recovery.value()->findCandidates({}, {}));
    const auto records = sink->records();
    const auto found = std::find_if(records.begin(), records.end(),
                                    [](const auto& r) { return r.message == "recovery candidates found"; });
    ASSERT_NE(found, records.end());
    const auto has = [&](std::string_view key, std::string_view value) {
        return std::any_of(found->fields.begin(), found->fields.end(),
                           [&](const auto& f) { return f.key == key && f.value == value; });
    };
    EXPECT_TRUE(has("candidates", "2"));
    EXPECT_TRUE(has("deleted", "1"));
    EXPECT_TRUE(has("filesystem", "exFAT"));
}

}  // namespace
}  // namespace recovery
