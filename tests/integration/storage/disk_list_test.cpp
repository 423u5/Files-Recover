// The Windows disk lister (P19) asks the system what disks it has, through
// handles that cannot read or change anything (desired access 0, no
// administrator rights): exercised against the real OS, like the resolver.

#include "storage/destination_guard.hpp"
#include "storage/disk_list.hpp"

#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace recovery::storage {
namespace {

TEST(DiskListTest, ListsTheDisksOfThisComputerByNumber) {
    const Result<std::vector<AttachedDisk>> disks = makePlatformDiskLister()();
    RECOVERY_ASSERT_OK(disks);
    ASSERT_FALSE(disks->empty()) << "a computer that runs the tests has a disk";
    for (std::size_t i = 1; i < disks->size(); ++i) {
        EXPECT_LT((*disks)[i - 1].number, (*disks)[i].number);
    }
    for (const AttachedDisk& disk : *disks) {
        EXPECT_LE(disk.number, 1023U);
        if (disk.sizeBytes != 0) {
            EXPECT_GT(disk.logicalSectorSize, 0U);
            EXPECT_EQ(disk.sizeBytes % disk.logicalSectorSize, 0U) << "disk " << disk.number;
        }
        for (const std::filesystem::path& volume : disk.volumes) {
            const std::wstring& root = volume.native();
            ASSERT_EQ(root.size(), 3U);
            EXPECT_EQ(root.substr(1), L":\\");
        }
    }
}

TEST(DiskListTest, TheDiskOfTheTemporaryFolderIsListed) {
    const test::TempDir dir;
    const Result<std::vector<std::uint32_t>> on = makePlatformDiskResolver()(dir.path());
    if (!on.ok() || on->empty()) {
        GTEST_SKIP() << "the temporary folder's disk cannot be resolved here";
    }
    const Result<std::vector<AttachedDisk>> disks = makePlatformDiskLister()();
    RECOVERY_ASSERT_OK(disks);
    const auto found = std::find_if(disks->begin(), disks->end(),
                                    [&](const AttachedDisk& disk) { return disk.number == on->front(); });
    ASSERT_NE(found, disks->end()) << "disk " << on->front();
    const std::filesystem::path root = dir.path().root_path();
    if (root.has_root_name() && root.native().size() == 3) {
        EXPECT_NE(std::find(found->volumes.begin(), found->volumes.end(), root), found->volumes.end())
            << "the drive " << root.string() << " is on disk " << found->number;
    }
}

}  // namespace
}  // namespace recovery::storage
