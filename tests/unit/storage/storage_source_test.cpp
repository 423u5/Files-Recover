// Request validation shared by every IStorageSource (implemented once in the
// non-virtual IStorageSource::read* functions).

#include "storage/storage_source.hpp"

#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace recovery::storage {
namespace {

using test::MemoryStorageSource;

constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();

class StorageSourceTest : public ::testing::Test {
protected:
    void SetUp() override { RECOVERY_ASSERT_OK(source_.open()); }

    std::vector<std::byte> data_ = test::makePattern(64 * 1024);
    MemoryStorageSource source_{data_, 512};
};

TEST_F(StorageSourceTest, ValidReadReturnsData) {
    std::vector<std::byte> buffer(1000);
    const ReadResult result = source_.read(ByteOffset{1234}, buffer);
    ASSERT_TRUE(result.ok()) << toString(result.status);
    EXPECT_EQ(result.bytesRead, buffer.size());
    EXPECT_EQ(result.requestedBytes, buffer.size());
    EXPECT_EQ(result.offset, 1234u);
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), data_.begin() + 1234));
}

TEST_F(StorageSourceTest, ReadOfEntireSourceSucceeds) {
    std::vector<std::byte> buffer(data_.size());
    RECOVERY_ASSERT_OK(source_.readExact(ByteOffset{0}, buffer));
    EXPECT_EQ(buffer, data_);
}

TEST_F(StorageSourceTest, ZeroLengthReadsSucceedInsideAndAtEnd) {
    const std::span<std::byte> empty;
    EXPECT_TRUE(source_.read(ByteOffset{0}, empty).ok());
    EXPECT_TRUE(source_.read(ByteOffset{100}, empty).ok());
    EXPECT_TRUE(source_.read(ByteOffset{data_.size()}, empty).ok());
    EXPECT_EQ(source_.readCount(), 0u) << "zero-length reads must not reach the device";
}

TEST_F(StorageSourceTest, ZeroLengthReadPastEndIsOutOfRange) {
    const ReadResult result = source_.read(ByteOffset{data_.size() + 1}, std::span<std::byte>{});
    EXPECT_EQ(result.status, ReadStatus::OutOfRange);
}

TEST_F(StorageSourceTest, ReadStartingPastEndIsOutOfRange) {
    std::vector<std::byte> buffer(16);
    const ReadResult result = source_.read(ByteOffset{data_.size() + 512}, buffer);
    EXPECT_EQ(result.status, ReadStatus::OutOfRange);
    EXPECT_EQ(result.bytesRead, 0u);
    EXPECT_EQ(source_.readCount(), 0u);
}

TEST_F(StorageSourceTest, ReadCrossingEndIsRejectedNotTruncated) {
    std::vector<std::byte> buffer(100);
    const ReadResult result = source_.read(ByteOffset{data_.size() - 50}, buffer);
    EXPECT_EQ(result.status, ReadStatus::OutOfRange);
    EXPECT_EQ(result.bytesRead, 0u);
}

TEST_F(StorageSourceTest, OffsetPlusLengthOverflowIsRejected) {
    std::vector<std::byte> buffer(16);
    EXPECT_EQ(source_.read(ByteOffset{kU64Max}, buffer).status, ReadStatus::InvalidArgument);
    EXPECT_EQ(source_.read(ByteOffset{kU64Max - 8}, buffer).status, ReadStatus::InvalidArgument);
}

TEST_F(StorageSourceTest, OffsetsThatWouldBeNegativeForWindowsAreRejected) {
    // 2^63 is negative as a LARGE_INTEGER.
    std::vector<std::byte> buffer(1);
    const ReadResult result = source_.read(ByteOffset{kMaxAddressableOffset + 1}, buffer);
    EXPECT_EQ(result.status, ReadStatus::InvalidArgument);
}

TEST_F(StorageSourceTest, OversizedReadIsRejectedBeforeReachingDevice) {
    std::vector<std::byte> buffer(kMaxReadSize + 1);
    const ReadResult result = source_.read(ByteOffset{0}, buffer);
    EXPECT_EQ(result.status, ReadStatus::InvalidArgument);
    EXPECT_EQ(source_.readCount(), 0u);
}

TEST_F(StorageSourceTest, ReadOnClosedSourceFails) {
    source_.close();
    std::vector<std::byte> buffer(16);
    EXPECT_EQ(source_.read(ByteOffset{0}, buffer).status, ReadStatus::NotOpen);
    EXPECT_EQ(source_.readSectors(SectorNumber{0}, SectorCount{1}, buffer).status, ReadStatus::NotOpen);
    RECOVERY_EXPECT_ERROR(source_.readExact(ByteOffset{0}, buffer), ErrorCode::InvalidInput);
    EXPECT_EQ(source_.size(), 0u);
}

TEST_F(StorageSourceTest, PartialReadIsDetected) {
    source_.setShortReadLimit(100);
    std::vector<std::byte> buffer(512);
    const ReadResult result = source_.read(ByteOffset{0}, buffer);
    EXPECT_EQ(result.status, ReadStatus::PartialRead);
    EXPECT_EQ(result.bytesRead, 100u);
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.begin() + 100, data_.begin()));
}

TEST_F(StorageSourceTest, ReadExactTreatsPartialReadAsError) {
    source_.setShortReadLimit(100);
    std::vector<std::byte> buffer(512);
    RECOVERY_EXPECT_ERROR(source_.readExact(ByteOffset{0}, buffer), ErrorCode::IoError);
}

TEST_F(StorageSourceTest, IoErrorCarriesSystemCodeAndGoodPrefix) {
    source_.addBadSector(3, MemoryStorageSource::kErrorSectorNotFound);
    std::vector<std::byte> buffer(4 * 512);
    const ReadResult result = source_.read(ByteOffset{512}, buffer);
    EXPECT_EQ(result.status, ReadStatus::IoError);
    EXPECT_EQ(result.systemErrorCode, MemoryStorageSource::kErrorSectorNotFound);
    EXPECT_EQ(result.bytesRead, 2u * 512u);

    const Status exact = source_.readExact(ByteOffset{512}, buffer);
    ASSERT_FALSE(exact.ok());
    EXPECT_EQ(exact.error().code, ErrorCode::IoError);
    EXPECT_EQ(exact.error().systemErrorCode, MemoryStorageSource::kErrorSectorNotFound);
}

TEST_F(StorageSourceTest, ReadSectorsReadsWholeSectors) {
    std::vector<std::byte> buffer(3 * 512);
    const ReadResult result = source_.readSectors(SectorNumber{4}, SectorCount{3}, buffer);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.offset, 4u * 512u);
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), data_.begin() + 4 * 512));
}

TEST_F(StorageSourceTest, ReadSectorsUsesOnlyNeededPartOfLargerBuffer) {
    std::vector<std::byte> buffer(4 * 512, std::byte{0xAA});
    const ReadResult result = source_.readSectors(SectorNumber{0}, SectorCount{1}, buffer);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.bytesRead, 512u);
    EXPECT_EQ(buffer[512], std::byte{0xAA});
}

TEST_F(StorageSourceTest, ReadSectorsRejectsSmallBuffer) {
    std::vector<std::byte> buffer(511);
    EXPECT_EQ(source_.readSectors(SectorNumber{0}, SectorCount{1}, buffer).status, ReadStatus::InvalidArgument);
}

TEST_F(StorageSourceTest, ReadSectorsRejectsArithmeticOverflow) {
    std::vector<std::byte> buffer(512);
    EXPECT_EQ(source_.readSectors(SectorNumber{kU64Max}, SectorCount{1}, buffer).status,
              ReadStatus::InvalidArgument);
    EXPECT_EQ(source_.readSectors(SectorNumber{0}, SectorCount{kU64Max}, buffer).status,
              ReadStatus::InvalidArgument);
    EXPECT_EQ(source_.readSectors(SectorNumber{kU64Max / 512}, SectorCount{1}, buffer).status,
              ReadStatus::InvalidArgument);
}

TEST_F(StorageSourceTest, ReadSectorsPastEndIsOutOfRange) {
    std::vector<std::byte> buffer(2 * 512);
    const std::uint64_t lastSector = data_.size() / 512 - 1;
    EXPECT_TRUE(source_.readSectors(SectorNumber{lastSector}, SectorCount{1}, buffer).ok());
    EXPECT_EQ(source_.readSectors(SectorNumber{lastSector}, SectorCount{2}, buffer).status, ReadStatus::OutOfRange);
}

// An implementation that claims to have read more than it was asked for.
class LyingSource final : public IStorageSource {
public:
    Status open() override { return success(); }
    void close() noexcept override {}
    bool isOpen() const noexcept override { return true; }
    std::uint64_t size() const noexcept override { return 4096; }
    std::uint32_t sectorSize() const noexcept override { return 512; }
    SourceInfo getInfo() const override { return {}; }

protected:
    ReadResult readValidated(std::uint64_t, std::span<std::byte> buffer) override {
        ReadResult result;
        result.status = ReadStatus::Success;
        result.bytesRead = buffer.size() + 1;
        result.requestedBytes = 1;  // also lies about the request
        return result;
    }
};

TEST(StorageSourceContractTest, ImplementationOverReportingIsCaught) {
    LyingSource source;
    std::vector<std::byte> buffer(16);
    const ReadResult result = source.read(ByteOffset{0}, buffer);
    EXPECT_EQ(result.status, ReadStatus::InternalError);
    EXPECT_EQ(result.bytesRead, 0u);
    EXPECT_EQ(result.requestedBytes, 16u);
}

TEST(StorageSourceHelpersTest, SectorSizeValidation) {
    EXPECT_TRUE(isValidSectorSize(512));
    EXPECT_TRUE(isValidSectorSize(4096));
    EXPECT_TRUE(isValidSectorSize(65536));
    EXPECT_FALSE(isValidSectorSize(0));
    EXPECT_FALSE(isValidSectorSize(256));
    EXPECT_FALSE(isValidSectorSize(520));
    EXPECT_FALSE(isValidSectorSize(131072));
}

TEST(StorageSourceHelpersTest, DeviceNamespacePathDetection) {
    EXPECT_TRUE(isDeviceNamespacePath(L"\\\\.\\PhysicalDrive0"));
    EXPECT_TRUE(isDeviceNamespacePath(L"//./PhysicalDrive1"));
    EXPECT_TRUE(isDeviceNamespacePath(L"\\\\.\\E:"));
    EXPECT_TRUE(isDeviceNamespacePath(L"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\"));
    EXPECT_TRUE(isDeviceNamespacePath(L"\\\\?\\GLOBALROOT\\Device\\Harddisk1\\Partition0"));
    EXPECT_TRUE(isDeviceNamespacePath(L"\\??\\PhysicalDrive0"));

    EXPECT_FALSE(isDeviceNamespacePath(L"C:\\images\\disk.img"));
    EXPECT_FALSE(isDeviceNamespacePath(L"\\\\?\\C:\\images\\disk.img"));
    EXPECT_FALSE(isDeviceNamespacePath(L"\\\\?\\UNC\\server\\share\\disk.img"));
    EXPECT_FALSE(isDeviceNamespacePath(L"\\\\server\\share\\disk.img"));
    EXPECT_FALSE(isDeviceNamespacePath(L"relative\\disk.img"));
}

TEST(StorageSourceHelpersTest, ToErrorMapsStatuses) {
    ReadResult result;
    result.status = ReadStatus::IoError;
    result.systemErrorCode = 1117;
    EXPECT_EQ(toError(result).code, ErrorCode::IoError);
    EXPECT_EQ(toError(result).systemErrorCode, 1117u);

    result.status = ReadStatus::OutOfRange;
    EXPECT_EQ(toError(result).code, ErrorCode::InvalidInput);
    result.status = ReadStatus::PartialRead;
    EXPECT_EQ(toError(result).code, ErrorCode::IoError);
    result.status = ReadStatus::InternalError;
    EXPECT_EQ(toError(result).code, ErrorCode::InternalError);
}

TEST(StorageSourceHelpersTest, SourceTypeRoundTrips) {
    for (const SourceType type : {SourceType::PhysicalDisk, SourceType::DiskImage, SourceType::Synthetic}) {
        EXPECT_EQ(parseSourceType(toString(type)), type);
    }
    EXPECT_FALSE(parseSourceType("physicaldisk").has_value());
}

}  // namespace
}  // namespace recovery::storage
