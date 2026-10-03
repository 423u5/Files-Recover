// NTFS boot sector: field validation, cluster and record size encodings, and
// rejection of anything that is not NTFS.

#include "filesystem/ntfs/ntfs_boot_sector.hpp"

#include "recovery/byte_order.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace recovery::filesystem::ntfs {
namespace {

constexpr std::uint64_t kLargeSource = 1ULL << 40;

// 512-byte sectors, 8 sectors per cluster, 1 KiB records, 4 KiB index records,
// 16 MiB volume with the MFT at cluster 4 and the mirror at cluster 2047.
std::vector<std::byte> defaultBootSector() {
    std::vector<std::byte> s(512);
    s[0] = std::byte{0xEB};
    s[1] = std::byte{0x52};
    s[2] = std::byte{0x90};
    std::memcpy(s.data() + 3, "NTFS    ", 8);
    storeLe16(s, 11, 512);
    s[13] = std::byte{8};
    s[21] = std::byte{0xF8};
    storeLe64(s, 40, 32767);
    storeLe64(s, 48, 4);
    storeLe64(s, 56, 2047);
    s[64] = std::byte{0xF6};  // 2^10
    s[68] = std::byte{1};     // one cluster
    storeLe64(s, 72, 0x0123456789ABCDEFULL);
    s[510] = std::byte{0x55};
    s[511] = std::byte{0xAA};
    return s;
}

bool hasWarning(const BootSector& boot, std::string_view text) {
    return std::any_of(boot.warnings.begin(), boot.warnings.end(),
                       [&](const std::string& w) { return w.find(text) != std::string::npos; });
}

TEST(NtfsBootSectorTest, ParsesAValidBootSector) {
    const Result<BootSector> boot = parseBootSector(defaultBootSector(), kLargeSource);
    RECOVERY_ASSERT_OK(boot);
    EXPECT_EQ(boot->bytesPerSector, 512u);
    EXPECT_EQ(boot->sectorsPerCluster, 8u);
    EXPECT_EQ(boot->clusterSize, 4096u);
    EXPECT_EQ(boot->totalSectors, 32767u);
    EXPECT_EQ(boot->volumeBytes, 32767u * 512u);
    EXPECT_EQ(boot->clusterCount, 32767u / 8u);  // the partial last cluster is unused
    EXPECT_EQ(boot->mftCluster, 4u);
    EXPECT_EQ(boot->mftMirrorCluster, 2047u);
    EXPECT_EQ(boot->recordSize, 1024u);
    EXPECT_EQ(boot->indexRecordSize, 4096u);
    EXPECT_EQ(boot->volumeSerial, 0x0123456789ABCDEFULL);
    EXPECT_EQ(boot->clusterOffset(4), 16384u);
    EXPECT_TRUE(boot->isValidCluster(4094));
    EXPECT_FALSE(boot->isValidCluster(4095));
    EXPECT_TRUE(boot->warnings.empty()) << boot->warnings.front();
}

TEST(NtfsBootSectorTest, OtherFilesystemsAreUnsupported) {
    std::vector<std::byte> s = defaultBootSector();
    std::memcpy(s.data() + 3, "EXFAT   ", 8);
    RECOVERY_EXPECT_ERROR(parseBootSector(s, kLargeSource), ErrorCode::UnsupportedFilesystem);
    std::memcpy(s.data() + 3, "MSDOS5.0", 8);
    RECOVERY_EXPECT_ERROR(parseBootSector(s, kLargeSource), ErrorCode::UnsupportedFilesystem);
    RECOVERY_EXPECT_ERROR(parseBootSector(std::vector<std::byte>(512), kLargeSource),
                          ErrorCode::UnsupportedFilesystem);
    RECOVERY_EXPECT_ERROR(parseBootSector(std::vector<std::byte>(100), kLargeSource), ErrorCode::InvalidInput);
}

TEST(NtfsBootSectorTest, MissingSignatureIsCorrupt) {
    std::vector<std::byte> s = defaultBootSector();
    s[511] = std::byte{0};
    RECOVERY_EXPECT_ERROR(parseBootSector(s, kLargeSource), ErrorCode::CorruptedFilesystem);
}

TEST(NtfsBootSectorTest, SectorSizes) {
    for (const std::uint16_t bytes : std::initializer_list<std::uint16_t>{512, 1024, 2048, 4096}) {
        std::vector<std::byte> s = defaultBootSector();
        storeLe16(s, 11, bytes);
        s[13] = std::byte{1};
        storeLe64(s, 48, 16);
        const Result<BootSector> boot = parseBootSector(s, kLargeSource);
        RECOVERY_ASSERT_OK(boot);
        EXPECT_EQ(boot->clusterSize, bytes);
    }
    for (const std::uint16_t bytes : std::initializer_list<std::uint16_t>{0, 256, 513, 8192}) {
        std::vector<std::byte> s = defaultBootSector();
        storeLe16(s, 11, bytes);
        RECOVERY_EXPECT_ERROR(parseBootSector(s, kLargeSource), ErrorCode::CorruptedFilesystem);
    }
}

TEST(NtfsBootSectorTest, ClusterSizeEncodings) {
    struct Case {
        std::uint8_t raw;
        std::uint32_t sectors;
    };
    // Up to 128 sectors the byte is the count; beyond, it is 256 - log2.
    for (const Case c : {Case{1, 1}, Case{2, 2}, Case{128, 128}, Case{0xF8, 256}, Case{0xF4, 4096}}) {
        std::vector<std::byte> s = defaultBootSector();
        s[13] = static_cast<std::byte>(c.raw);
        storeLe64(s, 40, 1ULL << 30);
        storeLe64(s, 48, 1);
        storeLe64(s, 56, 2);
        const Result<BootSector> boot = parseBootSector(s, kLargeSource);
        RECOVERY_ASSERT_OK(boot);
        EXPECT_EQ(boot->sectorsPerCluster, c.sectors);
        EXPECT_EQ(boot->clusterSize, c.sectors * 512);
    }
    // Not a power of two, zero, and clusters above 2 MiB.
    for (const std::uint8_t raw : std::initializer_list<std::uint8_t>{0, 3, 0x81, 0xF3}) {
        std::vector<std::byte> s = defaultBootSector();
        s[13] = static_cast<std::byte>(raw);
        RECOVERY_EXPECT_ERROR(parseBootSector(s, kLargeSource), ErrorCode::CorruptedFilesystem);
    }
}

TEST(NtfsBootSectorTest, RecordSizeEncodings) {
    EXPECT_EQ(decodeRecordSize(0xF6, 4096), 1024u);   // 2^10 bytes
    EXPECT_EQ(decodeRecordSize(0xF4, 512), 4096u);    // 2^12 bytes
    EXPECT_EQ(decodeRecordSize(2, 512), 1024u);       // two clusters
    EXPECT_EQ(decodeRecordSize(1, 4096), 4096u);      // one cluster
    EXPECT_EQ(decodeRecordSize(0, 4096), 0u);         // zero
    EXPECT_EQ(decodeRecordSize(0xF8, 4096), 0u);      // 256 bytes: below the minimum
    EXPECT_EQ(decodeRecordSize(0xEF, 4096), 0u);      // 128 KiB: above the maximum
    EXPECT_EQ(decodeRecordSize(3, 512), 0u);          // 1536 bytes: not a power of two
    EXPECT_EQ(decodeRecordSize(0x80, 512), 0u);       // 2^128
    EXPECT_EQ(decodeRecordSize(32, 65536), 0u);       // 2 MiB

    std::vector<std::byte> s = defaultBootSector();
    s[64] = std::byte{0};
    RECOVERY_EXPECT_ERROR(parseBootSector(s, kLargeSource), ErrorCode::CorruptedFilesystem);
    // A bad index record size only matters once indexes are read.
    s = defaultBootSector();
    s[68] = std::byte{0};
    const Result<BootSector> boot = parseBootSector(s, kLargeSource);
    RECOVERY_ASSERT_OK(boot);
    EXPECT_EQ(boot->indexRecordSize, 0u);
    EXPECT_TRUE(hasWarning(*boot, "index record size"));
}

TEST(NtfsBootSectorTest, ImpossibleGeometryIsCorrupt) {
    std::vector<std::byte> s = defaultBootSector();
    storeLe64(s, 40, 0);
    RECOVERY_EXPECT_ERROR(parseBootSector(s, kLargeSource), ErrorCode::CorruptedFilesystem);

    s = defaultBootSector();
    storeLe64(s, 40, 7);  // less than one cluster
    RECOVERY_EXPECT_ERROR(parseBootSector(s, kLargeSource), ErrorCode::CorruptedFilesystem);

    s = defaultBootSector();
    storeLe64(s, 40, ~0ULL);  // the byte count overflows
    RECOVERY_EXPECT_ERROR(parseBootSector(s, kLargeSource), ErrorCode::CorruptedFilesystem);

    s = defaultBootSector();
    storeLe64(s, 48, 4095);  // the MFT outside the volume
    RECOVERY_EXPECT_ERROR(parseBootSector(s, kLargeSource), ErrorCode::CorruptedFilesystem);

    s = defaultBootSector();
    storeLe64(s, 48, ~0ULL);
    RECOVERY_EXPECT_ERROR(parseBootSector(s, kLargeSource), ErrorCode::CorruptedFilesystem);
}

TEST(NtfsBootSectorTest, SuspiciousButUsableValuesAreWarnings) {
    std::vector<std::byte> s = defaultBootSector();
    storeLe64(s, 56, 1ULL << 40);  // mirror outside the volume
    s[0] = std::byte{0xE9};
    storeLe16(s, 14, 32);  // FAT reserved sectors
    const Result<BootSector> boot = parseBootSector(s, 1024 * 1024);
    RECOVERY_ASSERT_OK(boot);
    EXPECT_TRUE(hasWarning(*boot, "mirror"));
    EXPECT_TRUE(hasWarning(*boot, "jump"));
    EXPECT_TRUE(hasWarning(*boot, "FAT fields"));
    EXPECT_TRUE(hasWarning(*boot, "truncated"));
}

}  // namespace
}  // namespace recovery::filesystem::ntfs
