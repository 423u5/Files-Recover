// DiskImageSource against real files through the Windows platform layer.
// All files live in a per-test temporary directory.

#include "storage/disk_image_source.hpp"

#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <fstream>

namespace recovery::storage {
namespace {

class DiskImageSourceTest : public ::testing::Test {
protected:
    std::filesystem::path createImage(std::size_t size, const char* name = "disk.img") {
        const std::filesystem::path path = dir_ / name;
        data_ = test::makePattern(size);
        test::writeFile(path, data_);
        return path;
    }

    test::TempDir dir_;
    std::vector<std::byte> data_;
};

TEST_F(DiskImageSourceTest, OpensAndReadsImage) {
    DiskImageSource source(createImage(256 * 1024));
    RECOVERY_ASSERT_OK(source.open());
    EXPECT_EQ(source.size(), data_.size());
    EXPECT_EQ(source.sectorSize(), 512u);

    std::vector<std::byte> buffer(data_.size());
    RECOVERY_ASSERT_OK(source.readExact(ByteOffset{0}, buffer));
    EXPECT_EQ(buffer, data_);
}

TEST_F(DiskImageSourceTest, ReportsInfo) {
    const auto path = createImage(8192);
    DiskImageSource source(path, DiskImageOptions{4096});
    RECOVERY_ASSERT_OK(source.open());

    const SourceInfo info = source.getInfo();
    EXPECT_EQ(info.type, SourceType::DiskImage);
    EXPECT_EQ(info.path, path);
    EXPECT_EQ(info.sizeBytes, 8192u);
    EXPECT_EQ(info.logicalSectorSize, 4096u);
    EXPECT_TRUE(info.readOnly);
    EXPECT_FALSE(info.diskNumber.has_value());
}

TEST_F(DiskImageSourceTest, MissingFileFailsWithSystemError) {
    DiskImageSource source(dir_ / "does-not-exist.img");
    const Status status = source.open();
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error().code, ErrorCode::IoError);
    EXPECT_EQ(status.error().systemErrorCode, 2u);  // ERROR_FILE_NOT_FOUND
    EXPECT_FALSE(source.isOpen());
}

TEST_F(DiskImageSourceTest, DirectoryIsRejected) {
    DiskImageSource source(dir_.path());
    EXPECT_FALSE(source.open().ok());
    EXPECT_FALSE(source.isOpen());
}

TEST_F(DiskImageSourceTest, DevicePathIsRejectedBeforeOpening) {
    DiskImageSource source(L"\\\\.\\PhysicalDrive0");
    RECOVERY_EXPECT_ERROR(source.open(), ErrorCode::InvalidInput);
}

TEST_F(DiskImageSourceTest, ReservedDeviceNameIsRejected) {
    DiskImageSource source(L"NUL");
    EXPECT_FALSE(source.open().ok());
}

TEST_F(DiskImageSourceTest, EmptyPathIsRejected) {
    DiskImageSource source{std::filesystem::path{}};
    RECOVERY_EXPECT_ERROR(source.open(), ErrorCode::InvalidInput);
}

TEST_F(DiskImageSourceTest, InvalidSectorSizeIsRejected) {
    DiskImageSource source(createImage(4096), DiskImageOptions{1000});
    RECOVERY_EXPECT_ERROR(source.open(), ErrorCode::InvalidInput);
}

TEST_F(DiskImageSourceTest, EmptyImageOpensWithZeroSize) {
    DiskImageSource source(createImage(0));
    RECOVERY_ASSERT_OK(source.open());
    EXPECT_EQ(source.size(), 0u);
    EXPECT_TRUE(source.read(ByteOffset{0}, std::span<std::byte>{}).ok());
    std::vector<std::byte> buffer(1);
    EXPECT_EQ(source.read(ByteOffset{0}, buffer).status, ReadStatus::OutOfRange);
}

TEST_F(DiskImageSourceTest, ImageEndingInPartialSector) {
    DiskImageSource source(createImage(512 * 3 + 100));
    RECOVERY_ASSERT_OK(source.open());

    std::vector<std::byte> tail(100);
    RECOVERY_ASSERT_OK(source.readExact(ByteOffset{512 * 3}, tail));
    EXPECT_TRUE(std::equal(tail.begin(), tail.end(), data_.begin() + 512 * 3));

    std::vector<std::byte> sector(512);
    EXPECT_TRUE(source.readSectors(SectorNumber{2}, SectorCount{1}, sector).ok());
    EXPECT_EQ(source.readSectors(SectorNumber{3}, SectorCount{1}, sector).status, ReadStatus::OutOfRange);
}

TEST_F(DiskImageSourceTest, ImageCannotBeModifiedWhileOpen) {
    const auto path = createImage(4096);
    DiskImageSource source(path);
    RECOVERY_ASSERT_OK(source.open());

    // The source shares read access only, so opening for write must fail.
    std::ofstream writer(path, std::ios::binary | std::ios::in | std::ios::out);
    EXPECT_FALSE(writer.is_open());

    source.close();
    std::ofstream afterClose(path, std::ios::binary | std::ios::in | std::ios::out);
    EXPECT_TRUE(afterClose.is_open());
}

TEST_F(DiskImageSourceTest, ReadingDoesNotModifyImage) {
    const auto path = createImage(512 * 1024);
    const auto writeTimeBefore = std::filesystem::last_write_time(path);
    {
        DiskImageSource source(path);
        RECOVERY_ASSERT_OK(source.open());
        std::vector<std::byte> buffer(4096);
        for (std::uint64_t offset = 0; offset + buffer.size() <= data_.size(); offset += 12345) {
            RECOVERY_ASSERT_OK(source.readExact(ByteOffset{offset}, buffer));
        }
    }
    EXPECT_EQ(std::filesystem::last_write_time(path), writeTimeBefore);
    EXPECT_EQ(test::readFile(path), data_);
}

TEST_F(DiskImageSourceTest, OpenFailsWhenAnotherWriterHoldsTheImage) {
    const auto path = createImage(4096);
    std::ofstream writer(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(writer.is_open());

    DiskImageSource source(path);
    const Status status = source.open();
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error().systemErrorCode, 32u);  // ERROR_SHARING_VIOLATION
}

}  // namespace
}  // namespace recovery::storage
