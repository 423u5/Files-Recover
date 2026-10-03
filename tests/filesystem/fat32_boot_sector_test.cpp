#include "filesystem/fat32/fat32_boot_sector.hpp"

#include "recovery/byte_order.hpp"
#include "support/fat32_builder.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

namespace recovery::filesystem::fat32 {
namespace {

std::vector<std::byte> bootSectorOf(const test::Fat32BuilderOptions& options = {}) {
    test::Fat32ImageBuilder builder(options);
    std::vector<std::byte> image = builder.build();
    image.resize(512);
    return image;
}

bool hasWarningContaining(const BootSector& boot, std::string_view text) {
    return std::any_of(boot.warnings.begin(), boot.warnings.end(),
                       [text](const std::string& w) { return w.find(text) != std::string::npos; });
}

TEST(Fat32BootSectorTest, ParsesFieldsAndLayout) {
    test::Fat32BuilderOptions options;
    options.sectorsPerCluster = 8;
    options.clusterCount = 1000;
    const std::vector<std::byte> sector = bootSectorOf(options);

    const Result<BootSector> parsed = parseBootSector(sector, 100 * kMiB);
    RECOVERY_ASSERT_OK(parsed);
    const BootSector& b = parsed.value();
    EXPECT_EQ(b.oemName, "MSWIN4.1");
    EXPECT_EQ(b.bytesPerSector, 512u);
    EXPECT_EQ(b.sectorsPerCluster, 8u);
    EXPECT_EQ(b.clusterSize, 4096u);
    EXPECT_EQ(b.reservedSectors, 32u);
    EXPECT_EQ(b.fatCount, 2u);
    EXPECT_EQ(b.rootCluster, 2u);
    EXPECT_EQ(b.fsInfoSector, 1u);
    EXPECT_EQ(b.backupBootSector, 6u);
    EXPECT_EQ(b.volumeId, 0x1234ABCDu);
    EXPECT_EQ(b.volumeLabel, "TESTVOL");
    // FAT: (1000 + 2) * 4 bytes -> 8 sectors.
    EXPECT_EQ(b.fatSectors, 8u);
    EXPECT_EQ(b.fatOffset, 32u * 512u);
    EXPECT_EQ(b.dataOffset, (32u + 2u * 8u) * 512u);
    EXPECT_EQ(b.clusterCount, 1000u);
    EXPECT_EQ(b.lastCluster(), 1001u);
    EXPECT_TRUE(b.mirrored);
    EXPECT_EQ(b.activeFat, 0u);
    EXPECT_TRUE(hasWarningContaining(b, "below the FAT32 minimum"));
}

TEST(Fat32BootSectorTest, StandardSizedVolumeHasNoWarnings) {
    test::Fat32BuilderOptions options;
    options.clusterCount = kMinStandardClusters;
    const Result<BootSector> parsed = parseBootSector(bootSectorOf(options), 64 * kMiB);
    RECOVERY_ASSERT_OK(parsed);
    EXPECT_TRUE(parsed->warnings.empty()) << parsed->warnings.front();
}

TEST(Fat32BootSectorTest, EmptyLabelMeansNoName) {
    test::Fat32BuilderOptions options;
    options.label.clear();
    const Result<BootSector> parsed = parseBootSector(bootSectorOf(options), 64 * kMiB);
    RECOVERY_ASSERT_OK(parsed);
    EXPECT_EQ(parsed->volumeLabel, "");
}

TEST(Fat32BootSectorTest, WarnsWhenVolumeExceedsSource) {
    const Result<BootSector> parsed = parseBootSector(bootSectorOf(), 4096);
    RECOVERY_ASSERT_OK(parsed);
    EXPECT_TRUE(hasWarningContaining(parsed.value(), "truncated"));
}

TEST(Fat32BootSectorTest, ClusterCountIsLimitedByFatSize) {
    std::vector<std::byte> sector = bootSectorOf();
    storeLe32(sector, 36, 1);  // one FAT sector: 128 entries
    const Result<BootSector> parsed = parseBootSector(sector, 64 * kMiB);
    RECOVERY_ASSERT_OK(parsed);
    EXPECT_EQ(parsed->clusterCount, 126u);
    EXPECT_TRUE(hasWarningContaining(parsed.value(), "unreachable"));
}

TEST(Fat32BootSectorTest, NonMirroredFatSelectsActiveCopy) {
    std::vector<std::byte> sector = bootSectorOf();
    storeLe16(sector, 40, 0x81);
    const Result<BootSector> parsed = parseBootSector(sector, 64 * kMiB);
    RECOVERY_ASSERT_OK(parsed);
    EXPECT_FALSE(parsed->mirrored);
    EXPECT_EQ(parsed->activeFat, 1u);

    storeLe16(sector, 40, 0x85);  // copy 5 does not exist
    const Result<BootSector> invalid = parseBootSector(sector, 64 * kMiB);
    RECOVERY_ASSERT_OK(invalid);
    EXPECT_EQ(invalid->activeFat, 0u);
}

struct BootCase {
    const char* name;
    std::size_t offset;
    std::size_t width;
    std::uint32_t value;
    ErrorCode expected;
};

class Fat32BootSectorRejectTest : public ::testing::TestWithParam<BootCase> {};

TEST_P(Fat32BootSectorRejectTest, IsRejected) {
    std::vector<std::byte> sector = bootSectorOf();
    const BootCase& c = GetParam();
    switch (c.width) {
    case 1:
        sector[c.offset] = static_cast<std::byte>(c.value);
        break;
    case 2:
        storeLe16(sector, c.offset, static_cast<std::uint16_t>(c.value));
        break;
    default:
        storeLe32(sector, c.offset, c.value);
        break;
    }
    RECOVERY_EXPECT_ERROR(parseBootSector(sector, 64 * kMiB), c.expected);
}

INSTANTIATE_TEST_SUITE_P(
    Fields, Fat32BootSectorRejectTest,
    ::testing::Values(BootCase{"MissingSignature", 510, 2, 0, ErrorCode::UnsupportedFilesystem},
                      BootCase{"Fat16RootEntries", 17, 2, 512, ErrorCode::UnsupportedFilesystem},
                      BootCase{"Fat16FatSize", 22, 2, 9, ErrorCode::UnsupportedFilesystem},
                      BootCase{"ZeroFat32Size", 36, 4, 0, ErrorCode::UnsupportedFilesystem},
                      BootCase{"BytesPerSector1000", 11, 2, 1000, ErrorCode::CorruptedFilesystem},
                      BootCase{"BytesPerSectorZero", 11, 2, 0, ErrorCode::CorruptedFilesystem},
                      BootCase{"SectorsPerClusterZero", 13, 1, 0, ErrorCode::CorruptedFilesystem},
                      BootCase{"SectorsPerClusterThree", 13, 1, 3, ErrorCode::CorruptedFilesystem},
                      BootCase{"NoReservedSectors", 14, 2, 0, ErrorCode::CorruptedFilesystem},
                      BootCase{"NoFats", 16, 1, 0, ErrorCode::CorruptedFilesystem},
                      BootCase{"TooManyFats", 16, 1, 9, ErrorCode::CorruptedFilesystem},
                      BootCase{"ZeroTotalSectors", 32, 4, 0, ErrorCode::CorruptedFilesystem},
                      BootCase{"FatLargerThanVolume", 36, 4, 0xFFFFFFFF, ErrorCode::CorruptedFilesystem},
                      BootCase{"RootClusterZero", 44, 4, 0, ErrorCode::CorruptedFilesystem},
                      BootCase{"RootClusterOne", 44, 4, 1, ErrorCode::CorruptedFilesystem},
                      BootCase{"RootClusterBeyondVolume", 44, 4, 0x0FFFFFF0, ErrorCode::CorruptedFilesystem}),
    [](const auto& info) { return std::string(info.param.name); });

TEST(Fat32BootSectorTest, RejectsHugeClusters) {
    test::Fat32BuilderOptions options;
    options.bytesPerSector = 4096;
    options.sectorsPerCluster = 1;
    options.clusterCount = 100;
    std::vector<std::byte> image = test::Fat32ImageBuilder(options).build();
    image[13] = std::byte{128};  // 4096 * 128 = 512 KiB clusters
    RECOVERY_EXPECT_ERROR(parseBootSector(std::span(image).first(512), 64 * kMiB), ErrorCode::CorruptedFilesystem);
}

TEST(Fat32BootSectorTest, RejectsOtherFilesystems) {
    std::vector<std::byte> sector = bootSectorOf();
    std::memcpy(sector.data() + 3, "EXFAT   ", 8);
    RECOVERY_EXPECT_ERROR(parseBootSector(sector, 64 * kMiB), ErrorCode::UnsupportedFilesystem);
    std::memcpy(sector.data() + 3, "NTFS    ", 8);
    RECOVERY_EXPECT_ERROR(parseBootSector(sector, 64 * kMiB), ErrorCode::UnsupportedFilesystem);
    RECOVERY_EXPECT_ERROR(parseBootSector(std::span(sector).first(100), 64 * kMiB), ErrorCode::InvalidInput);
}

TEST(Fat32BootSectorTest, FsInfo) {
    test::Fat32ImageBuilder builder;
    (void)builder.addFile(builder.root(), "A.BIN", std::vector<std::byte>(1500));
    const std::vector<std::byte> image = builder.build();
    const Result<BootSector> boot = parseBootSector(std::span(image).first(512), image.size());
    RECOVERY_ASSERT_OK(boot);

    const auto fsInfoSector = std::span(image).subspan(512, 512);
    const std::optional<FsInfo> info = parseFsInfo(fsInfoSector, boot.value());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->freeClusters, 2048u - 1u - 3u);  // root + 3 data clusters

    std::vector<std::byte> broken(fsInfoSector.begin(), fsInfoSector.end());
    broken[0] = std::byte{0};
    EXPECT_FALSE(parseFsInfo(broken, boot.value()).has_value());

    std::vector<std::byte> unknown(fsInfoSector.begin(), fsInfoSector.end());
    storeLe32(unknown, 488, 0xFFFFFFFF);
    EXPECT_FALSE(parseFsInfo(unknown, boot.value())->freeClusters.has_value());
}

}  // namespace
}  // namespace recovery::filesystem::fat32
