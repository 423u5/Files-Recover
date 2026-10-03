// FAT32 metadata corruption: every traversal must terminate, every reported
// extent must stay inside the volume, and every inconsistency must be named.

#include "filesystem/fat32/fat32_filesystem.hpp"

#include "recovery/byte_order.hpp"
#include "support/fat32_builder.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <random>

namespace recovery::filesystem::fat32 {
namespace {

using test::Fat32ImageBuilder;

constexpr std::uint32_t kEoc = 0x0FFFFFFF;

struct Mounted {
    std::unique_ptr<test::MemoryStorageSource> source;
    std::unique_ptr<Fat32Filesystem> fs;
};

Mounted mount(std::vector<std::byte> image) {
    Mounted m;
    m.source = std::make_unique<test::MemoryStorageSource>(std::move(image), 512);
    EXPECT_TRUE(m.source->open().ok());
    Result<std::unique_ptr<Fat32Filesystem>> fs = Fat32Filesystem::open(*m.source);
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

class Fat32ChainTest : public ::testing::Test {
protected:
    void SetUp() override { file_ = builder_.addFile(builder_.root(), "FILE.BIN", data_); }

    const FileRecord& scanned() {
        mounted_ = mount(builder_.build());
        scan_ = scanOf(mounted_);
        return record(scan_, "FILE.BIN");
    }

    Fat32ImageBuilder builder_;
    std::vector<std::byte> data_ = test::makePattern(5 * 512, 3);  // 5 clusters
    Fat32ImageBuilder::Entry file_;
    Mounted mounted_;
    FileScan scan_;
};

TEST_F(Fat32ChainTest, LoopIsDetectedAndWalkTerminates) {
    builder_.setFat(file_.clusters[3], file_.clusters[1]);  // c3 -> c1 -> c2 -> c3 ...
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainLoop));
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    EXPECT_EQ(r.allocation.clusterCount, 4u);
}

TEST_F(Fat32ChainTest, SelfLoop) {
    builder_.setFat(file_.clusters[0], file_.clusters[0]);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainLoop));
    EXPECT_EQ(r.allocation.clusterCount, 1u);
}

TEST_F(Fat32ChainTest, FreeClusterInChain) {
    builder_.setFat(file_.clusters[1], 0);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::FreeClusterInChain));
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    EXPECT_EQ(r.allocation.dataBytes(), 2u * 512u);
}

TEST_F(Fat32ChainTest, OutOfRangeClusterInChain) {
    builder_.setFat(file_.clusters[2], 0x0FFFFF00);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::InvalidClusterInChain));
    EXPECT_EQ(r.allocation.clusterCount, 3u);
}

TEST_F(Fat32ChainTest, ReservedValueInChain) {
    builder_.setFat(file_.clusters[0], 1);
    EXPECT_TRUE(scanned().allocation.hasIssue(AllocationIssue::InvalidClusterInChain));
}

TEST_F(Fat32ChainTest, BadClusterMarkerInChain) {
    builder_.setFat(file_.clusters[2], 0x0FFFFFF7);
    EXPECT_TRUE(scanned().allocation.hasIssue(AllocationIssue::BadClusterInChain));
}

TEST_F(Fat32ChainTest, TruncatedChain) {
    builder_.setFat(file_.clusters[2], kEoc);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    EXPECT_EQ(r.allocation.dataBytes(), 3u * 512u);
    const auto partial = test::readExtents(*mounted_.source, r.allocation.extents);
    EXPECT_TRUE(std::equal(partial.begin(), partial.end(), data_.begin()));
}

TEST_F(Fat32ChainTest, ChainLongerThanRecordedSize) {
    storeLe32(builder_.shortEntry(file_), 28, 2 * 512);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainLongerThanSize));
    EXPECT_EQ(r.allocation.clusterCount, 2u);
}

TEST_F(Fat32ChainTest, InvalidStartCluster) {
    storeLe16(builder_.shortEntry(file_), 20, 0x0FFF);
    storeLe16(builder_.shortEntry(file_), 26, 0xFFFF);
    const FileRecord& r = scanned();
    EXPECT_EQ(r.allocation.method, AllocationMethod::None);
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::InvalidStartCluster));
}

TEST_F(Fat32ChainTest, SizeLargerThanVolumeIsBounded) {
    storeLe32(builder_.shortEntry(file_), 28, 0xFFFFFFFF);
    const FileRecord& r = scanned();
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::SizeExceedsVolume));
    EXPECT_TRUE(r.allocation.hasIssue(AllocationIssue::ChainShorterThanSize));
    EXPECT_EQ(r.allocation.clusterCount, 5u);
}

TEST_F(Fat32ChainTest, CrossLinkedFiles) {
    const auto other = builder_.addFile(builder_.root(), "OTHER.BIN", test::makePattern(3 * 512, 4));
    builder_.setFat(other.clusters[1], file_.clusters[2]);  // OTHER merges into FILE's chain
    storeLe32(builder_.shortEntry(other), 28, 5 * 512);
    (void)scanned();
    EXPECT_TRUE(record(scan_, "FILE.BIN").allocation.hasIssue(AllocationIssue::CrossLinked));
    EXPECT_TRUE(record(scan_, "OTHER.BIN").allocation.hasIssue(AllocationIssue::CrossLinked));
    EXPECT_EQ(scan_.crossLinkedClusters, 3u);
}

TEST_F(Fat32ChainTest, SharedStartCluster) {
    const auto twin = builder_.addFile(builder_.root(), "TWIN.BIN", {});
    storeLe16(builder_.shortEntry(twin), 26, static_cast<std::uint16_t>(file_.clusters[0]));
    storeLe32(builder_.shortEntry(twin), 28, 512);
    (void)scanned();
    EXPECT_TRUE(record(scan_, "TWIN.BIN").allocation.hasIssue(AllocationIssue::CrossLinked));
    EXPECT_TRUE(record(scan_, "FILE.BIN").allocation.hasIssue(AllocationIssue::CrossLinked));
}

TEST(Fat32CorruptionTest, FatCopiesThatDisagree) {
    Fat32ImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "A.BIN", test::makePattern(2048, 1));
    builder.setFatInCopy(1, file.clusters[1], 0);
    builder.setFatInCopy(1, 900, 0x0FFFFFF7);
    const Mounted m = mount(builder.build());
    const Result<ClusterUsage> usage = m.fs->analyzeClusters({});
    RECOVERY_ASSERT_OK(usage);
    EXPECT_EQ(usage->mirrorMismatches, 2u);
    // The active (first) copy is authoritative for chains.
    EXPECT_TRUE(record(scanOf(m), "A.BIN").allocation.issues.empty());
}

TEST(Fat32CorruptionTest, SubdirectoryPointingToRootIsALoop) {
    Fat32ImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "LOOP");
    (void)builder.addFile(builder.root(), "A.TXT", test::makePattern(10, 1));
    storeLe16(builder.shortEntry(folder), 26, static_cast<std::uint16_t>(builder.root()));
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    ASSERT_EQ(scan.issues.size(), 1u);
    EXPECT_EQ(scan.issues[0].kind, ScanIssueKind::DirectoryLoop);
    EXPECT_EQ(scan.records.size(), 2u);
}

TEST(Fat32CorruptionTest, SubdirectoryPointingToAncestorIsALoop) {
    Fat32ImageBuilder builder;
    const auto a = builder.addDirectory(builder.root(), "A");
    const auto b = builder.addDirectory(a.clusters[0], "B");
    storeLe16(builder.shortEntry(b), 26, static_cast<std::uint16_t>(a.clusters[0]));
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    ASSERT_EQ(scan.issues.size(), 1u);
    EXPECT_EQ(scan.issues[0].kind, ScanIssueKind::DirectoryLoop);
    EXPECT_EQ(scan.issues[0].path, "/A/B");
}

TEST(Fat32CorruptionTest, DirectoryChainLoop) {
    Fat32ImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "BIG");
    for (int i = 0; i < 20; ++i) {
        (void)builder.addFile(folder.clusters[0], "F" + std::to_string(i) + ".TXT", {});
    }
    // The folder now spans two clusters; make the second point back to the first.
    const std::uint32_t second = builder.fat(folder.clusters[0]);
    builder.setFat(second, folder.clusters[0]);
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    EXPECT_EQ(scan.records.size(), 21u);
    EXPECT_FALSE(scan.issues.empty());
    EXPECT_EQ(scan.issues[0].kind, ScanIssueKind::DirectoryInvalid);
}

TEST(Fat32CorruptionTest, LongNameChecksumMismatchFallsBackToShortName) {
    Fat32ImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "Long file name.txt", test::makePattern(10, 1));
    builder.shortEntry(file)[1] = std::byte{'X'};  // short name changed, checksum no longer matches
    const Mounted m = mount(builder.build());
    const FileScan scanned = scanOf(m);
    const FileRecord& r = scanned.records.at(0);
    EXPECT_EQ(r.entry.name, "LXNGFI~1.TXT");
    EXPECT_TRUE(r.entry.hasIssue(EntryIssue::LongNameChecksumMismatch));
}

TEST(Fat32CorruptionTest, BrokenLongNameSequenceIsIgnored) {
    Fat32ImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "A rather long name.txt", test::makePattern(10, 1));
    // Two long-name slots; clear the "last part" flag of the first one.
    const std::uint64_t firstSlot = builder.shortEntryOffset(file) - 2 * 32;
    builder.raw()[firstSlot] &= std::byte{0x1F};
    const Mounted m = mount(builder.build());
    const FileScan scanned = scanOf(m);
    const FileRecord& r = scanned.records.at(0);
    EXPECT_EQ(r.entry.name, r.entry.shortName);
}

TEST(Fat32CorruptionTest, InvalidShortNameAndAttributesAreFlagged) {
    Fat32ImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "OK.TXT", test::makePattern(10, 1));
    builder.shortEntry(file)[2] = std::byte{'*'};
    builder.shortEntry(file)[11] = std::byte{0xE0};
    const Mounted m = mount(builder.build());
    const FileScan scanned = scanOf(m);
    const DirectoryEntry& e = scanned.records.at(0).entry;
    EXPECT_TRUE(e.hasIssue(EntryIssue::InvalidShortName));
    EXPECT_TRUE(e.hasIssue(EntryIssue::ReservedAttributeBits));
}

TEST(Fat32CorruptionTest, GarbageDeletedSlotsAreIgnored) {
    Fat32ImageBuilder builder;
    const auto file = builder.addFile(builder.root(), "REAL.TXT", test::makePattern(10, 1));
    // Deleted slot full of noise after the real entry.
    std::span<std::byte> slot = builder.raw();
    const std::uint64_t offset = builder.shortEntryOffset(file) + 32;
    auto noise = test::makePattern(32, 77);
    noise[0] = std::byte{0xE5};
    noise[1] = std::byte{0x01};  // control character: not a name
    std::copy(noise.begin(), noise.end(), slot.begin() + static_cast<std::ptrdiff_t>(offset));
    const Mounted m = mount(builder.build());
    const FileScan scan = scanOf(m);
    ASSERT_EQ(scan.records.size(), 1u);
    EXPECT_EQ(scan.records[0].entry.name, "REAL.TXT");
}

TEST(Fat32CorruptionTest, DirectoryWithSizeIsFlagged) {
    Fat32ImageBuilder builder;
    const auto folder = builder.addDirectory(builder.root(), "DIR");
    storeLe32(builder.shortEntry(folder), 28, 4096);
    const Mounted m = mount(builder.build());
    EXPECT_TRUE(scanOf(m).records.at(0).entry.hasIssue(EntryIssue::DirectoryWithSize));
}

// ---------------------------------------------------------------------------
// Randomised corruption of FATs, directories and the boot sector.
// ---------------------------------------------------------------------------

std::vector<std::byte> richImage(Fat32ImageBuilder& builder) {
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
    (void)builder.addFileInClusters(builder.root(), "FRAG.BIN", test::makePattern(4 * 512, 9), {300, 301, 350, 320});
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

TEST(Fat32FuzzTest, RandomCorruptionNeverEscapesTheVolume) {
    Fat32ImageBuilder builder;
    const std::vector<std::byte> pristine = richImage(builder);
    const std::uint64_t fatStart = builder.fatOffset(0);
    const std::uint64_t fatEnd = builder.fatOffset(2);
    const std::uint64_t dirStart = builder.clusterOffset(2);
    const std::uint64_t dirEnd = builder.clusterOffset(400);

    std::mt19937_64 random(0xFA7);
    int opened = 0;
    for (int iteration = 0; iteration < 400; ++iteration) {
        std::vector<std::byte> image = pristine;
        const int mutations = 1 + static_cast<int>(random() % 24);
        for (int i = 0; i < mutations; ++i) {
            std::uint64_t at = 0;
            switch (random() % 4) {
            case 0:
                at = random() % 512;  // boot sector
                break;
            case 1:
            case 2:
                at = fatStart + random() % (fatEnd - fatStart);
                break;
            default:
                at = dirStart + random() % (dirEnd - dirStart);
                break;
            }
            image[at] = static_cast<std::byte>(random());
        }

        test::MemoryStorageSource source(image, 512);
        ASSERT_TRUE(source.open().ok());
        Result<std::unique_ptr<Fat32Filesystem>> fs = Fat32Filesystem::open(source);
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
    EXPECT_GT(opened, 100) << "mutations should mostly leave the volume mountable";
}

}  // namespace
}  // namespace recovery::filesystem::fat32
