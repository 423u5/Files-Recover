#include "partition/partition_source.hpp"

#include "support/memory_source.hpp"
#include "support/partition_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <random>

namespace recovery::partition {
namespace {

using storage::ReadStatus;

class PartitionSourceTest : public ::testing::Test {
protected:
    void SetUp() override { RECOVERY_ASSERT_OK(parent_.open()); }

    std::vector<std::byte> data_ = test::makePattern(1 * kMiB);
    test::MemoryStorageSource parent_{data_, 512};
};

TEST_F(PartitionSourceTest, TranslatesOffsets) {
    PartitionSource part(parent_, 4096, 8192);
    RECOVERY_ASSERT_OK(part.open());
    EXPECT_EQ(part.size(), 8192u);
    EXPECT_EQ(part.sectorSize(), 512u);
    EXPECT_EQ(part.offsetInParent(), 4096u);
    EXPECT_EQ(part.getInfo().sizeBytes, 8192u);

    std::vector<std::byte> buffer(100);
    RECOVERY_ASSERT_OK(part.readExact(ByteOffset{10}, buffer));
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), data_.begin() + 4096 + 10));

    std::vector<std::byte> sector(512);
    ASSERT_TRUE(part.readSectors(SectorNumber{15}, SectorCount{1}, sector).ok());
    EXPECT_TRUE(std::equal(sector.begin(), sector.end(), data_.begin() + 4096 + 15 * 512));
}

TEST_F(PartitionSourceTest, ReadsCannotLeaveThePartition) {
    PartitionSource part(parent_, 4096, 8192);
    RECOVERY_ASSERT_OK(part.open());
    std::vector<std::byte> buffer(16);
    EXPECT_EQ(part.read(ByteOffset{8192 - 8}, buffer).status, ReadStatus::OutOfRange);
    EXPECT_EQ(part.read(ByteOffset{8192}, buffer).status, ReadStatus::OutOfRange);
    EXPECT_EQ(part.read(ByteOffset{std::numeric_limits<std::uint64_t>::max() - 4}, buffer).status,
              ReadStatus::InvalidArgument);
    EXPECT_EQ(part.readSectors(SectorNumber{16}, SectorCount{1}, buffer).status, ReadStatus::InvalidArgument);
    std::vector<std::byte> sector(512);
    EXPECT_EQ(part.readSectors(SectorNumber{16}, SectorCount{1}, sector).status, ReadStatus::OutOfRange);
    EXPECT_EQ(parent_.readCount(), 0u) << "rejected reads must not reach the parent";
}

TEST_F(PartitionSourceTest, OpenRejectsRangesOutsideParent) {
    PartitionSource beyond(parent_, data_.size() - 100, 200);
    RECOVERY_EXPECT_ERROR(beyond.open(), ErrorCode::InvalidInput);
    PartitionSource overflowing(parent_, std::numeric_limits<std::uint64_t>::max() - 10, 100);
    RECOVERY_EXPECT_ERROR(overflowing.open(), ErrorCode::InvalidInput);
    EXPECT_FALSE(overflowing.isOpen());

    PartitionSource exact(parent_, 0, data_.size());
    RECOVERY_EXPECT_OK(exact.open());
}

TEST_F(PartitionSourceTest, RequiresOpenParent) {
    parent_.close();
    PartitionSource part(parent_, 0, 512);
    RECOVERY_EXPECT_ERROR(part.open(), ErrorCode::InvalidInput);
}

TEST_F(PartitionSourceTest, ClosingParentClosesView) {
    PartitionSource part(parent_, 0, 4096);
    RECOVERY_ASSERT_OK(part.open());
    parent_.close();
    EXPECT_FALSE(part.isOpen());
    std::vector<std::byte> buffer(16);
    EXPECT_EQ(part.read(ByteOffset{0}, buffer).status, ReadStatus::NotOpen);
}

TEST_F(PartitionSourceTest, ParentErrorsPropagate) {
    parent_.addBadSector(10);
    PartitionSource part(parent_, 4096, 8192);
    RECOVERY_ASSERT_OK(part.open());
    std::vector<std::byte> buffer(1024);
    const storage::ReadResult result = part.read(ByteOffset{512}, buffer);  // parent sectors 9-10
    EXPECT_EQ(result.status, ReadStatus::IoError);
    EXPECT_EQ(result.bytesRead, 512u);
    EXPECT_EQ(result.offset, 512u);  // reported relative to the partition
}

TEST_F(PartitionSourceTest, BuildsFromPartition) {
    Partition p;
    p.offset = 2048;
    p.size = 1024;
    PartitionSource part(parent_, p);
    RECOVERY_ASSERT_OK(part.open());
    EXPECT_EQ(part.offsetInParent(), 2048u);
    EXPECT_EQ(part.size(), 1024u);
}

// ---------------------------------------------------------------------------
// Robustness: randomly corrupted tables never yield partitions outside the
// device, and every reported partition can be opened as a PartitionSource.
// ---------------------------------------------------------------------------

void checkInvariants(test::MemoryStorageSource& disk, const PartitionTable& table) {
    for (const Partition& p : table.partitions) {
        ASSERT_LE(p.offset, disk.size());
        ASSERT_LE(p.size, disk.size() - p.offset);
        ASSERT_GT(p.size, 0u);
        PartitionSource view(disk, p);
        ASSERT_TRUE(view.open().ok());
    }
}

class PartitionFuzzTest : public ::testing::TestWithParam<int> {};

TEST_P(PartitionFuzzTest, CorruptedTablesStaySafe) {
    constexpr std::uint32_t kSector = 512;
    constexpr std::size_t kSectors = 4096;
    std::vector<std::byte> pristine(kSectors * kSector);
    if (GetParam() == 0) {
        test::GptSpec spec;
        spec.partitions = {{gpt_types::kMicrosoftBasicData, *Guid::parse("AAAAAAAA-0000-0000-0000-000000000001"),
                            100, 2000, 0, u"a"},
                           {gpt_types::kEfiSystem, *Guid::parse("AAAAAAAA-0000-0000-0000-000000000002"), 2001, 4000,
                            0, u"b"}};
        test::writeGpt(pristine, spec);
    } else {
        test::writeMbrSector(pristine, kSector, 0, {{0x80, 0x0C, 64, 1000}, {0x00, 0x0F, 1100, 2900}});
        test::writeMbrSector(pristine, kSector, 1100, {{0x00, 0x07, 10, 500}, {0x00, 0x05, 600, 600}});
        test::writeMbrSector(pristine, kSector, 1700, {{0x00, 0x83, 10, 500}});
    }

    // Interesting bytes: the MBR/EBR tables, the GPT headers and the entry arrays.
    const std::vector<std::pair<std::size_t, std::size_t>> regions = {
        {0, 3 * kSector}, {1100 * kSector, kSector}, {1700 * kSector, kSector}, {(kSectors - 33) * kSector, 33 * kSector}};

    std::mt19937_64 random(0xF00D + static_cast<std::uint64_t>(GetParam()));
    for (int iteration = 0; iteration < 1500; ++iteration) {
        std::vector<std::byte> disk = pristine;
        const int mutations = 1 + static_cast<int>(random() % 8);
        for (int m = 0; m < mutations; ++m) {
            const auto& [start, length] = regions[random() % regions.size()];
            const std::size_t at = start + random() % length;
            switch (random() % 3) {
            case 0:
                disk[at] = static_cast<std::byte>(random());
                break;
            case 1:
                disk[at] = std::byte{0xFF};
                break;
            default:
                disk[at] = std::byte{0x00};
                break;
            }
        }
        // Keep CRCs valid half of the time so semantic checks are exercised too.
        if (GetParam() == 0 && (random() & 1) != 0) {
            test::resealGptEntries(disk, kSector, 1);
        }

        test::MemoryStorageSource source(disk, kSector);
        ASSERT_TRUE(source.open().ok());
        const Result<PartitionTable> table = readPartitionTable(source);
        ASSERT_TRUE(table.ok());
        checkInvariants(source, table.value());
        if (HasFatalFailure()) {
            FAIL() << "iteration " << iteration;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Schemes, PartitionFuzzTest, ::testing::Values(0, 1),
                         [](const auto& info) { return info.param == 0 ? std::string("Gpt") : std::string("Mbr"); });

}  // namespace
}  // namespace recovery::partition
