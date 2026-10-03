// Filesystem-based recovery on damaged metadata: truncated and looping
// chains, cross-links, hostile sizes, invalid run lists, checksum
// mismatches, unreadable allocation tables and data sectors, and random
// mutation of FAT32, exFAT and NTFS images. Every candidate must stay
// well-formed and inside the volume, and its reconstruction bounded.

#include "recovery/filesystem_recovery.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/candidate_reader.hpp"
#include "recovery/checked_math.hpp"
#include "support/candidate_helpers.hpp"
#include "support/exfat_builder.hpp"
#include "support/fat32_builder.hpp"
#include "support/memory_source.hpp"
#include "support/ntfs_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <random>

namespace recovery {
namespace {

using filesystem::AllocationIssue;
using test::candidateAt;
using test::describeCandidate;

constexpr std::uint32_t kClusterSize = 512;
constexpr std::uint64_t kNtfsRoot = test::NtfsImageBuilder::root();

std::vector<std::byte> bytesOf(std::size_t size, std::uint64_t seed) {
    return test::makePattern(size, seed);
}

Result<CandidateScan> candidatesOf(test::MemoryStorageSource& source) {
    Result<std::unique_ptr<FilesystemRecovery>> recovery = openFilesystemRecovery(source, 0);
    if (!recovery.ok()) {
        return recovery.error();
    }
    return recovery.value()->findCandidates({}, {});
}

// ---------------------------------------------------------------------------
// Targeted damage
// ---------------------------------------------------------------------------

TEST(FilesystemRecoveryCorruptionTest, Fat32ChainLoopIsDamageAndMissingData) {
    test::Fat32ImageBuilder builder;
    const auto data = bytesOf(5 * kClusterSize, 1);
    const auto entry = builder.addFile(test::Fat32ImageBuilder::root(), "loop.bin", data);
    builder.setFat(entry.clusters[2], entry.clusters[0]);
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    const Result<CandidateScan> scan = candidatesOf(source);
    RECOVERY_ASSERT_OK(scan);

    const RecoveryCandidate* candidate = candidateAt(scan.value(), "/loop.bin");
    ASSERT_NE(candidate, nullptr);
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::AllocationDamaged)) << describeCandidate(*candidate);
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::DataMissing));
    EXPECT_EQ(candidate->bytes(RegionKind::Stored), 3u * kClusterSize);
    const test::Reconstructed rebuilt = test::reconstructToMemory(source, *candidate);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_TRUE(std::equal(rebuilt.data.begin(), rebuilt.data.end(), data.begin()));
}

TEST(FilesystemRecoveryCorruptionTest, Fat32HostileSizeIsBounded) {
    test::Fat32ImageBuilder builder;
    const auto data = bytesOf(3 * kClusterSize, 2);
    const auto entry = builder.addFile(test::Fat32ImageBuilder::root(), "huge.bin", data);
    storeLe32(builder.shortEntry(entry), 28, 0xFFFFFFFF);
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    const Result<CandidateScan> scan = candidatesOf(source);
    RECOVERY_ASSERT_OK(scan);

    const RecoveryCandidate* candidate = candidateAt(scan.value(), "/huge.bin");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->expectedSize, 0xFFFFFFFFu);
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::AllocationDamaged));
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::DataMissing));
    const test::Reconstructed rebuilt = test::reconstructToMemory(source, *candidate);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_EQ(rebuilt.report.outputSize, 3u * kClusterSize);  // the chain, not 4 GiB
    EXPECT_TRUE(rebuilt.data == data);
}

TEST(FilesystemRecoveryCorruptionTest, Fat32CrossLinkedFilesAreFlagged) {
    test::Fat32ImageBuilder builder;
    const auto root = test::Fat32ImageBuilder::root();
    (void)builder.addFileInClusters(root, "a.bin", bytesOf(2 * kClusterSize, 3), {10, 11});
    (void)builder.addFileInClusters(root, "b.bin", bytesOf(2 * kClusterSize, 4), {20, 21});
    builder.setFat(20, 11);  // b.bin now continues into a.bin's second cluster
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    const Result<CandidateScan> scan = candidatesOf(source);
    RECOVERY_ASSERT_OK(scan);
    for (const std::string_view path : {"/a.bin", "/b.bin"}) {
        const RecoveryCandidate* candidate = candidateAt(scan.value(), path);
        ASSERT_NE(candidate, nullptr);
        EXPECT_TRUE(candidate->hasWarning(CandidateWarning::CrossLinked)) << describeCandidate(*candidate);
    }
}

TEST(FilesystemRecoveryCorruptionTest, ExFatChecksumMismatchIsMetadataDamage) {
    test::ExFatImageBuilder builder;
    const auto data = bytesOf(2000, 5);
    const auto file = builder.addFile(builder.root(), "tampered.jpg", data);
    builder.slot(file, 0)[8] ^= std::byte{0x01};  // a timestamp byte, checksum not updated
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    const Result<CandidateScan> scan = candidatesOf(source);
    RECOVERY_ASSERT_OK(scan);
    const RecoveryCandidate* candidate = candidateAt(scan.value(), "/tampered.jpg");
    ASSERT_NE(candidate, nullptr);
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::MetadataDamaged));
    EXPECT_TRUE(test::reconstructToMemory(source, *candidate).data == data);
}

TEST(FilesystemRecoveryCorruptionTest, ExFatUnreadableBitmapMakesReuseUnknown) {
    test::ExFatImageBuilder builder;
    const auto data = bytesOf(1500, 6);
    builder.deleteEntry(builder.addFile(builder.root(), "gone.bin", data));
    test::MemoryStorageSource source(builder.build(), 512);
    source.addBadSector(builder.clusterOffset(builder.bitmapCluster()) / 512);
    RECOVERY_ASSERT_OK(source.open());
    const Result<CandidateScan> scan = candidatesOf(source);
    RECOVERY_ASSERT_OK(scan);
    const RecoveryCandidate* candidate = candidateAt(scan.value(), "/gone.bin");
    ASSERT_NE(candidate, nullptr);
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::AllocationUnknown));
    EXPECT_FALSE(candidate->hasWarning(CandidateWarning::ClustersReallocated));
    EXPECT_TRUE(test::reconstructToMemory(source, *candidate).data == data);
}

TEST(FilesystemRecoveryCorruptionTest, BadSectorInsideFileData) {
    test::NtfsImageBuilder builder;
    const auto data = bytesOf(8 * kClusterSize, 7);
    (void)builder.addFileInClusters(kNtfsRoot, "scratched.mp4", data,
                                    {5000, 5001, 5002, 5003, 6000, 6001, 6002, 6003});
    test::MemoryStorageSource source(builder.build(), 512);
    source.addBadSector(builder.clusterOffset(6001) / 512);
    RECOVERY_ASSERT_OK(source.open());
    const Result<CandidateScan> scan = candidatesOf(source);
    RECOVERY_ASSERT_OK(scan);
    const RecoveryCandidate* candidate = candidateAt(scan.value(), "/scratched.mp4");
    ASSERT_NE(candidate, nullptr);
    const test::Reconstructed rebuilt = test::reconstructToMemory(source, *candidate);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_EQ(rebuilt.report.unreadableBytes, kClusterSize);
    ASSERT_EQ(rebuilt.report.unreadableRegions.size(), 1u);
    EXPECT_EQ(rebuilt.report.unreadableRegions[0].offset, builder.clusterOffset(6001));
    std::vector<std::byte> expected = data;
    std::fill(expected.begin() + 5 * kClusterSize, expected.begin() + 6 * kClusterSize, std::byte{0});
    EXPECT_TRUE(rebuilt.data == expected);
}

TEST(FilesystemRecoveryCorruptionTest, NtfsInvalidRunListKeepsTheRunsBeforeTheDamage) {
    test::NtfsImageBuilder builder;
    const auto data = bytesOf(4 * kClusterSize, 8);
    const auto file = builder.addFileInClusters(kNtfsRoot, "broken runs.bin", data, {5000, 5001, 5100, 5101});
    const std::span<std::byte> record = builder.record(file.record);
    const std::size_t attribute = builder.attributeOffset(file.record, 0x80);
    const std::size_t pairs = attribute + loadLe16(record, attribute + 0x20);
    const auto header = std::to_integer<std::size_t>(record[pairs]);
    const std::size_t second = pairs + 1 + (header & 0x0F) + (header >> 4);
    record[second] = std::byte{0x01};  // a one-byte length field...
    record[second + 1] = std::byte{0x00};  // ...of zero
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    const Result<CandidateScan> scan = candidatesOf(source);
    RECOVERY_ASSERT_OK(scan);

    const RecoveryCandidate* candidate = candidateAt(scan.value(), "/broken runs.bin");
    ASSERT_NE(candidate, nullptr);
    EXPECT_TRUE(candidate->filesystemEvidence.allocation.hasIssue(AllocationIssue::InvalidRunList));
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::AllocationDamaged));
    EXPECT_TRUE(candidate->hasWarning(CandidateWarning::DataMissing));
    const test::Reconstructed rebuilt = test::reconstructToMemory(source, *candidate);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_EQ(rebuilt.report.outputSize, 2u * kClusterSize);
    EXPECT_TRUE(std::equal(rebuilt.data.begin(), rebuilt.data.end(), data.begin()));
}

TEST(FilesystemRecoveryCorruptionTest, NtfsSizeLargerThanTheVolumeIsBounded) {
    test::NtfsImageBuilder builder;
    const auto file = builder.addFileWithRuns(kNtfsRoot, "repeat.bin", bytesOf(kClusterSize, 9),
                                              {{5000, 1000}, {6100, 1000}, {7200, 900}});
    const auto sparse = builder.addFileWithRuns(kNtfsRoot, "hole.bin", bytesOf(kClusterSize, 10),
                                                {{3000, 1}, {std::nullopt, 4}}, 0x8000);
    for (const std::uint64_t number : {file.record, sparse.record}) {
        const std::span<std::byte> record = builder.record(number);
        const std::size_t attribute = builder.attributeOffset(number, 0x80);
        storeLe64(record, attribute + 0x30, 1ULL << 40);  // real size far beyond the volume
        storeLe64(record, attribute + 0x38, 1ULL << 40);  // initialized size
    }
    // Zero the relative offsets of the second and third runs: all three now
    // start at cluster 5000, repeating the same clusters.
    {
        const std::span<std::byte> record = builder.record(file.record);
        const std::size_t attribute = builder.attributeOffset(file.record, 0x80);
        std::size_t pair = attribute + loadLe16(record, attribute + 0x20);
        for (int i = 0; i < 3; ++i) {
            const auto header = std::to_integer<std::size_t>(record[pair]);
            const std::size_t lengthSize = header & 0x0F;
            const std::size_t offsetSize = header >> 4;
            if (i > 0) {
                std::fill_n(record.begin() + static_cast<std::ptrdiff_t>(pair + 1 + lengthSize),
                            offsetSize, std::byte{0});
            }
            pair += 1 + lengthSize + offsetSize;
        }
    }
    test::MemoryStorageSource source(builder.build(), 512);
    RECOVERY_ASSERT_OK(source.open());
    const Result<CandidateScan> scan = candidatesOf(source);
    RECOVERY_ASSERT_OK(scan);
    const std::uint64_t clusterArea = scan->filesystemInfo.clusterCount * scan->filesystemInfo.clusterSize;

    for (const std::string_view path : {"/repeat.bin", "/hole.bin"}) {
        const RecoveryCandidate* candidate = candidateAt(scan.value(), path);
        ASSERT_NE(candidate, nullptr) << path;
        EXPECT_TRUE(candidate->hasWarning(CandidateWarning::AllocationDamaged)) << describeCandidate(*candidate);
        EXPECT_LE(candidate->bytes(RegionKind::Stored), clusterArea);
        EXPECT_EQ(candidate->bytes(RegionKind::Zeros), 0u);
        std::uint64_t delivered = 0;
        const Result<ReconstructionReport> report = reconstructCandidate(
            source, *candidate, [&](std::uint64_t, std::span<const std::byte> data) {
                delivered += data.size();
                return success();
            });
        RECOVERY_ASSERT_OK(report);
        EXPECT_LE(report->outputSize, clusterArea) << path;
        EXPECT_EQ(delivered, report->storedBytes);
    }
    EXPECT_EQ(candidateAt(scan.value(), "/hole.bin")->bytes(RegionKind::Stored), kClusterSize);
    const RecoveryCandidate* repeat = candidateAt(scan.value(), "/repeat.bin");
    ASSERT_EQ(repeat->sourceRegions.size(), 4u) << describeCandidate(*repeat);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(repeat->sourceRegions[i].sourceOffset, builder.clusterOffset(5000));
    }
}

// ---------------------------------------------------------------------------
// Random mutation
// ---------------------------------------------------------------------------

// Opens a mutated image and checks every candidate and its reconstruction.
// Returns false when the volume did not open.
bool checkMutatedImage(const std::vector<std::byte>& image) {
    test::MemoryStorageSource source(image, 512);
    EXPECT_TRUE(source.open().ok());
    Result<std::unique_ptr<FilesystemRecovery>> recovery = openFilesystemRecovery(source, 0);
    if (!recovery.ok()) {
        const ErrorCode code = recovery.error().code;
        EXPECT_TRUE(code == ErrorCode::UnsupportedFilesystem || code == ErrorCode::CorruptedFilesystem ||
                    code == ErrorCode::IoError)
            << describe(recovery.error());
        return false;
    }
    const Result<CandidateScan> scan = recovery.value()->findCandidates({}, {});
    if (!scan.ok()) {
        EXPECT_NE(scan.error().code, ErrorCode::InternalError) << describe(scan.error());
        return true;
    }
    const filesystem::FilesystemInfo& info = scan->filesystemInfo;
    const std::uint64_t clusterArea =
        checkedMul<std::uint64_t>(info.clusterCount, info.clusterSize).value_or(~0ULL);
    const std::uint64_t volumeEnd = checkedAdd(info.dataOffset, clusterArea).value_or(~0ULL);
    for (const RecoveryCandidate& candidate : scan->candidates) {
        test::expectWellFormed(candidate, 0, volumeEnd);
        std::uint64_t next = 0;
        std::uint64_t delivered = 0;
        const Result<ReconstructionReport> report = reconstructCandidate(
            source, candidate, [&](std::uint64_t offset, std::span<const std::byte> data) {
                EXPECT_GE(offset, next);
                next = offset + data.size();
                delivered += data.size();
                return success();
            });
        EXPECT_TRUE(report.ok()) << (report.ok() ? "" : describe(report.error()));
        if (report.ok()) {
            EXPECT_LE(report->outputSize, std::max<std::uint64_t>(clusterArea, 64 * 1024))
                << describeCandidate(candidate);
            EXPECT_LE(next, report->outputSize);
            EXPECT_EQ(delivered, report->storedBytes + report->embeddedBytes);
            EXPECT_EQ(report->storedBytes + report->unreadableBytes, candidate.bytes(RegionKind::Stored));
        }
    }
    return true;
}

// Mutates `pristine` `iterations` times at offsets chosen by `pick`.
int fuzz(const std::vector<std::byte>& pristine, int iterations, std::uint64_t seed,
         const std::function<std::uint64_t(std::mt19937_64&)>& pick) {
    std::mt19937_64 random(seed);
    int opened = 0;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        std::vector<std::byte> image = pristine;
        const int mutations = 1 + static_cast<int>(random() % 16);
        for (int i = 0; i < mutations; ++i) {
            image[pick(random) % image.size()] = static_cast<std::byte>(random());
        }
        if (checkMutatedImage(image)) {
            ++opened;
        }
        if (::testing::Test::HasFailure()) {
            ADD_FAILURE() << "iteration " << iteration;
            break;
        }
    }
    return opened;
}

TEST(FilesystemRecoveryFuzzTest, Fat32) {
    test::Fat32ImageBuilder builder;
    const auto root = test::Fat32ImageBuilder::root();
    const auto dir = builder.addDirectory(root, "DCIM");
    for (int i = 0; i < 10; ++i) {
        const auto file = builder.addFile(i % 2 == 0 ? dir.clusters.front() : root,
                                          "Picture " + std::to_string(i) + ".jpg",
                                          bytesOf(700 * static_cast<std::size_t>(i + 1), i));
        if (i % 3 == 0) {
            builder.deleteEntry(file);
        }
    }
    (void)builder.addFileInClusters(root, "frag.bin", bytesOf(4 * kClusterSize, 99), {600, 601, 650, 620});
    const std::vector<std::byte> pristine = builder.build();
    const std::uint64_t fat = builder.fatOffset(0);
    const std::uint64_t data = builder.clusterOffset(2);
    const int opened = fuzz(pristine, 150, 0xFA7, [&](std::mt19937_64& random) -> std::uint64_t {
        switch (random() % 4) {
        case 0:
            return random() % 512;  // boot sector
        case 1:
            return fat + random() % 4096;  // FAT entries in use
        default:
            return data + random() % (64 * kClusterSize);  // directories and early data
        }
    });
    EXPECT_GT(opened, 75);
}

TEST(FilesystemRecoveryFuzzTest, ExFat) {
    test::ExFatImageBuilder builder;
    const auto dir = builder.addDirectory(builder.root(), "DCIM");
    for (int i = 0; i < 10; ++i) {
        const auto file = builder.addFile(i % 2 == 0 ? dir.clusters.front() : builder.root(),
                                          "Видео " + std::to_string(i) + ".mp4",
                                          bytesOf(900 * static_cast<std::size_t>(i + 1), i));
        if (i % 3 == 0) {
            builder.deleteEntry(file);
        }
    }
    builder.deleteEntry(builder.addFileInClusters(builder.root(), "frag.bin", bytesOf(4 * kClusterSize, 98),
                                                  {900, 902, 950, 901}));
    const std::vector<std::byte> pristine = builder.build();
    const std::uint64_t fat = builder.fatOffset();
    const std::uint64_t heap = builder.clusterOffset(2);
    const int opened = fuzz(pristine, 150, 0xE7FA7, [&](std::mt19937_64& random) -> std::uint64_t {
        switch (random() % 4) {
        case 0:
            return random() % (12 * 512);  // main boot region
        case 1:
            return fat + random() % 4096;
        default:
            return heap + random() % (64 * kClusterSize);  // bitmap, up-case table, directories
        }
    });
    EXPECT_GT(opened, 50);
}

TEST(FilesystemRecoveryFuzzTest, Ntfs) {
    test::NtfsImageBuilder builder;
    const auto dir = builder.addDirectory(kNtfsRoot, "DCIM", 2);
    for (int i = 0; i < 10; ++i) {
        const auto file = builder.addFile(i % 2 == 0 ? dir.record : kNtfsRoot, "Photo " + std::to_string(i) + ".jpg",
                                          bytesOf(400 * static_cast<std::size_t>(i + 1), i));
        if (i % 3 == 0) {
            builder.deleteEntry(file);
        }
    }
    (void)builder.addFileInClusters(kNtfsRoot, "frag.bin", bytesOf(4 * kClusterSize, 97), {5000, 5001, 5050, 5020});
    (void)builder.addFileWithRuns(kNtfsRoot, "sparse.bin", bytesOf(5 * kClusterSize, 96),
                                  {{3000, 2}, {std::nullopt, 2}, {3100, 1}}, 0x8000);
    const std::vector<std::byte> pristine = builder.build();
    const std::uint64_t mft = builder.recordOffset(0);
    const std::uint64_t userRecords = builder.recordOffset(64);
    const int opened = fuzz(pristine, 150, 0x47F5, [&](std::mt19937_64& random) -> std::uint64_t {
        switch (random() % 4) {
        case 0:
            return random() % 512;  // boot sector
        case 1:
            return mft + random() % (16 * 1024);  // system records
        default:
            return userRecords + random() % (24 * 1024);  // user records
        }
    });
    EXPECT_GT(opened, 75);
}

}  // namespace
}  // namespace recovery
