// The Windows disk resolver is the last line of defence against writing an
// image onto the disk being imaged, so it is exercised against the real OS.

#include "storage/destination_guard.hpp"

#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace recovery::storage {
namespace {

// Creates a directory junction (no administrator rights needed).
bool createJunction(const std::filesystem::path& link, const std::filesystem::path& target) {
    const std::wstring command = L"cmd /c mklink /J \"" + link.native() + L"\" \"" + target.native() + L"\" >nul 2>&1";
    return _wsystem(command.c_str()) == 0 && std::filesystem::exists(link);
}

TEST(DiskResolverTest, ResolvesLocalDirectory) {
    const test::TempDir dir;
    const Result<std::vector<std::uint32_t>> disks = makePlatformDiskResolver()(dir.path());
    RECOVERY_ASSERT_OK(disks);
    EXPECT_FALSE(disks->empty());
}

TEST(DiskResolverTest, FailsForNonexistentVolume) {
    // A drive letter that is essentially never assigned.
    const Result<std::vector<std::uint32_t>> disks = makePlatformDiskResolver()(L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\");
    EXPECT_FALSE(disks.ok());
}

TEST(DiskResolverTest, FollowsJunctionsToTheirTargetVolume) {
    const test::TempDir dir;
    const std::filesystem::path target = std::filesystem::current_path();
    const std::filesystem::path link = dir / "link";
    if (!createJunction(link, target)) {
        GTEST_SKIP() << "cannot create a directory junction here";
    }

    const DiskResolver resolver = makePlatformDiskResolver();
    const Result<std::vector<std::uint32_t>> viaLink = resolver(link);
    const Result<std::vector<std::uint32_t>> direct = resolver(target);
    RECOVERY_ASSERT_OK(viaLink);
    RECOVERY_ASSERT_OK(direct);
    EXPECT_EQ(viaLink.value(), direct.value());

    // A destination reached through the junction is attributed to the target's
    // disk, so it is refused when that disk is the source.
    SourceInfo source;
    source.type = SourceType::PhysicalDisk;
    source.diskNumber = direct->front();
    RECOVERY_EXPECT_ERROR(checkDestinationSafety(source, link / "image.img", resolver), ErrorCode::DestinationError);

    std::filesystem::remove(link);  // removes the junction, not the target
}

}  // namespace
}  // namespace recovery::storage
