// exFAT metadata corruption: every traversal must terminate, every reported
// extent must stay inside the volume, and every inconsistency must be named.

#include "filesystem/exfat/exfat_filesystem.hpp"

#include "recovery/byte_order.hpp"
#include "support/exfat_builder.hpp"
#include "support/fat32_builder.hpp"  // readExtents
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <random>

namespace recovery::filesystem::exfat {
namespace {

using test::ExFatImageBuilder;

constexpr std::uint32_t kEoc = 0xFFFFFFFF;

struct Mounted {
    std::unique_ptr<test::MemoryStorageSource> source;
    std::unique_ptr<ExFatFilesystem> fs;
};

Mounted mount(std::vector<std::byte> image) {
    Mounted m;
    m.source = std::make_unique<test::MemoryStorageSource>(std::move(image), 512);
    EXPECT_TRUE(m.source->open().ok());
    Result<std::unique_ptr<ExFatFilesystem>> fs = ExFatFilesystem::open(*m.source);
    EXPECT_TRUE(fs.ok());
    if (fs.ok()) {
        m.fs = std::move(fs).value();
    }
    return m;
}

FileScan scanOf(const Mounted& m) {
    Result<FileScan> scan = m.fs->scan({}, {});
    EXPECT_TRUE(scan.ok());
    return scan.ok() ? std::move(scan).value() : FileScan{};
}

const FileRecord& record(const FileScan& scan, std::string_view name) {
    for (const FileRecord& r : scan.records) {
        if (r.entry.name == name) {
            return r;
        }
    }
    throw std::runtime_error("record not found: " + std::string(name));
}

bool hasRecord(const FileScan& scan, std::string_view name) {
    return std::any_of(scan.records.begin(), scan.records.end(),
                       [&](const FileRecord& r) { return r.entry.name == name; });
}

bool hasIssueContaining(const FileScan& scan, std::string_view text) {
    return std::any_of(scan.issues.begin(), scan.issues.end(),
                       [&](const ScanIssue& i) { return i.detail.find(text) != std::string::npos; });
}

// ---------------------------------------------------------------------------
// FAT chains of a fragmented (FAT-chained) file.
// ---------------------------------------------------------------------------

class ExFatChainTest : public ::testing::Test {
protected:
    void SetUp() override {
        file_ = builder_.addFileInClusters(builder_.root(), "file.bin", data_, {300, 301, 302, 400, 401});
    }

    const FileRecord& scanned() {
        mounted_ = mount(builder_.build());
        scan_ = scanOf(mounted_);
        return record(scan_, "file.bin");
    }

    ExFatImageBuilder builder_;
    std::vector<std::byte> data_ = test::makePattern(5 * 512, 3);  // 5 clusters
    ExFatImageBuilder::Entry file_;
    Mounted mounted_;
    FileScan scan_;
};

TEST_F(ExFatChainTest, LoopIsDetectedAndWalkTerminates) {
    builder_.setFat(400, 301);  // 301 -> 302 -> 400 -> 301 ...
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainLoop));
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    EXPECT_EQ(r.allocation.clusterCount, 4u);
}

TEST_F(ExFatChainTest, SelfLoop) {
    builder_.setFat(300, 300);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainLoop));
    EXPECT_EQ(r.allocation.clusterCount, 1u);
}

TEST_F(ExFatChainTest, ZeroEntryInChain) {
    builder_.setFat(301, 0);  // not a valid exFAT FAT value
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::InvalidClusterInChain));
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    EXPECT_EQ(r.allocation.dataBytes(), 2u * 512u);
}

TEST_F(ExFatChainTest, OutOfRangeClusterInChain) {
    builder_.setFat(302, 0x00FFFF00);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::InvalidClusterInChain));
    EXPECT_EQ(r.allocation.clusterCount, 3u);
}

TEST_F(ExFatChainTest, BadClusterMarkerInChain) {
    builder_.setFat(302, 0xFFFFFFF7);
    EXPECT_TRUE(scanned().allocation.hasIssue(AllocationIssue::BadClusterInChain));
}

TEST_F(ExFatChainTest, TruncatedChain) {
    builder_.setFat(302, kEoc);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    EXPECT_EQ(r.allocation.dataBytes(), 3u * 512u);
    const auto partial = test::readExtents(*mounted_.source, r.allocation.extents);
    EXPECT_TRUE(std::equal(partial.begin(), partial.end(), data_.begin()));
}

TEST_F(ExFatChainTest, ChainLongerThanRecordedSize) {
    storeLe64(builder_.slot(file_, 1), 24, 2 * 512);
    storeLe64(builder_.slot(file_, 1), 8, 2 * 512);
    builder_.rechecksum(file_);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainLongerThanSize));
    EXPECT_EQ(r.allocation.clusterCount, 2u);
}

TEST_F(ExFatChainTest, InvalidStartCluster) {
    storeLe32(builder_.slot(file_, 1), 20, 0x0FFFFFFF);
    builder_.rechecksum(file_);
    const FileRecord& r = scanned();
    EXPECT_EQ(r.allocation.method, AllocationMethod::None);
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::InvalidStartCluster));
}

TEST_F(ExFatChainTest, SizeLargerThanVolumeIsBounded) {
    storeLe64(builder_.slot(file_, 1), 24, 0xFFFFFFFFFFFFULL);
    builder_.rechecksum(file_);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::SizeExceedsVolume));
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    EXPECT_EQ(r.allocation.clusterCount, 5u);
}

TEST_F(ExFatChainTest, ClusterMarkedFreeInBitmap) {
    builder_.setAllocated(302, false);
    EXPECT_TRUE(scanned().allocation.hasIssue(AllocationIssue::ClustersMarkedFree));
}

TEST_F(ExFatChainTest, CrossLinkedFiles) {
    const auto other =
        builder_.addFileInClusters(builder_.root(), "other.bin", test::makePattern(3 * 512, 4), {500, 501, 502});
    builder_.setFat(501, 302);  // other.bin merges into file.bin's chain
    storeLe64(builder_.slot(other, 1), 24, 5 * 512);
    storeLe64(builder_.slot(other, 1), 8, 5 * 512);
    builder_.rechecksum(other);
    (void)scanned();
    EXPECT_TRUE(record(scan_, "file.bin").allocation.hasIssue(AllocationIssue::CrossLinked));
    EXPECT_TRUE(record(scan_, "other.bin").allocation.hasIssue(AllocationIssue::CrossLinked));
    EXPECT_EQ(scan_.crossLinkedClusters, 3u);  // 302, 400, 401
}

// ---------------------------------------------------------------------------
// Contiguous (NoFatChain) runs.
// ---------------------------------------------------------------------------

TEST(ExFatRunTest, OverlappingRunsAreCrossLinked) {
    ExFatImageBuilder builder;
    const auto a = builder.addFile(builder.root(), "a.bin", test::makePattern(4 * 512, 1));
    const auto b = builder.addFile(builder.root(), "b.bin", test::makePattern(4 * 512, 2));
    storeLe32(builder.slot(b, 1), 20, a.clusters[2]);  // b now starts inside a
    builder.rechecksum(b);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_TRUE(record(scan, "a.bin").allocation.hasIssue(AllocationIssue::CrossLinked));
    EXPECT_TRUE(record(scan, "b.bin").allocation.hasIssue(AllocationIssue::CrossLinked));
    EXPECT_EQ(scan.crossLinkedClusters, 2u);
}

TEST(ExFatRunTest, RunIntoSystemStructuresIsCrossLinked) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(512, 1));
    storeLe32(builder.slot(file, 1), 20, builder.bitmapCluster());
    builder.rechecksum(file);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_TRUE(record(scan, "a.bin").allocation.hasIssue(AllocationIssue::CrossLinked));
}

TEST(ExFatRunTest, RunBeyondTheHeapIsClipped) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(512, 1));
    storeLe32(builder.slot(file, 1), 20, builder.lastCluster() - 1);
    storeLe64(builder.slot(file, 1), 24, 10 * 512);
    storeLe64(builder.slot(file, 1), 8, 10 * 512);
    builder.rechecksum(file);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    const FileRecord& r = record(scan, "a.bin");
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::BeyondVolume));
    EXPECT_EQ(r.allocation.clusterCount, 2u);
    EXPECT_EQ(r.allocation.dataBytes(), 2u * 512u);
}

TEST(ExFatRunTest, ActiveRunMarkedFree) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(3 * 512, 1));
    builder.setAllocated(file.clusters[1], false);
    const Mounted m = mount(builder.build());
    EXPECT_TRUE(record(scanOf(m), "a.bin").allocation.hasIssue(AllocationIssue::ClustersMarkedFree));
}

TEST(ExFatRunTest, EmptyFileWithClusters) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", {});
    storeLe32(builder.slot(file, 1), 20, 1000);
    builder.rechecksum(file);
    const Mounted m = mount(builder.build());
    EXPECT_TRUE(record(scanOf(m), "a.bin").allocation.hasIssue(AllocationIssue::ChainLongerThanSize));
}

// ---------------------------------------------------------------------------
// Damaged entry sets.
// ---------------------------------------------------------------------------

TEST(ExFatEntrySetTest, ChecksumMismatchIsFlaggedButListed) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(100, 1));
    builder.slot(file, 0)[2] ^= std::byte{0xFF};
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    const FileRecord& r = record(scan, "a.bin");
    EXPECT_TRUE(r.entry.hasIssue(EntryIssue::EntrySetChecksumMismatch));
    EXPECT_EQ(test::readExtents(*m.source, r.allocation.extents), test::makePattern(100, 1));
}

TEST(ExFatEntrySetTest, NameHashMismatchIsFlagged) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(100, 1));
    builder.slot(file, 1)[4] ^= std::byte{0x01};
    builder.rechecksum(file);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_TRUE(record(scan, "a.bin").entry.hasIssue(EntryIssue::NameHashMismatch));
}

TEST(ExFatEntrySetTest, SecondaryCountTooLargeStillListsTheFile) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(100, 1));
    (void)builder.addFile(builder.root(), "b.bin", test::makePattern(100, 2));
    builder.slot(file, 0)[1] = std::byte{5};  // claims three more entries than it has
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    ASSERT_EQ(scan.records.size(), 2u);
    EXPECT_TRUE(record(scan, "a.bin").entry.hasIssue(EntryIssue::EntrySetChecksumMismatch));
    EXPECT_TRUE(record(scan, "b.bin").entry.issues.empty());
}

TEST(ExFatEntrySetTest, SecondaryCountTooSmallDropsTheSet) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "A name longer than fifteen.bin", test::makePattern(100, 1));
    (void)builder.addFile(builder.root(), "b.bin", test::makePattern(100, 2));
    builder.slot(file, 0)[1] = std::byte{2};  // stream and one name entry: too few for 30 characters
    builder.rechecksum(file);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_FALSE(hasRecord(scan, "A name longer than fifteen.bin"));
    EXPECT_TRUE(hasIssueContaining(scan, "too few entries"));
    EXPECT_TRUE(hasRecord(scan, "b.bin"));
}

TEST(ExFatEntrySetTest, InvalidSecondaryCountIsReported) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(100, 1));
    builder.slot(file, 0)[1] = std::byte{19};
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_FALSE(hasRecord(scan, "a.bin"));
    EXPECT_TRUE(hasIssueContaining(scan, "invalid secondary count 19"));
}

TEST(ExFatEntrySetTest, MissingStreamExtension) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(100, 1));
    builder.slot(file, 1)[0] = std::byte{0xC1};  // a name entry where the stream belongs
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_FALSE(hasRecord(scan, "a.bin"));
    EXPECT_TRUE(hasIssueContaining(scan, "no stream extension"));
}

TEST(ExFatEntrySetTest, ZeroNameLength) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(100, 1));
    builder.slot(file, 1)[3] = std::byte{0};
    builder.rechecksum(file);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_TRUE(scan.records.empty());
    EXPECT_TRUE(hasIssueContaining(scan, "name length is zero"));
}

TEST(ExFatEntrySetTest, UnknownCriticalEntries) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(100, 1), 1);
    builder.slot(file, 3)[0] = std::byte{0xC5};  // a critical secondary nobody knows
    builder.rechecksum(file);
    (void)builder.addFile(builder.root(), "b.bin", test::makePattern(100, 2));
    const std::size_t slot = builder.nextFreeSlot(builder.root());
    builder.directorySlot(builder.root(), slot)[0] = std::byte{0x8F};  // an unknown critical primary
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_FALSE(hasRecord(scan, "a.bin"));
    EXPECT_TRUE(hasIssueContaining(scan, "unknown critical secondary entry 0xC5"));
    EXPECT_TRUE(hasIssueContaining(scan, "unknown critical entry type 0x8F"));
    EXPECT_TRUE(hasRecord(scan, "b.bin"));
}

TEST(ExFatEntrySetTest, InvalidNameCharactersAreFlagged) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a?b", test::makePattern(10, 1));
    const auto dots = builder.addFile(builder.root(), "..", test::makePattern(10, 2));
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_TRUE(record(scan, "a?b").entry.hasIssue(EntryIssue::InvalidName));
    EXPECT_TRUE(record(scan, "..").entry.hasIssue(EntryIssue::InvalidName));
    (void)file;
    (void)dots;
}

TEST(ExFatEntrySetTest, ValidDataLengthBeyondSizeIsFlagged) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(100, 1));
    storeLe64(builder.slot(file, 1), 8, 5000);
    builder.rechecksum(file);
    const Mounted m = mount(builder.build());
    EXPECT_TRUE(record(scanOf(m), "a.bin").entry.hasIssue(EntryIssue::ValidDataLengthExceedsSize));
}

TEST(ExFatEntrySetTest, ReservedAttributeBitsAreFlagged) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(100, 1));
    storeLe16(builder.slot(file, 0), 4, 0x0120);
    builder.rechecksum(file);
    const Mounted m = mount(builder.build());
    EXPECT_TRUE(record(scanOf(m), "a.bin").entry.hasIssue(EntryIssue::ReservedAttributeBits));
}

TEST(ExFatEntrySetTest, DeletedSetWithBadChecksumIsDropped) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(100, 1));
    builder.deleteEntry(file);
    builder.slot(file, 0)[8] ^= std::byte{0x01};  // timestamp changed after the checksum
    builder.slot(file, 1)[4] ^= std::byte{0x01};  // and the name hash: no orphan match either
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_TRUE(scan.records.empty());
    EXPECT_TRUE(scan.issues.empty());  // deleted leftovers are not errors
}

TEST(ExFatEntrySetTest, DeletedSetWithBadChecksumButMatchingHashIsAnOrphan) {
    ExFatImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "a.bin", test::makePattern(100, 1));
    builder.deleteEntry(file);
    builder.slot(file, 0)[8] ^= std::byte{0x01};  // the File entry no longer matches the set
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    ASSERT_EQ(scan.records.size(), 1u);
    EXPECT_TRUE(scan.records[0].entry.hasIssue(EntryIssue::MetadataIncomplete));
    EXPECT_EQ(test::readExtents(*m.source, scan.records[0].allocation.extents), test::makePattern(100, 1));
}

TEST(ExFatEntrySetTest, GarbageDeletedEntriesAreIgnored) {
    ExFatImageBuilder builder;
    (void)builder.addFile(builder.root(), "real.bin", test::makePattern(10, 1));
    std::mt19937_64 random(0xE5);
    const std::size_t first = builder.nextFreeSlot(builder.root());
    for (std::size_t i = 0; i < 200; ++i) {
        const std::span<std::byte> slot = builder.directorySlot(builder.root(), first + i);
        for (std::byte& b : slot) {
            b = static_cast<std::byte>(random());
        }
        // Deleted File, Stream and Name types with random contents.
        constexpr std::array<std::uint8_t, 4> kTypes = {0x05, 0x40, 0x41, 0x41};
        slot[0] = static_cast<std::byte>(kTypes[i % kTypes.size()]);
    }
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    ASSERT_EQ(scan.records.size(), 1u);
    EXPECT_EQ(scan.records[0].entry.name, "real.bin");
}

TEST(ExFatEntrySetTest, InvalidLabelLengthIsReported) {
    ExFatImageBuilder builder;
    std::vector<std::byte> image = builder.build();
    image[static_cast<std::size_t>(builder.clusterOffset(builder.root())) + 1] = std::byte{12};
    const Mounted m = mount(std::move(image));
    ASSERT_NE(m.fs, nullptr);
    EXPECT_EQ(m.fs->info().label, "");
    EXPECT_TRUE(hasIssueContaining(scanOf(m), "volume label"));
}

// ---------------------------------------------------------------------------
// Directory structure.
// ---------------------------------------------------------------------------

TEST(ExFatDirectoryTest, SubdirectoryPointingToRootIsALoop) {
    ExFatImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "loop");
    (void)builder.addFile(builder.root(), "a.txt", test::makePattern(10, 1));
    storeLe32(builder.slot(folder, 1), 20, builder.root());
    builder.rechecksum(folder);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    ASSERT_EQ(scan.issues.size(), 1u);
    EXPECT_EQ(scan.issues[0].kind, ScanIssueKind::DirectoryLoop);
    EXPECT_EQ(scan.records.size(), 2u);
}

TEST(ExFatDirectoryTest, SubdirectoryPointingToAncestorIsALoop) {
    ExFatImageBuilder builder;
    const auto a = builder.addDirectory(builder.root(), "A");
    const auto b = builder.addDirectory(a.clusters[0], "B");
    storeLe32(builder.slot(b, 1), 20, a.clusters[0]);
    builder.rechecksum(b);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    ASSERT_FALSE(scan.issues.empty());
    EXPECT_EQ(scan.issues[0].kind, ScanIssueKind::DirectoryLoop);
    EXPECT_EQ(scan.issues[0].path, "/A/B");
}

TEST(ExFatDirectoryTest, RootChainLoop) {
    ExFatImageBuilder builder;
    for (int i = 0; i < 20; ++i) {
        (void)builder.addFile(builder.root(), "file " + std::to_string(i), {});
    }
    // The root now spans more than one cluster; make the second point back to the first.
    const std::uint32_t second = builder.fat(builder.root());
    ASSERT_NE(second, kEoc);
    builder.setFat(second, builder.root());
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    // Only the two clusters before the loop are read: 32 entries, of which 4
    // are system entries and 28 hold nine 3-entry sets and a cut-off tenth.
    EXPECT_EQ(scan.records.size(), 9u);
    EXPECT_TRUE(hasIssueContaining(scan, "ChainLoop"));
    EXPECT_TRUE(hasIssueContaining(scan, "incomplete entry set"));
}

TEST(ExFatDirectoryTest, DirectoryWithoutClusters) {
    ExFatImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "empty dir");
    storeLe64(builder.slot(folder, 1), 24, 0);
    storeLe64(builder.slot(folder, 1), 8, 0);
    builder.rechecksum(folder);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_TRUE(hasIssueContaining(scan, "directory has no clusters"));
}

TEST(ExFatDirectoryTest, OversizedDirectoryIsCapped) {
    ExFatImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "huge");
    storeLe64(builder.slot(folder, 1), 24, 1ULL << 40);
    storeLe64(builder.slot(folder, 1), 8, 1ULL << 40);
    builder.rechecksum(folder);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_TRUE(hasIssueContaining(scan, "exceeds the 256 MiB limit"));
    EXPECT_LE(record(scan, "huge").allocation.dataBytes(), 256ULL * 1024 * 1024);
}

// ---------------------------------------------------------------------------
// Hostile boot sectors.
// ---------------------------------------------------------------------------

TEST(ExFatHostileTest, HugeClaimedVolumeOnATinyImage) {
    ExFatImageBuilder builder;
    (void)builder.addFile(builder.root(), "a.bin", test::makePattern(1000, 1));
    std::vector<std::byte> image = builder.build();
    // Claim ~2^32 clusters with a matching FAT and volume length; the image stays 2 MiB.
    storeLe64(image, 72, 1ULL << 42);
    storeLe32(image, 84, 0x08000000);
    storeLe32(image, 88, 0x08000000 + 32);
    storeLe32(image, 92, 0xFFFFFFF5);
    test::ExFatImageBuilder::rewriteBootChecksum(image, 512, 0);
    test::MemoryStorageSource source(image, 512);
    RECOVERY_ASSERT_OK(source.open());
    Result<std::unique_ptr<ExFatFilesystem>> fs = ExFatFilesystem::open(source);
    if (!fs.ok()) {
        EXPECT_EQ(fs.error().code, ErrorCode::CorruptedFilesystem);
        return;
    }
    // Whatever is found, every operation is bounded by what the image holds.
    RECOVERY_EXPECT_OK(fs.value()->scan({}, {}));
    RECOVERY_EXPECT_OK(fs.value()->analyzeClusters({}));
    EXPECT_LT(source.readCount(), 200000u);
}

// ---------------------------------------------------------------------------
// Randomised corruption of the boot region, FAT, bitmap and directories.
// ---------------------------------------------------------------------------

std::vector<std::byte> richImage(ExFatImageBuilder& builder) {
    const auto dcim = builder.addDirectory(builder.root(), "DCIM");
    const auto camera = builder.addDirectory(dcim.clusters[0], "100 Camera");
    for (int i = 0; i < 12; ++i) {
        const auto file = builder.addFile(i % 2 == 0 ? camera.clusters[0] : builder.root(),
                                          "Picture number " + std::to_string(i) + ".jpg",
                                          test::makePattern(700 * static_cast<std::size_t>(i + 1), i));
        if (i % 3 == 0) {
            builder.deleteEntry(file);
        }
    }
    (void)builder.addFileInClusters(builder.root(), "frag.bin", test::makePattern(4 * 512, 9), {300, 301, 350, 320});
    const auto old = builder.addDirectory(builder.root(), "Old stuff");
    (void)builder.addFile(old.clusters[0], "note.txt", test::makePattern(900, 5));
    builder.deleteEntry(old);
    return builder.build();
}

void checkAllocationInvariants(const FileScan& scan, const FilesystemInfo& info) {
    const std::uint64_t dataEnd = info.dataOffset + info.clusterCount * info.clusterSize;
    for (const FileRecord& r : scan.records) {
        for (const Extent& extent : r.allocation.extents) {
            ASSERT_GE(extent.offset, info.dataOffset) << r.path;
            ASSERT_LE(extent.length, dataEnd - extent.offset) << r.path;
        }
        ASSERT_LE(r.allocation.clusterCount, info.clusterCount) << r.path;
    }
}

TEST(ExFatFuzzTest, RandomCorruptionNeverEscapesTheVolume) {
    ExFatImageBuilder builder;
    const std::vector<std::byte> pristine = richImage(builder);
    const std::uint64_t bootEnd = 24 * 512;
    const std::uint64_t fatStart = builder.fatOffset();
    const std::uint64_t fatEnd = builder.clusterOffset(2);
    const std::uint64_t heapStart = builder.clusterOffset(2);
    const std::uint64_t heapEnd = builder.clusterOffset(400);

    std::mt19937_64 random(0xE7FA7);
    int opened = 0;
    for (int iteration = 0; iteration < 400; ++iteration) {
        std::vector<std::byte> image = pristine;
        const int mutations = 1 + static_cast<int>(random() % 24);
        bool bootTouched = false;
        for (int i = 0; i < mutations; ++i) {
            std::uint64_t at = 0;
            switch (random() % 5) {
            case 0:
                at = random() % bootEnd;  // boot regions
                bootTouched = true;
                break;
            case 1:
                at = fatStart + random() % (fatEnd - fatStart);
                break;
            default:
                at = heapStart + random() % (heapEnd - heapStart);  // bitmap, up-case table, directories
                break;
            }
            image[at] = static_cast<std::byte>(random());
        }
        if (bootTouched && random() % 2 == 0) {
            test::ExFatImageBuilder::rewriteBootChecksum(image, 512, 0);  // make the damage look valid
        }

        test::MemoryStorageSource source(image, 512);
        ASSERT_TRUE(source.open().ok());
        Result<std::unique_ptr<ExFatFilesystem>> fs = ExFatFilesystem::open(source);
        if (!fs.ok()) {
            ASSERT_TRUE(fs.error().code == ErrorCode::UnsupportedFilesystem ||
                        fs.error().code == ErrorCode::CorruptedFilesystem)
                << describe(fs.error());
            continue;
        }
        ++opened;
        const Result<FileScan> scan = fs.value()->scan({}, {});
        ASSERT_TRUE(scan.ok()) << describe(scan.error());
        checkAllocationInvariants(scan.value(), fs.value()->info());
        ASSERT_TRUE(fs.value()->analyzeClusters({}).ok());
        if (HasFatalFailure()) {
            FAIL() << "iteration " << iteration;
        }
    }
    EXPECT_GT(opened, 300) << "most mutations should leave the volume mountable (the backup boot region helps)";
}

}  // namespace
}  // namespace recovery::filesystem::exfat
