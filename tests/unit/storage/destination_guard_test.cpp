#include "storage/destination_guard.hpp"

#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

namespace recovery::storage {
namespace {

DiskResolver resolverReturning(std::vector<std::uint32_t> disks) {
    return [disks](const std::filesystem::path&) -> Result<std::vector<std::uint32_t>> { return disks; };
}

SourceInfo physicalSource(std::uint32_t disk) {
    SourceInfo info;
    info.type = SourceType::PhysicalDisk;
    info.path = L"\\\\.\\PhysicalDrive" + std::to_wstring(disk);
    info.diskNumber = disk;
    return info;
}

TEST(DestinationGuardTest, RejectsDevicePaths) {
    const SourceInfo source = physicalSource(1);
    for (const wchar_t* path : {L"\\\\.\\PhysicalDrive2", L"\\\\.\\E:", L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}"}) {
        RECOVERY_EXPECT_ERROR(checkDestinationSafety(source, path, resolverReturning({0})),
                              ErrorCode::DestinationError);
    }
}

TEST(DestinationGuardTest, RejectsEmptyPath) {
    RECOVERY_EXPECT_ERROR(checkDestinationSafety(physicalSource(1), {}, resolverReturning({0})),
                          ErrorCode::InvalidInput);
}

TEST(DestinationGuardTest, RejectsDestinationOnSourceDisk) {
    const test::TempDir dir;
    RECOVERY_EXPECT_ERROR(checkDestinationSafety(physicalSource(1), dir / "image.img", resolverReturning({1})),
                          ErrorCode::DestinationError);
    // Volume spanning several disks, one of which is the source.
    RECOVERY_EXPECT_ERROR(checkDestinationSafety(physicalSource(1), dir / "image.img", resolverReturning({0, 1})),
                          ErrorCode::DestinationError);
}

TEST(DestinationGuardTest, AcceptsDestinationOnOtherDisk) {
    const test::TempDir dir;
    RECOVERY_EXPECT_OK(checkDestinationSafety(physicalSource(1), dir / "sub" / "image.img", resolverReturning({0})));
}

TEST(DestinationGuardTest, FailsClosedWhenDiskCannotBeDetermined) {
    const test::TempDir dir;
    const DiskResolver failing = [](const std::filesystem::path&) -> Result<std::vector<std::uint32_t>> {
        return makeError(ErrorCode::IoError, "network share", 1);
    };
    RECOVERY_EXPECT_ERROR(checkDestinationSafety(physicalSource(1), dir / "image.img", failing),
                          ErrorCode::DestinationError);
    RECOVERY_EXPECT_ERROR(checkDestinationSafety(physicalSource(1), dir / "image.img", DiskResolver{}),
                          ErrorCode::DestinationError);
}

TEST(DestinationGuardTest, ResolverReceivesExistingAncestor) {
    const test::TempDir dir;
    std::filesystem::path seen;
    const DiskResolver recording = [&seen](const std::filesystem::path& path) -> Result<std::vector<std::uint32_t>> {
        seen = path;
        return std::vector<std::uint32_t>{0};
    };
    RECOVERY_ASSERT_OK(checkDestinationSafety(physicalSource(1), dir / "a" / "b" / "image.img", recording));
    EXPECT_EQ(seen, dir.path());
}

TEST(DestinationGuardTest, RejectsImageSourceAsDestination) {
    const test::TempDir dir;
    const auto image = dir / "source.img";
    test::writeFile(image, test::makePattern(512));

    SourceInfo source;
    source.type = SourceType::DiskImage;
    source.path = image;

    RECOVERY_EXPECT_ERROR(checkDestinationSafety(source, image, {}), ErrorCode::DestinationError);
    // Different spelling of the same file.
    std::filesystem::path upper = image.parent_path() / L"SOURCE.IMG";
    RECOVERY_EXPECT_ERROR(checkDestinationSafety(source, upper, {}), ErrorCode::DestinationError);
    std::filesystem::path dotted = image.parent_path() / L"." / L"source.img";
    RECOVERY_EXPECT_ERROR(checkDestinationSafety(source, dotted, {}), ErrorCode::DestinationError);

    RECOVERY_EXPECT_OK(checkDestinationSafety(source, dir / "copy.img", {}));
}

}  // namespace
}  // namespace recovery::storage
