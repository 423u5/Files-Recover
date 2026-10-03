// Cross-checks the FAT32 parser against images written by other FAT
// implementations (Windows, mkfs.fat, pyfatfs, ...), which guards against
// the test builder and the parser sharing a misreading of the specification.
//
// Skipped unless RECOVERY_FAT32_REFERENCE_DIR names a directory holding pairs
// of "<name>.img" and "<name>.manifest". Manifest lines, tab-separated:
//   <absolute path>\t<size>\t<CRC-32 as 8 hex digits>   an active file
//   deleted\t<file name>                                 a deleted entry
// See docs/testing/testing.md.

#include "filesystem/fat32/fat32_filesystem.hpp"

#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/crc32.hpp"
#include "storage/disk_image_source.hpp"
#include "support/fat32_builder.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace recovery::filesystem::fat32 {
namespace {

std::vector<std::filesystem::path> referenceImages() {
    std::vector<std::filesystem::path> images;
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, L"RECOVERY_FAT32_REFERENCE_DIR") != 0 || value == nullptr) {
        return images;
    }
    const std::filesystem::path dir(value);
    std::free(value);
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
        if (item.path().extension() == ".img") {
            images.push_back(item.path());
        }
    }
    return images;
}

TEST(Fat32ReferenceImageTest, MatchesManifests) {
    const std::vector<std::filesystem::path> images = referenceImages();
    if (images.empty()) {
        GTEST_SKIP() << "set RECOVERY_FAT32_REFERENCE_DIR to run against externally created images";
    }
    for (const std::filesystem::path& imagePath : images) {
        SCOPED_TRACE(imagePath.string());
        storage::DiskImageSource device(imagePath);
        RECOVERY_ASSERT_OK(device.open());
        const Result<partition::PartitionTable> table = partition::readPartitionTable(device);
        RECOVERY_ASSERT_OK(table);
        const auto volume = std::find_if(table->partitions.begin(), table->partitions.end(),
                                         [](const partition::Partition& p) { return p.candidates.fat; });
        ASSERT_NE(volume, table->partitions.end()) << "no FAT partition found";
        partition::PartitionSource part(device, *volume);
        RECOVERY_ASSERT_OK(part.open());

        Result<std::unique_ptr<Fat32Filesystem>> fs = Fat32Filesystem::open(part);
        RECOVERY_ASSERT_OK(fs);
        const Result<FileScan> scan = fs.value()->scan({}, {});
        RECOVERY_ASSERT_OK(scan);
        EXPECT_TRUE(scan->issues.empty()) << scan->issues.front().detail;

        std::filesystem::path manifestPath = imagePath;
        manifestPath.replace_extension(".manifest");
        std::ifstream manifest(manifestPath);
        ASSERT_TRUE(manifest.is_open());
        std::string line;
        std::size_t checked = 0;
        while (std::getline(manifest, line)) {
            std::istringstream fields(line);
            std::string first;
            std::string second;
            std::string third;
            std::getline(fields, first, '\t');
            std::getline(fields, second, '\t');
            std::getline(fields, third, '\t');
            if (first == "deleted") {
                const bool found = std::any_of(scan->records.begin(), scan->records.end(), [&](const FileRecord& r) {
                    return r.entry.state == EntryState::Deleted && r.entry.name == second;
                });
                EXPECT_TRUE(found) << "deleted entry not found: " << second;
                ++checked;
                continue;
            }
            const auto record = std::find_if(scan->records.begin(), scan->records.end(),
                                             [&](const FileRecord& r) { return r.path == first; });
            ASSERT_NE(record, scan->records.end()) << "missing " << first;
            EXPECT_EQ(record->entry.state, EntryState::Active) << first;
            EXPECT_EQ(record->entry.size, std::stoull(second)) << first;
            EXPECT_TRUE(record->allocation.issues.empty()) << first;
            const std::vector<std::byte> data = test::readExtents(part, record->allocation.extents);
            EXPECT_EQ(crc32(data), static_cast<std::uint32_t>(std::stoul(third, nullptr, 16))) << first;
            ++checked;
        }
        EXPECT_GT(checked, 0u);
    }
}

}  // namespace
}  // namespace recovery::filesystem::fat32
