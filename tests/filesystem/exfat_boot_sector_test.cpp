// exFAT boot sector parsing and boot region checksums.

#include "filesystem/exfat/exfat_boot_sector.hpp"

#include "recovery/byte_order.hpp"
#include "support/exfat_builder.hpp"
#include "support/fat32_builder.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <functional>

namespace recovery::filesystem::exfat {
namespace {

using test::ExFatImageBuilder;

constexpr std::uint64_t kLargeSource = 1ULL << 40;

// The first 512 bytes of a freshly built volume.
std::vector<std::byte> bootSectorOf(ExFatImageBuilder& builder) {
    const std::vector<std::byte> image = builder.build();
    return {image.begin(), image.begin() + 512};
}

std::vector<std::byte> defaultBootSector() {
    ExFatImageBuilder builder;
    return bootSectorOf(builder);
}

bool hasWarning(const BootSector& boot, std::string_view text) {
    return std::any_of(boot.warnings.begin(), boot.warnings.end(),
                       [&](const std::string& w) { return w.find(text) != std::string::npos; });
}

TEST(ExFatBootSectorTest, ParsesFieldsAndLayout) {
    ExFatImageBuilder builder;
    const std::vector<std::byte> image = builder.build();
    const Result<BootSector> boot = parseBootSector(image, image.size());
    RECOVERY_ASSERT_OK(boot);
    EXPECT_EQ(boot->bytesPerSector, 512u);
    EXPECT_EQ(boot->clusterSize, 512u);
    EXPECT_EQ(boot->fatCount, 1u);
    EXPECT_EQ(boot->fatOffset, 32u * 512u);
    EXPECT_EQ(boot->fatBytes, 33u * 512u);  // (4096 + 2) entries of 4 bytes
    EXPECT_EQ(boot->dataOffset, builder.clusterOffset(2));
    EXPECT_EQ(boot->clusterCount, 4096u);
    EXPECT_EQ(boot->recordedClusterCount, 4096u);
    EXPECT_EQ(boot->lastCluster(), 4097u);
    EXPECT_EQ(boot->rootCluster, builder.root());
    EXPECT_EQ(boot->volumeSerial, 0x1234ABCDu);
    EXPECT_EQ(boot->revisionMajor, 1u);
    EXPECT_EQ(boot->revisionMinor, 0u);
    EXPECT_EQ(boot->volumeBytes, image.size());
    EXPECT_EQ(boot->activeFat, 0u);
    EXPECT_FALSE(boot->dirty);
    EXPECT_TRUE(boot->warnings.empty()) << boot->warnings.front();
    EXPECT_TRUE(boot->isValidCluster(2));
    EXPECT_FALSE(boot->isValidCluster(1));
    EXPECT_FALSE(boot->isValidCluster(4098));
    EXPECT_EQ(boot->clusterOffset(3), builder.clusterOffset(3));
}

TEST(ExFatBootSectorTest, BootChecksumMatchesReferenceValue) {
    std::vector<std::byte> region(11 * 512);
    for (std::size_t i = 0; i < region.size(); ++i) {
        region[i] = static_cast<std::byte>((i * 13 + 5) & 0xFF);
    }
    EXPECT_EQ(bootChecksum(region, 512), 0x185A0A5Du);
}

TEST(ExFatBootSectorTest, BootChecksumIgnoresFlagsAndPercentInUse) {
    ExFatImageBuilder builder;
    std::vector<std::byte> image = builder.build();
    const std::span<std::byte> main(image.data(), 12 * 512);
    const std::span<std::byte> backup(image.data() + 12 * 512, 12 * 512);
    EXPECT_TRUE(bootChecksumMatches(main, 512));
    EXPECT_TRUE(bootChecksumMatches(backup, 512));
    main[106] = std::byte{0x02};
    main[107] = std::byte{0xFF};
    main[112] = std::byte{77};
    EXPECT_TRUE(bootChecksumMatches(main, 512));
    main[100] ^= std::byte{1};  // volume serial number
    EXPECT_FALSE(bootChecksumMatches(main, 512));
    EXPECT_FALSE(bootChecksumMatches(main.first(11 * 512), 512));  // no checksum sector
}

TEST(ExFatBootSectorTest, LargerSectorsAndClusters) {
    test::ExFatBuilderOptions options;
    options.bytesPerSectorShift = 12;
    options.sectorsPerClusterShift = 3;
    options.clusterCount = 64;
    options.fatOffsetSectors = 24;
    ExFatImageBuilder builder(options);
    const std::vector<std::byte> image = builder.build();
    const Result<BootSector> boot = parseBootSector(image, image.size());
    RECOVERY_ASSERT_OK(boot);
    EXPECT_EQ(boot->bytesPerSector, 4096u);
    EXPECT_EQ(boot->clusterSize, 32768u);
    EXPECT_EQ(boot->dataOffset, builder.clusterOffset(2));
    EXPECT_TRUE(bootChecksumMatches(std::span<const std::byte>(image).first(12 * 4096), 4096));
}

TEST(ExFatBootSectorTest, WarnsWhenVolumeExceedsSource) {
    const std::vector<std::byte> sector = defaultBootSector();
    const Result<BootSector> boot = parseBootSector(sector, 1024 * 1024);
    RECOVERY_ASSERT_OK(boot);
    EXPECT_TRUE(hasWarning(*boot, "truncated"));
}

TEST(ExFatBootSectorTest, ClusterCountIsLimitedByTheHeap) {
    std::vector<std::byte> sector = defaultBootSector();
    storeLe32(sector, 92, 4200);  // the FAT has room for 4222, the heap for 4096
    const Result<BootSector> boot = parseBootSector(sector, kLargeSource);
    RECOVERY_ASSERT_OK(boot);
    EXPECT_EQ(boot->recordedClusterCount, 4200u);
    EXPECT_EQ(boot->clusterCount, 4096u);
    EXPECT_TRUE(hasWarning(*boot, "fit in the volume"));
}

TEST(ExFatBootSectorTest, ClusterCountIsLimitedByTheFat) {
    std::vector<std::byte> sector = defaultBootSector();
    storeLe32(sector, 84, 20);  // 20 FAT sectors hold 2560 entries
    const Result<BootSector> boot = parseBootSector(sector, kLargeSource);
    RECOVERY_ASSERT_OK(boot);
    EXPECT_EQ(boot->clusterCount, 2558u);
    EXPECT_TRUE(hasWarning(*boot, "FAT covers only"));
}

TEST(ExFatBootSectorTest, VolumeFlags) {
    std::vector<std::byte> sector = defaultBootSector();
    sector[106] = std::byte{0x06};  // dirty, media failure
    const Result<BootSector> boot = parseBootSector(sector, kLargeSource);
    RECOVERY_ASSERT_OK(boot);
    EXPECT_TRUE(boot->dirty);
    EXPECT_TRUE(boot->mediaFailure);
    EXPECT_TRUE(hasWarning(*boot, "dirty"));
    EXPECT_TRUE(hasWarning(*boot, "media failures"));

    sector[106] = std::byte{0x01};  // second FAT on a single-FAT volume
    const Result<BootSector> single = parseBootSector(sector, kLargeSource);
    RECOVERY_ASSERT_OK(single);
    EXPECT_EQ(single->activeFat, 0u);
    EXPECT_TRUE(hasWarning(*single, "does not exist"));
}

TEST(ExFatBootSectorTest, SecondFatSelectedByActiveFatFlag) {
    test::ExFatBuilderOptions options;
    options.fatCount = 2;
    ExFatImageBuilder builder(options);
    std::vector<std::byte> sector = bootSectorOf(builder);
    sector[106] = std::byte{0x01};
    const Result<BootSector> boot = parseBootSector(sector, kLargeSource);
    RECOVERY_ASSERT_OK(boot);
    EXPECT_EQ(boot->fatCount, 2u);
    EXPECT_EQ(boot->activeFat, 1u);
    EXPECT_TRUE(boot->warnings.empty());
}

TEST(ExFatBootSectorTest, NonStandardValuesWarn) {
    std::vector<std::byte> sector = defaultBootSector();
    sector[1] = std::byte{0x3C};  // jump
    sector[40] = std::byte{1};    // inside the zero area
    sector[112] = std::byte{150};
    const Result<BootSector> boot = parseBootSector(sector, kLargeSource);
    RECOVERY_ASSERT_OK(boot);
    EXPECT_TRUE(hasWarning(*boot, "jump"));
    EXPECT_TRUE(hasWarning(*boot, "bytes 11-63"));
    EXPECT_TRUE(hasWarning(*boot, "percent-in-use"));

    test::ExFatBuilderOptions options;
    options.clusterCount = 1024;  // about 0.5 MiB
    ExFatImageBuilder small(options);
    const Result<BootSector> tiny = parseBootSector(bootSectorOf(small), kLargeSource);
    RECOVERY_ASSERT_OK(tiny);
    EXPECT_TRUE(hasWarning(*tiny, "1 MiB"));
}

struct Corruption {
    const char* name;
    std::function<void(std::vector<std::byte>&)> apply;
};

class ExFatBootSectorRejectTest : public ::testing::TestWithParam<Corruption> {};

TEST_P(ExFatBootSectorRejectTest, IsCorrupted) {
    std::vector<std::byte> sector = defaultBootSector();
    GetParam().apply(sector);
    RECOVERY_EXPECT_ERROR(parseBootSector(sector, kLargeSource), ErrorCode::CorruptedFilesystem);
}

INSTANTIATE_TEST_SUITE_P(
    Fields, ExFatBootSectorRejectTest,
    ::testing::Values(
        Corruption{"MissingSignature", [](auto& s) { s[510] = std::byte{0}; }},
        Corruption{"SectorShiftTooSmall", [](auto& s) { s[108] = std::byte{8}; }},
        Corruption{"SectorShiftTooLarge", [](auto& s) { s[108] = std::byte{13}; }},
        Corruption{"ClusterTooLarge", [](auto& s) { s[109] = std::byte{17}; }},  // 512 << 17 = 64 MiB
        Corruption{"NoFats", [](auto& s) { s[110] = std::byte{0}; }},
        Corruption{"ThreeFats", [](auto& s) { s[110] = std::byte{3}; }},
        Corruption{"ZeroVolumeLength", [](auto& s) { storeLe64(s, 72, 0); }},
        Corruption{"VolumeLengthOverflows",
                   [](auto& s) {
                       storeLe64(s, 72, 1ULL << 62);
                       s[108] = std::byte{12};
                   }},
        Corruption{"FatOverlapsBootRegions", [](auto& s) { storeLe32(s, 80, 23); }},
        Corruption{"ZeroFatLength", [](auto& s) { storeLe32(s, 84, 0); }},
        Corruption{"FatOverlapsHeap", [](auto& s) { storeLe32(s, 88, 40); }},
        Corruption{"HeapBeyondVolume", [](auto& s) { storeLe32(s, 88, 0xFFFFFFFF); }},
        Corruption{"ZeroClusters", [](auto& s) { storeLe32(s, 92, 0); }},
        Corruption{"RootClusterTooSmall", [](auto& s) { storeLe32(s, 96, 1); }},
        Corruption{"RootClusterTooLarge", [](auto& s) { storeLe32(s, 96, 4098); }}),
    [](const auto& info) { return std::string(info.param.name); });

TEST(ExFatBootSectorTest, RejectsOtherFilesystemsAndRevisions) {
    std::vector<std::byte> sector = defaultBootSector();
    sector[105] = std::byte{2};  // revision 2.00
    RECOVERY_EXPECT_ERROR(parseBootSector(sector, kLargeSource), ErrorCode::UnsupportedFilesystem);

    std::vector<std::byte> ntfs = defaultBootSector();
    std::memcpy(ntfs.data() + 3, "NTFS    ", 8);
    RECOVERY_EXPECT_ERROR(parseBootSector(ntfs, kLargeSource), ErrorCode::UnsupportedFilesystem);

    test::Fat32ImageBuilder fat32;
    const std::vector<std::byte> fatImage = fat32.build();
    RECOVERY_EXPECT_ERROR(parseBootSector(fatImage, fatImage.size()), ErrorCode::UnsupportedFilesystem);

    RECOVERY_EXPECT_ERROR(parseBootSector(std::vector<std::byte>(512), kLargeSource),
                          ErrorCode::UnsupportedFilesystem);
    RECOVERY_EXPECT_ERROR(parseBootSector(std::vector<std::byte>(100), kLargeSource), ErrorCode::InvalidInput);
}

}  // namespace
}  // namespace recovery::filesystem::exfat
