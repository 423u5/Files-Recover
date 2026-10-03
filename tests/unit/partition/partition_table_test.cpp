#include "partition/partition_table.hpp"

#include "recovery/byte_order.hpp"
#include "support/memory_source.hpp"
#include "support/partition_builder.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace recovery::partition {
namespace {

using test::GptPartitionSpec;
using test::GptSpec;
using test::MbrEntrySpec;

constexpr std::uint32_t kSector = 512;
constexpr std::uint64_t kDiskSectors = 16384;  // 8 MiB

class PartitionTableTest : public ::testing::Test {
protected:
    PartitionTable read(std::uint32_t sectorSize = kSector) {
        source_ = std::make_unique<test::MemoryStorageSource>(disk_, sectorSize);
        EXPECT_TRUE(source_->open().ok());
        Result<PartitionTable> table = readPartitionTable(*source_);
        EXPECT_TRUE(table.ok());
        return table.ok() ? std::move(table).value() : PartitionTable{};
    }

    void writeMbr(const std::vector<MbrEntrySpec>& entries) { test::writeMbrSector(disk_, kSector, 0, entries); }

    std::span<std::byte> sector(std::uint64_t lba, std::uint32_t sectorSize = kSector) {
        return std::span<std::byte>(disk_).subspan(static_cast<std::size_t>(lba * sectorSize), sectorSize);
    }

    std::vector<std::byte> disk_ = std::vector<std::byte>(kDiskSectors * kSector);
    std::unique_ptr<test::MemoryStorageSource> source_;
};

// ---------------------------------------------------------------------------
// MBR
// ---------------------------------------------------------------------------

TEST_F(PartitionTableTest, SinglePrimaryPartition) {
    writeMbr({{0x80, 0x0C, 2048, 8192}});
    const PartitionTable t = read();
    EXPECT_EQ(t.scheme, PartitionScheme::Mbr);
    EXPECT_EQ(t.sectorSize, kSector);
    EXPECT_EQ(t.deviceSectors, kDiskSectors);
    EXPECT_TRUE(t.issues.empty());
    ASSERT_EQ(t.partitions.size(), 1u);
    const Partition& p = t.partitions[0];
    EXPECT_EQ(p.index, 0u);
    EXPECT_EQ(p.firstLba, 2048u);
    EXPECT_EQ(p.sectorCount, 8192u);
    EXPECT_EQ(p.offset, 2048u * kSector);
    EXPECT_EQ(p.size, 8192u * kSector);
    EXPECT_EQ(p.mbrType, 0x0C);
    EXPECT_TRUE(p.bootable);
    EXPECT_FALSE(p.truncated);
    EXPECT_EQ(p.typeName, "FAT32");
    EXPECT_EQ(p.candidates, (FilesystemCandidates{true, false, false}));
}

TEST_F(PartitionTableTest, FourPrimaryPartitions) {
    writeMbr({{0x00, 0x0C, 64, 1000}, {0x00, 0x07, 2048, 2000}, {0x00, 0x83, 5000, 3000}, {0x00, 0x0E, 9000, 7000}});
    const PartitionTable t = read();
    ASSERT_EQ(t.partitions.size(), 4u);
    for (std::uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(t.partitions[i].index, i);
    }
    EXPECT_EQ(t.partitions[1].candidates, (FilesystemCandidates{false, true, true}));
    EXPECT_FALSE(t.partitions[2].candidates.any());
    EXPECT_TRUE(t.issues.empty());
}

TEST_F(PartitionTableTest, EmptyMbrHasNoPartitions) {
    writeMbr({});
    const PartitionTable t = read();
    EXPECT_EQ(t.scheme, PartitionScheme::Mbr);
    EXPECT_TRUE(t.partitions.empty());
}

TEST_F(PartitionTableTest, MissingSignatureMeansNoTable) {
    writeMbr({{0x00, 0x0C, 2048, 8192}});
    sector(0)[510] = std::byte{0};
    const PartitionTable t = read();
    EXPECT_EQ(t.scheme, PartitionScheme::Unknown);
    EXPECT_TRUE(t.partitions.empty());
}

TEST_F(PartitionTableTest, InvalidStatusByteMeansNotAnMbr) {
    writeMbr({{0x12, 0x0C, 2048, 8192}});
    const PartitionTable t = read();
    EXPECT_EQ(t.scheme, PartitionScheme::Unknown);
    EXPECT_TRUE(t.partitions.empty());
}

TEST_F(PartitionTableTest, BlankDiskIsUnknown) {
    const PartitionTable t = read();
    EXPECT_EQ(t.scheme, PartitionScheme::Unknown);
    EXPECT_TRUE(t.partitions.empty());
}

TEST_F(PartitionTableTest, PartitionStartingBeyondDeviceIsDropped) {
    writeMbr({{0x00, 0x0C, static_cast<std::uint32_t>(kDiskSectors), 100}, {0x00, 0x0C, 2048, 100}});
    const PartitionTable t = read();
    ASSERT_EQ(t.partitions.size(), 1u);
    EXPECT_EQ(t.partitions[0].index, 1u);
    ASSERT_TRUE(t.hasIssue(PartitionIssueKind::PartitionOutOfRange));
    EXPECT_EQ(t.issues[0].partitionIndex, 0u);
}

TEST_F(PartitionTableTest, PartitionExtendingBeyondDeviceIsClamped) {
    writeMbr({{0x00, 0x0C, 2048, 1'000'000}});
    const PartitionTable t = read();
    ASSERT_EQ(t.partitions.size(), 1u);
    EXPECT_TRUE(t.partitions[0].truncated);
    EXPECT_EQ(t.partitions[0].sectorCount, kDiskSectors - 2048);
    EXPECT_EQ(t.partitions[0].offset + t.partitions[0].size, kDiskSectors * kSector);
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::PartitionTruncated));
}

TEST_F(PartitionTableTest, MaximumFieldValuesDoNotOverflow) {
    writeMbr({{0x00, 0x0C, 0xFFFFFFFF, 0xFFFFFFFF}, {0x00, 0x0C, 1, 0xFFFFFFFF}});
    const PartitionTable t = read();
    ASSERT_EQ(t.partitions.size(), 1u);
    EXPECT_EQ(t.partitions[0].firstLba, 1u);
    EXPECT_EQ(t.partitions[0].sectorCount, kDiskSectors - 1);
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::PartitionOutOfRange));
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::PartitionTruncated));
}

TEST_F(PartitionTableTest, InvalidEntriesAreSkipped) {
    writeMbr({{0x00, 0x0C, 0, 100}, {0x00, 0x0C, 100, 0}, {0x00, 0x0C, 2048, 100}});
    const PartitionTable t = read();
    ASSERT_EQ(t.partitions.size(), 1u);
    EXPECT_EQ(t.partitions[0].index, 2u);
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::MbrInvalidEntry));
}

TEST_F(PartitionTableTest, OverlappingPartitionsAreReported) {
    writeMbr({{0x00, 0x0C, 2048, 4096}, {0x00, 0x0C, 4096, 4096}});
    const PartitionTable t = read();
    EXPECT_EQ(t.partitions.size(), 2u);
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::PartitionOverlap));
}

// ---------------------------------------------------------------------------
// Extended partitions
// ---------------------------------------------------------------------------

TEST_F(PartitionTableTest, ExtendedPartitionWithLogicalDrives) {
    constexpr std::uint32_t kExt = 4096;
    writeMbr({{0x00, 0x0C, 2048, 2000}, {0x00, 0x0F, kExt, 12000}});
    // EBR chain: kExt -> kExt+4000 -> kExt+8000
    test::writeMbrSector(disk_, kSector, kExt, {{0x00, 0x0C, 63, 1000}, {0x00, 0x05, 4000, 3000}});
    test::writeMbrSector(disk_, kSector, kExt + 4000, {{0x00, 0x07, 63, 1500}, {0x00, 0x05, 8000, 3000}});
    test::writeMbrSector(disk_, kSector, kExt + 8000, {{0x00, 0x83, 63, 2000}});

    const PartitionTable t = read();
    EXPECT_TRUE(t.issues.empty()) << toString(t.issues[0].kind);
    ASSERT_EQ(t.partitions.size(), 4u);
    EXPECT_FALSE(t.partitions[0].logical);
    const std::uint64_t expectedStarts[] = {kExt + 63, kExt + 4000 + 63, kExt + 8000 + 63};
    for (std::size_t i = 0; i < 3; ++i) {
        const Partition& p = t.partitions[i + 1];
        EXPECT_TRUE(p.logical);
        EXPECT_EQ(p.index, 4u + i);
        EXPECT_EQ(p.firstLba, expectedStarts[i]);
    }
    EXPECT_EQ(t.partitions[3].sectorCount, 2000u);
}

TEST_F(PartitionTableTest, ExtendedChainLoopTerminates) {
    constexpr std::uint32_t kExt = 4096;
    writeMbr({{0x00, 0x0F, kExt, 12000}});
    test::writeMbrSector(disk_, kSector, kExt, {{0x00, 0x0C, 63, 1000}, {0x00, 0x05, 4000, 3000}});
    // Second EBR links back to the first.
    test::writeMbrSector(disk_, kSector, kExt + 4000, {{0x00, 0x0C, 63, 1000}, {0x00, 0x05, 0, 3000}});

    const PartitionTable t = read();
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::ExtendedChainLoop));
    EXPECT_EQ(t.partitions.size(), 2u);
}

TEST_F(PartitionTableTest, ExtendedChainLeavingContainerIsRejected) {
    constexpr std::uint32_t kExt = 4096;
    writeMbr({{0x00, 0x0F, kExt, 5000}});
    test::writeMbrSector(disk_, kSector, kExt, {{0x00, 0x0C, 63, 1000}, {0x00, 0x05, 9000, 100}});
    const PartitionTable t = read();
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::ExtendedChainInvalid));
    EXPECT_EQ(t.partitions.size(), 1u);
}

TEST_F(PartitionTableTest, LogicalPartitionIsConfinedToContainer) {
    constexpr std::uint32_t kExt = 4096;
    writeMbr({{0x00, 0x0F, kExt, 2000}});
    test::writeMbrSector(disk_, kSector, kExt, {{0x00, 0x0C, 63, 5000}});
    const PartitionTable t = read();
    ASSERT_EQ(t.partitions.size(), 1u);
    EXPECT_TRUE(t.partitions[0].truncated);
    EXPECT_EQ(t.partitions[0].firstLba + t.partitions[0].sectorCount, kExt + 2000u);
}

TEST_F(PartitionTableTest, EbrWithoutSignatureStopsChain) {
    constexpr std::uint32_t kExt = 4096;
    writeMbr({{0x00, 0x0F, kExt, 5000}});
    const PartitionTable t = read();
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::ExtendedChainInvalid));
    EXPECT_TRUE(t.partitions.empty());
}

TEST_F(PartitionTableTest, OverlongExtendedChainIsCut) {
    constexpr std::uint32_t kExt = 1024;
    writeMbr({{0x00, 0x0F, kExt, 15000}});
    for (std::uint32_t i = 0; i < kMaxLogicalPartitions + 10; ++i) {
        test::writeMbrSector(disk_, kSector, kExt + i * 2,
                             {{0x00, 0x0C, 1, 1}, {0x00, 0x05, (i + 1) * 2, 2}});
    }
    const PartitionTable t = read();
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::ExtendedChainTooLong));
    EXPECT_EQ(t.partitions.size(), kMaxLogicalPartitions);
}

TEST_F(PartitionTableTest, SecondExtendedPartitionIsIgnored) {
    writeMbr({{0x00, 0x0F, 4096, 100}, {0x00, 0x05, 8192, 100}});
    test::writeMbrSector(disk_, kSector, 4096, {{0x00, 0x0C, 1, 50}});
    const PartitionTable t = read();
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::MbrInvalidEntry));
    EXPECT_EQ(t.partitions.size(), 1u);
}

// ---------------------------------------------------------------------------
// Unpartitioned media (volume boot record at sector 0)
// ---------------------------------------------------------------------------

TEST_F(PartitionTableTest, Fat32SuperfloppyIsUnpartitioned) {
    const auto vbr = test::makeFat32BootRecordStub();
    std::copy(vbr.begin(), vbr.end(), disk_.begin());
    const PartitionTable t = read();
    EXPECT_EQ(t.scheme, PartitionScheme::Unpartitioned);
    ASSERT_EQ(t.partitions.size(), 1u);
    EXPECT_EQ(t.partitions[0].offset, 0u);
    EXPECT_EQ(t.partitions[0].size, disk_.size());
    EXPECT_EQ(t.partitions[0].candidates, (FilesystemCandidates{true, false, false}));
}

TEST_F(PartitionTableTest, ExFatAndNtfsSuperfloppies) {
    auto vbr = test::makeOemBootRecordStub("EXFAT   ");
    std::copy(vbr.begin(), vbr.end(), disk_.begin());
    EXPECT_EQ(read().partitions.at(0).candidates, (FilesystemCandidates{false, true, false}));

    vbr = test::makeOemBootRecordStub("NTFS    ");
    std::copy(vbr.begin(), vbr.end(), disk_.begin());
    EXPECT_EQ(read().partitions.at(0).candidates, (FilesystemCandidates{false, false, true}));
}

TEST_F(PartitionTableTest, BootCodeWithoutBpbIsNotAVolume) {
    // Jump instruction and signature, but no valid BPB or OEM name.
    writeMbr({{0x00, 0x0C, 2048, 100}});
    sector(0)[0] = std::byte{0xEB};
    sector(0)[2] = std::byte{0x90};
    const PartitionTable t = read();
    EXPECT_EQ(t.scheme, PartitionScheme::Mbr);
    EXPECT_EQ(t.partitions.size(), 1u);
}

// ---------------------------------------------------------------------------
// GPT
// ---------------------------------------------------------------------------

const Guid kUnique1 = *Guid::parse("AAAAAAAA-0000-0000-0000-000000000001");
const Guid kUnique2 = *Guid::parse("AAAAAAAA-0000-0000-0000-000000000002");
const Guid kUnique3 = *Guid::parse("AAAAAAAA-0000-0000-0000-000000000003");

GptSpec threePartitionSpec(std::uint32_t sectorSize = kSector) {
    GptSpec spec;
    spec.sectorSize = sectorSize;
    spec.partitions = {
        {gpt_types::kEfiSystem, kUnique1, 64, 1087, 0, u"EFI system partition"},
        {gpt_types::kMicrosoftReserved, kUnique2, 1088, 1119, 0, u"Microsoft reserved partition"},
        {gpt_types::kMicrosoftBasicData, kUnique3, 1120, 9000, 0x8000000000000000ULL,
         u"Données \U0001F4F7"},
    };
    return spec;
}

TEST_F(PartitionTableTest, ValidGpt) {
    const test::GptLayout layout = test::writeGpt(disk_, threePartitionSpec());
    const PartitionTable t = read();
    EXPECT_EQ(t.scheme, PartitionScheme::Gpt);
    EXPECT_TRUE(t.issues.empty()) << toString(t.issues.front().kind) << ": " << t.issues.front().detail;
    ASSERT_TRUE(t.gpt.has_value());
    EXPECT_TRUE(t.gpt->primaryValid);
    EXPECT_TRUE(t.gpt->backupValid);
    EXPECT_EQ(t.gpt->headerLba, 1u);
    EXPECT_EQ(t.gpt->firstUsableLba, layout.firstUsableLba);
    EXPECT_EQ(t.gpt->lastUsableLba, layout.lastUsableLba);
    EXPECT_EQ(t.gpt->diskGuid.toString(), "11111111-2222-3333-4444-555555555555");

    ASSERT_EQ(t.partitions.size(), 3u);
    const Partition& data = t.partitions[2];
    EXPECT_EQ(data.index, 2u);
    EXPECT_EQ(data.firstLba, 1120u);
    EXPECT_EQ(data.sectorCount, 9000u - 1120u + 1u);
    EXPECT_EQ(data.offset, 1120u * kSector);
    EXPECT_EQ(data.typeGuid, gpt_types::kMicrosoftBasicData);
    EXPECT_EQ(data.uniqueGuid, kUnique3);
    EXPECT_EQ(data.attributes, 0x8000000000000000ULL);
    EXPECT_EQ(data.name, "Donn\xC3\xA9" "es \xF0\x9F\x93\xB7");
    EXPECT_EQ(data.typeName, "Microsoft basic data");
    EXPECT_EQ(data.candidates, (FilesystemCandidates{true, true, true}));
    EXPECT_EQ(t.partitions[0].candidates, (FilesystemCandidates{true, false, false}));
    EXPECT_FALSE(t.partitions[1].candidates.any());
}

TEST_F(PartitionTableTest, Gpt4KSectors) {
    constexpr std::uint32_t k4K = 4096;
    disk_.assign(2048 * static_cast<std::size_t>(k4K), std::byte{0});
    GptSpec spec;
    spec.sectorSize = k4K;
    spec.partitions = {{gpt_types::kMicrosoftBasicData, kUnique1, 256, 2000, 0, u"Data"}};
    test::writeGpt(disk_, spec);

    const PartitionTable t = read(k4K);
    EXPECT_EQ(t.scheme, PartitionScheme::Gpt);
    EXPECT_TRUE(t.issues.empty());
    ASSERT_EQ(t.partitions.size(), 1u);
    EXPECT_EQ(t.partitions[0].offset, 256u * k4K);
    EXPECT_EQ(t.partitions[0].size, (2000u - 256u + 1u) * k4K);
}

TEST_F(PartitionTableTest, CorruptPrimaryHeaderFallsBackToBackup) {
    const test::GptLayout layout = test::writeGpt(disk_, threePartitionSpec());
    sector(1)[60] ^= std::byte{0xFF};  // disk GUID byte: CRC no longer matches

    const PartitionTable t = read();
    EXPECT_EQ(t.scheme, PartitionScheme::Gpt);
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::GptPrimaryInvalid));
    ASSERT_TRUE(t.gpt.has_value());
    EXPECT_FALSE(t.gpt->primaryValid);
    EXPECT_TRUE(t.gpt->backupValid);
    EXPECT_EQ(t.gpt->headerLba, layout.backupHeaderLba);
    EXPECT_EQ(t.partitions.size(), 3u);
}

TEST_F(PartitionTableTest, CorruptPrimaryEntryArrayFallsBackToBackup) {
    const test::GptLayout layout = test::writeGpt(disk_, threePartitionSpec());
    sector(layout.primaryEntriesLba)[40] ^= std::byte{0x01};
    const PartitionTable t = read();
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::GptPrimaryInvalid));
    EXPECT_EQ(t.partitions.size(), 3u);
    EXPECT_EQ(t.partitions[0].firstLba, 64u);
}

TEST_F(PartitionTableTest, CorruptBackupIsReportedButPrimaryUsed) {
    const test::GptLayout layout = test::writeGpt(disk_, threePartitionSpec());
    sector(layout.backupHeaderLba)[0] = std::byte{'X'};
    const PartitionTable t = read();
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::GptBackupInvalid));
    EXPECT_TRUE(t.gpt->primaryValid);
    EXPECT_EQ(t.partitions.size(), 3u);
}

TEST_F(PartitionTableTest, BothHeadersCorruptLeavesNoPartitions) {
    const test::GptLayout layout = test::writeGpt(disk_, threePartitionSpec());
    sector(1)[0] = std::byte{'X'};
    sector(layout.backupHeaderLba)[0] = std::byte{'X'};
    const PartitionTable t = read();
    EXPECT_EQ(t.scheme, PartitionScheme::Gpt);
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::GptUnavailable));
    EXPECT_TRUE(t.partitions.empty());
    EXPECT_FALSE(t.gpt.has_value());
}

TEST_F(PartitionTableTest, HybridMbrUsedWhenGptIsUnavailable) {
    const test::GptLayout layout = test::writeGpt(disk_, threePartitionSpec());
    writeMbr({{0x00, 0xEE, 1, 63}, {0x00, 0x0C, 1120, 7881}});
    sector(1)[0] = std::byte{'X'};
    sector(layout.backupHeaderLba)[0] = std::byte{'X'};
    const PartitionTable t = read();
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::GptUnavailable));
    ASSERT_EQ(t.partitions.size(), 1u);
    EXPECT_EQ(t.partitions[0].firstLba, 1120u);
}

TEST_F(PartitionTableTest, HybridMbrWithValidGptIsReported) {
    test::writeGpt(disk_, threePartitionSpec());
    writeMbr({{0x00, 0xEE, 1, 63}, {0x00, 0x0C, 1120, 7881}});
    const PartitionTable t = read();
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::ProtectiveMbrInconsistent));
    EXPECT_EQ(t.partitions.size(), 3u);  // GPT wins
}

TEST_F(PartitionTableTest, GptWithoutProtectiveMbrIsFound) {
    GptSpec spec = threePartitionSpec();
    spec.protectiveMbr = false;
    test::writeGpt(disk_, spec);
    const PartitionTable t = read();
    EXPECT_EQ(t.scheme, PartitionScheme::Gpt);
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::ProtectiveMbrMissing));
    EXPECT_EQ(t.partitions.size(), 3u);
}

TEST_F(PartitionTableTest, UnreadableSectorZeroStillFindsGpt) {
    test::writeGpt(disk_, threePartitionSpec());
    source_ = std::make_unique<test::MemoryStorageSource>(disk_, kSector);
    source_->addBadSector(0);
    RECOVERY_ASSERT_OK(source_->open());
    const Result<PartitionTable> t = readPartitionTable(*source_);
    RECOVERY_ASSERT_OK(t);
    EXPECT_EQ(t->scheme, PartitionScheme::Gpt);
    EXPECT_TRUE(t->hasIssue(PartitionIssueKind::SectorUnreadable));
    EXPECT_EQ(t->partitions.size(), 3u);
}

TEST_F(PartitionTableTest, HeadersDescribingDifferentTablesAreReported) {
    const test::GptLayout layout = test::writeGpt(disk_, threePartitionSpec());
    // Change a backup entry and re-seal it: both headers are valid but differ.
    storeLe64(sector(layout.backupEntriesLba), 32, 70);
    test::resealGptEntries(disk_, kSector, layout.backupHeaderLba);
    const PartitionTable t = read();
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::GptHeadersDisagree));
    EXPECT_EQ(t.partitions.at(0).firstLba, 64u);  // primary wins
}

struct HeaderTamper {
    const char* name;
    std::size_t offset;
    std::size_t width;  // 4 or 8
    std::uint64_t value;
};

class GptHeaderTamperTest : public PartitionTableTest, public ::testing::WithParamInterface<HeaderTamper> {};

TEST_P(GptHeaderTamperTest, SemanticallyInvalidPrimaryIsRejected) {
    const test::GptLayout layout = test::writeGpt(disk_, threePartitionSpec());
    const HeaderTamper& c = GetParam();
    if (c.width == 8) {
        storeLe64(sector(1), c.offset, c.value);
    } else {
        storeLe32(sector(1), c.offset, static_cast<std::uint32_t>(c.value));
    }
    test::resealGptHeader(disk_, kSector, 1);  // CRC is valid again

    const PartitionTable t = read();
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::GptPrimaryInvalid));
    ASSERT_TRUE(t.gpt.has_value());
    EXPECT_EQ(t.gpt->headerLba, layout.backupHeaderLba);
    EXPECT_EQ(t.partitions.size(), 3u);
}

INSTANTIATE_TEST_SUITE_P(
    Fields, GptHeaderTamperTest,
    ::testing::Values(HeaderTamper{"RevisionMajor2", 8, 4, 0x00020000},
                      HeaderTamper{"HeaderSizeTooSmall", 12, 4, 91},
                      HeaderTamper{"HeaderSizeLargerThanSector", 12, 4, 513},
                      HeaderTamper{"WrongMyLba", 24, 8, 5},
                      HeaderTamper{"FirstUsableAfterLastUsable", 40, 8, kDiskSectors - 1},
                      HeaderTamper{"EntriesLbaBeyondDevice", 72, 8, kDiskSectors},
                      HeaderTamper{"EntriesLbaOverflows", 72, 8, std::numeric_limits<std::uint64_t>::max()},
                      HeaderTamper{"EntriesOverlapHeader", 72, 8, 1},
                      HeaderTamper{"EntriesInMbr", 72, 8, 0},
                      HeaderTamper{"ZeroEntries", 80, 4, 0},
                      HeaderTamper{"HugeEntryCount", 80, 4, 0xFFFFFFFF},
                      HeaderTamper{"EntryCountAboveLimit", 80, 4, kMaxGptEntries + 1},
                      HeaderTamper{"EntrySizeTooSmall", 84, 4, 64},
                      HeaderTamper{"EntrySizeNotPowerOfTwo", 84, 4, 200},
                      HeaderTamper{"EntrySizeTooLarge", 84, 4, 8192},
                      HeaderTamper{"EntrySizeZero", 84, 4, 0}),
    [](const auto& info) { return std::string(info.param.name); });

TEST_F(PartitionTableTest, InvalidGptEntriesAreHandled) {
    GptSpec spec;
    const std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    spec.partitions = {
        {gpt_types::kMicrosoftBasicData, kUnique1, 5000, 4000, 0, u"reversed"},       // first > last: dropped
        {gpt_types::kMicrosoftBasicData, kUnique2, 3000, kMax, 0, u"to infinity"},    // clamped to device
        {gpt_types::kMicrosoftBasicData, kUnique3, kMax, kMax, 0, u"far away"},       // beyond device: dropped
        {gpt_types::kMicrosoftBasicData, kUnique1, 1, 10, 0, u"in the header area"},  // outside usable range
    };
    test::writeGpt(disk_, spec);
    const PartitionTable t = read();

    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::GptEntryInvalid));
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::PartitionTruncated));
    EXPECT_TRUE(t.hasIssue(PartitionIssueKind::PartitionOutOfRange));
    ASSERT_EQ(t.partitions.size(), 2u);
    EXPECT_EQ(t.partitions[0].index, 1u);
    EXPECT_TRUE(t.partitions[0].truncated);
    EXPECT_EQ(t.partitions[0].offset + t.partitions[0].size, kDiskSectors * kSector);
    EXPECT_EQ(t.partitions[1].index, 3u);
}

TEST_F(PartitionTableTest, LargeEntrySizeIsSupported) {
    GptSpec spec = threePartitionSpec();
    spec.entrySize = 256;
    spec.entryCount = 64;
    test::writeGpt(disk_, spec);
    const PartitionTable t = read();
    EXPECT_TRUE(t.issues.empty());
    EXPECT_EQ(t.partitions.size(), 3u);
    EXPECT_EQ(t.gpt->entrySize, 256u);
}

TEST_F(PartitionTableTest, RejectsClosedSource) {
    test::MemoryStorageSource source(disk_, kSector);
    RECOVERY_EXPECT_ERROR(readPartitionTable(source), ErrorCode::InvalidInput);
}

TEST_F(PartitionTableTest, TinyDeviceIsUnknown) {
    disk_.resize(100);
    const PartitionTable t = read();
    EXPECT_EQ(t.scheme, PartitionScheme::Unknown);
    EXPECT_EQ(t.deviceSectors, 0u);
}

}  // namespace
}  // namespace recovery::partition
