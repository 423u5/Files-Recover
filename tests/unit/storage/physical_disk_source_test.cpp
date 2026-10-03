// PhysicalDiskSource against a simulated device: open validation, geometry
// validation and aligned I/O, without ever touching a real disk.

#include "storage/physical_disk_source.hpp"

#include "support/fake_device.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <random>

namespace recovery::storage {
namespace {

using test::FakeDeviceConfig;
using test::MockDeviceOpener;

class PhysicalDiskSourceTest : public ::testing::Test {
protected:
    std::shared_ptr<FakeDeviceConfig> makeConfig(std::size_t size, std::uint32_t sectorSize = 512) {
        auto config = std::make_shared<FakeDeviceConfig>();
        config->data = test::makePattern(size);
        config->geometry = test::diskGeometry(size, sectorSize);
        return config;
    }
};

TEST_F(PhysicalDiskSourceTest, OpensTheDevicePathReadOnly) {
    auto config = makeConfig(1 * kMiB);
    auto opener = std::make_shared<MockDeviceOpener>(config);
    PhysicalDiskSource source(3, opener);

    RECOVERY_ASSERT_OK(source.open());
    EXPECT_TRUE(source.isOpen());

    const auto calls = opener->calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].path.native(), L"\\\\.\\PhysicalDrive3");
    EXPECT_EQ(calls[0].kind, DeviceKind::PhysicalDisk);
}

TEST_F(PhysicalDiskSourceTest, OpenIsIdempotent) {
    auto opener = std::make_shared<MockDeviceOpener>(makeConfig(1 * kMiB));
    PhysicalDiskSource source(0, opener);
    RECOVERY_ASSERT_OK(source.open());
    RECOVERY_ASSERT_OK(source.open());
    EXPECT_EQ(opener->calls().size(), 1u);
}

TEST_F(PhysicalDiskSourceTest, RejectsOutOfRangeDiskNumberWithoutOpening) {
    auto opener = std::make_shared<MockDeviceOpener>(makeConfig(1 * kMiB));
    PhysicalDiskSource source(PhysicalDiskSource::kMaxDiskNumber + 1, opener);
    RECOVERY_EXPECT_ERROR(source.open(), ErrorCode::InvalidInput);
    EXPECT_TRUE(opener->calls().empty());
    EXPECT_FALSE(source.isOpen());
}

TEST_F(PhysicalDiskSourceTest, PropagatesOpenFailure) {
    auto opener = std::make_shared<MockDeviceOpener>(makeConfig(1 * kMiB));
    opener->failOpenWith(makeError(ErrorCode::IoError, "access denied", 5));
    PhysicalDiskSource source(1, opener);

    const Status status = source.open();
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error().code, ErrorCode::IoError);
    EXPECT_EQ(status.error().systemErrorCode, 5u);
    EXPECT_FALSE(source.isOpen());
}

TEST_F(PhysicalDiskSourceTest, PropagatesGeometryFailure) {
    auto config = makeConfig(1 * kMiB);
    config->geometryError = makeError(ErrorCode::IoError, "IOCTL failed", 1);
    PhysicalDiskSource source(1, std::make_shared<MockDeviceOpener>(config));
    RECOVERY_EXPECT_ERROR(source.open(), ErrorCode::IoError);
    EXPECT_FALSE(source.isOpen());
}

struct BadGeometryCase {
    const char* name;
    std::uint64_t size;
    std::uint32_t logical;
    std::uint32_t physical;
    std::uint32_t alignment;
};

class PhysicalDiskBadGeometryTest : public ::testing::TestWithParam<BadGeometryCase> {};

TEST_P(PhysicalDiskBadGeometryTest, IsRejected) {
    const BadGeometryCase& c = GetParam();
    auto config = std::make_shared<FakeDeviceConfig>();
    config->geometry.sizeBytes = c.size;
    config->geometry.logicalSectorSize = c.logical;
    config->geometry.physicalSectorSize = c.physical;
    config->geometry.requiredAlignment = c.alignment;
    PhysicalDiskSource source(0, std::make_shared<MockDeviceOpener>(config));

    RECOVERY_EXPECT_ERROR(source.open(), ErrorCode::IoError);
    EXPECT_FALSE(source.isOpen());
    EXPECT_EQ(source.size(), 0u);
}

INSTANTIATE_TEST_SUITE_P(
    Geometry, PhysicalDiskBadGeometryTest,
    ::testing::Values(BadGeometryCase{"ZeroSectorSize", 1 << 20, 0, 0, 512},
                      BadGeometryCase{"TinySectorSize", 1 << 20, 256, 256, 256},
                      BadGeometryCase{"NonPowerOfTwoSector", 520 * 100, 520, 520, 520},
                      BadGeometryCase{"HugeSectorSize", 1 << 20, 1 << 17, 1 << 17, 1 << 17},
                      BadGeometryCase{"ZeroSize", 0, 512, 512, 512},
                      BadGeometryCase{"SizeNotSectorMultiple", 1000, 512, 512, 512},
                      BadGeometryCase{"BadPhysicalSector", 1 << 20, 512, 3000, 512},
                      BadGeometryCase{"NonPowerOfTwoAlignment", 1 << 20, 512, 512, 768},
                      BadGeometryCase{"SizeBeyondAddressableRange", 0x8000000000000000ULL, 512, 512, 512}),
    [](const auto& info) { return std::string(info.param.name); });

TEST_F(PhysicalDiskSourceTest, ReportsInfo) {
    auto config = makeConfig(2 * kMiB, 4096);
    config->geometry.logicalSectorSize = 512;
    config->geometry.physicalSectorSize = 4096;
    config->geometry.requiredAlignment = 512;
    config->description = DeviceDescription{"Vendor", "USB Flash", true};
    PhysicalDiskSource source(2, std::make_shared<MockDeviceOpener>(config));
    RECOVERY_ASSERT_OK(source.open());

    const SourceInfo info = source.getInfo();
    EXPECT_EQ(info.type, SourceType::PhysicalDisk);
    EXPECT_EQ(info.sizeBytes, 2 * kMiB);
    EXPECT_EQ(info.logicalSectorSize, 512u);
    EXPECT_EQ(info.physicalSectorSize, 4096u);
    EXPECT_TRUE(info.readOnly);
    ASSERT_TRUE(info.diskNumber.has_value());
    EXPECT_EQ(*info.diskNumber, 2u);
    EXPECT_EQ(info.vendor, "Vendor");
    EXPECT_EQ(info.product, "USB Flash");
    EXPECT_EQ(info.removable, true);
}

TEST_F(PhysicalDiskSourceTest, AlignmentIsAtLeastTheSectorSize) {
    auto config = makeConfig(1 * kMiB, 4096);
    config->geometry.requiredAlignment = 1;  // driver under-reports...
    config->enforcedAlignment = 4096;        // ...but the hardware still needs sector alignment
    auto opener = std::make_shared<MockDeviceOpener>(config);
    PhysicalDiskSource source(0, opener);
    RECOVERY_ASSERT_OK(source.open());

    std::vector<std::byte> buffer(10);
    RECOVERY_ASSERT_OK(source.readExact(ByteOffset{4097}, buffer));
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), config->data.begin() + 4097));
    EXPECT_EQ(opener->stats()->alignmentViolations.load(), 0u);
}

TEST_F(PhysicalDiskSourceTest, UnalignedReadsAreServedWithAlignedDeviceRequests) {
    for (const std::uint32_t sectorSize : {512U, 4096U}) {
        SCOPED_TRACE(sectorSize);
        auto config = makeConfig(3 * kMiB, sectorSize);
        auto opener = std::make_shared<MockDeviceOpener>(config);
        PhysicalDiskSource source(0, opener);
        RECOVERY_ASSERT_OK(source.open());

        std::mt19937_64 random(42);
        for (int i = 0; i < 300; ++i) {
            const std::size_t length = 1 + random() % (2 * kMiB + 100);
            const std::uint64_t offset = random() % (config->data.size() - length + 1);
            std::vector<std::byte> buffer(length);
            RECOVERY_ASSERT_OK(source.readExact(ByteOffset{offset}, buffer));
            ASSERT_TRUE(std::equal(buffer.begin(), buffer.end(), config->data.begin() + static_cast<std::ptrdiff_t>(offset)))
                << "offset " << offset << " length " << length;
        }
        EXPECT_EQ(opener->stats()->alignmentViolations.load(), 0u);
    }
}

TEST_F(PhysicalDiskSourceTest, ReadEndingExactlyAtDiskEnd) {
    auto config = makeConfig(1 * kMiB, 4096);
    PhysicalDiskSource source(0, std::make_shared<MockDeviceOpener>(config));
    RECOVERY_ASSERT_OK(source.open());
    std::vector<std::byte> buffer(3);
    RECOVERY_ASSERT_OK(source.readExact(ByteOffset{config->data.size() - 3}, buffer));
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), config->data.end() - 3));
}

TEST_F(PhysicalDiskSourceTest, LargeReadsAreSplitIntoBoundedDeviceRequests) {
    auto config = makeConfig(40 * kMiB);
    auto opener = std::make_shared<MockDeviceOpener>(config);
    PhysicalDiskSource source(0, opener);
    RECOVERY_ASSERT_OK(source.open());

    std::vector<std::byte> buffer(kMaxReadSize - 24 * kMiB);  // 40 MiB
    RECOVERY_ASSERT_OK(source.readExact(ByteOffset{0}, buffer));
    EXPECT_EQ(buffer, config->data);
    EXPECT_GE(opener->stats()->reads.load(), 3u);
}

TEST_F(PhysicalDiskSourceTest, DeviceErrorReportsGoodPrefix) {
    auto config = makeConfig(1 * kMiB);
    config->failingRanges = {{8192, 8192 + 512}};
    config->failureCode = 1117;  // ERROR_IO_DEVICE
    PhysicalDiskSource source(0, std::make_shared<MockDeviceOpener>(config));
    RECOVERY_ASSERT_OK(source.open());

    std::vector<std::byte> buffer(8192);
    const ReadResult aligned = source.read(ByteOffset{4096}, buffer);
    EXPECT_EQ(aligned.status, ReadStatus::IoError);
    EXPECT_EQ(aligned.systemErrorCode, 1117u);
    EXPECT_EQ(aligned.bytesRead, 4096u);

    std::vector<std::byte> odd(8000);
    const ReadResult unaligned = source.read(ByteOffset{4100}, odd);
    EXPECT_EQ(unaligned.status, ReadStatus::IoError);
    EXPECT_EQ(unaligned.bytesRead, 8192u - 4100u);
    EXPECT_TRUE(std::equal(odd.begin(), odd.begin() + static_cast<std::ptrdiff_t>(unaligned.bytesRead),
                           config->data.begin() + 4100));
}

TEST_F(PhysicalDiskSourceTest, ShortDeviceReadIsPartialRead) {
    auto config = makeConfig(1 * kMiB);
    config->maxBytesPerRead = 1024;
    PhysicalDiskSource source(0, std::make_shared<MockDeviceOpener>(config));
    RECOVERY_ASSERT_OK(source.open());

    std::vector<std::byte> buffer(4096);
    const ReadResult result = source.read(ByteOffset{0}, buffer);
    EXPECT_EQ(result.status, ReadStatus::PartialRead);
    EXPECT_EQ(result.bytesRead, 1024u);
}

TEST_F(PhysicalDiskSourceTest, CloseAndReopen) {
    auto config = makeConfig(1 * kMiB);
    PhysicalDiskSource source(0, std::make_shared<MockDeviceOpener>(config));
    RECOVERY_ASSERT_OK(source.open());
    source.close();
    EXPECT_FALSE(source.isOpen());
    EXPECT_EQ(source.size(), 0u);
    EXPECT_EQ(source.sectorSize(), 0u);

    std::vector<std::byte> buffer(512);
    EXPECT_EQ(source.read(ByteOffset{0}, buffer).status, ReadStatus::NotOpen);

    RECOVERY_ASSERT_OK(source.open());
    EXPECT_TRUE(source.read(ByteOffset{0}, buffer).ok());
}

TEST(PhysicalDiskPathTest, DevicePathFormat) {
    EXPECT_EQ(PhysicalDiskSource::devicePathFor(0).native(), L"\\\\.\\PhysicalDrive0");
    EXPECT_EQ(PhysicalDiskSource::devicePathFor(12).native(), L"\\\\.\\PhysicalDrive12");
}

TEST(PhysicalDiskPathTest, ParseAcceptsValidPaths) {
    EXPECT_EQ(PhysicalDiskSource::parseDevicePath(L"\\\\.\\PhysicalDrive0"), 0u);
    EXPECT_EQ(PhysicalDiskSource::parseDevicePath(L"\\\\.\\physicaldrive7"), 7u);
    EXPECT_EQ(PhysicalDiskSource::parseDevicePath(L"\\\\.\\PHYSICALDRIVE1023"), 1023u);
}

TEST(PhysicalDiskPathTest, ParseRejectsInvalidPaths) {
    for (const wchar_t* path : {L"", L"\\\\.\\PhysicalDrive", L"\\\\.\\PhysicalDrive-1", L"\\\\.\\PhysicalDrive01",
                                L"\\\\.\\PhysicalDrive1024", L"\\\\.\\PhysicalDrive99999", L"\\\\.\\PhysicalDrive1x",
                                L"\\\\.\\C:", L"C:\\PhysicalDrive1", L"\\\\?\\PhysicalDrive1"}) {
        EXPECT_FALSE(PhysicalDiskSource::parseDevicePath(path).has_value()) << "accepted: " << testing::PrintToString(std::wstring(path));
    }
}

}  // namespace
}  // namespace recovery::storage
