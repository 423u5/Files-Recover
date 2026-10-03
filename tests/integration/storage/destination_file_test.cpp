#include "storage/destination_file.hpp"
#include "storage/storage_source.hpp"

#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

namespace recovery::storage {
namespace {

using Mode = DestinationFile::OpenMode;

TEST(DestinationFileTest, CreatesWritesAndSizes) {
    const test::TempDir dir;
    Result<DestinationFile> file = DestinationFile::open(dir / "out.bin", Mode::CreateNew);
    RECOVERY_ASSERT_OK(file);

    const std::vector<std::byte> data = test::makePattern(100000);
    RECOVERY_ASSERT_OK(file->writeAt(0, data));
    RECOVERY_ASSERT_OK(file->writeAt(200000, std::span(data).first(10)));
    RECOVERY_ASSERT_OK(file->flush());
    EXPECT_EQ(file->size().value(), 200010u);

    RECOVERY_ASSERT_OK(file->truncate(100000));
    file->close();
    EXPECT_EQ(test::readFile(dir / "out.bin"), data);
}

TEST(DestinationFileTest, CreateNewNeverOverwrites) {
    const test::TempDir dir;
    const std::vector<std::byte> original = test::makePattern(64);
    test::writeFile(dir / "existing.bin", original);

    const Result<DestinationFile> file = DestinationFile::open(dir / "existing.bin", Mode::CreateNew);
    ASSERT_FALSE(file.ok());
    EXPECT_EQ(file.error().code, ErrorCode::DestinationError);
    EXPECT_EQ(file.error().systemErrorCode, 80u);  // ERROR_FILE_EXISTS
    EXPECT_EQ(test::readFile(dir / "existing.bin"), original);
}

TEST(DestinationFileTest, OpenExistingRequiresFile) {
    const test::TempDir dir;
    RECOVERY_EXPECT_ERROR(DestinationFile::open(dir / "missing.bin", Mode::OpenExisting), ErrorCode::DestinationError);
}

TEST(DestinationFileTest, RefusesDevicePaths) {
    for (const wchar_t* path : {L"\\\\.\\PhysicalDrive0", L"\\\\.\\C:", L"\\\\?\\GLOBALROOT\\Device\\Harddisk0\\DR0"}) {
        RECOVERY_EXPECT_ERROR(DestinationFile::open(path, Mode::OpenExisting), ErrorCode::DestinationError);
        RECOVERY_EXPECT_ERROR(DestinationFile::open(path, Mode::CreateNew), ErrorCode::DestinationError);
    }
}

TEST(DestinationFileTest, RefusesCharacterDevices) {
    RECOVERY_EXPECT_ERROR(DestinationFile::open(L"NUL", Mode::OpenExisting), ErrorCode::DestinationError);
}

TEST(DestinationFileTest, RejectsOverflowingWrite) {
    const test::TempDir dir;
    Result<DestinationFile> file = DestinationFile::open(dir / "out.bin", Mode::CreateNew);
    RECOVERY_ASSERT_OK(file);
    const std::vector<std::byte> data(16);
    RECOVERY_EXPECT_ERROR(file->writeAt(kMaxAddressableOffset - 4, data), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(file->writeAt(~0ULL, data), ErrorCode::InvalidInput);
}

TEST(DestinationFileTest, ClosedFileRejectsOperations) {
    DestinationFile file;
    const std::vector<std::byte> data(4);
    RECOVERY_EXPECT_ERROR(file.writeAt(0, data), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(file.flush(), ErrorCode::InvalidInput);
    EXPECT_FALSE(file.size().ok());
}

TEST(DestinationFileTest, ReplaceFileSwapsContent) {
    const test::TempDir dir;
    test::writeFile(dir / "a", test::makePattern(10, 1));
    test::writeFile(dir / "b", test::makePattern(20, 2));
    RECOVERY_ASSERT_OK(replaceFile(dir / "b", dir / "a"));
    EXPECT_EQ(test::readFile(dir / "a"), test::makePattern(20, 2));
    EXPECT_FALSE(std::filesystem::exists(dir / "b"));
}

}  // namespace
}  // namespace recovery::storage
