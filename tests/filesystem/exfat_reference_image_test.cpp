// Cross-checks against other exFAT implementations, which guards against the
// test builder and the parser sharing a misreading of the specification.
//
// ExFatReferenceImageTest reads images written by other software (Windows,
// mkfs.exfat, ...). It is skipped unless RECOVERY_EXFAT_REFERENCE_DIR names a
// directory holding pairs of "<name>.img" and "<name>.manifest". Manifest
// lines, tab-separated:
//   label\t<volume label>
//   cluster_size\t<bytes>
//   <absolute path>\t<size>\t<CRC-32 as 8 hex digits>   an active file
//   deleted\t<file name>                                 a deleted entry
//
// ExFatBuilderExport writes the builder's images to RECOVERY_EXFAT_EXPORT_DIR
// (skipped otherwise), so that another implementation's checker
// (fsck.exfat) can verify them. See docs/testing/testing.md.

#include "filesystem/exfat/exfat_filesystem.hpp"

#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/crc32.hpp"
#include "storage/disk_image_source.hpp"
#include "support/exfat_builder.hpp"
#include "support/fat32_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace recovery::filesystem::exfat {
namespace {

std::optional<std::filesystem::path> directoryFromEnvironment(const wchar_t* name) {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path dir(value);
    std::free(value);
    return dir;
}

std::vector<std::filesystem::path> referenceImages() {
    std::vector<std::filesystem::path> images;
    const std::optional<std::filesystem::path> dir = directoryFromEnvironment(L"RECOVERY_EXFAT_REFERENCE_DIR");
    if (!dir) {
        return images;
    }
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(*dir, ec)) {
        if (item.path().extension() == ".img") {
            images.push_back(item.path());
        }
    }
    return images;
}

TEST(ExFatReferenceImageTest, MatchesManifests) {
    const std::vector<std::filesystem::path> images = referenceImages();
    if (images.empty()) {
        GTEST_SKIP() << "set RECOVERY_EXFAT_REFERENCE_DIR to run against externally created images";
    }
    for (const std::filesystem::path& imagePath : images) {
        SCOPED_TRACE(imagePath.string());
        storage::DiskImageSource device(imagePath);
        RECOVERY_ASSERT_OK(device.open());
        const Result<partition::PartitionTable> table = partition::readPartitionTable(device);
        RECOVERY_ASSERT_OK(table);
        const auto volume = std::find_if(table->partitions.begin(), table->partitions.end(),
                                         [](const partition::Partition& p) { return p.candidates.exfat; });
        ASSERT_NE(volume, table->partitions.end()) << "no exFAT partition found";
        partition::PartitionSource part(device, *volume);
        RECOVERY_ASSERT_OK(part.open());

        Result<std::unique_ptr<ExFatFilesystem>> fs = ExFatFilesystem::open(part);
        RECOVERY_ASSERT_OK(fs);
        ExFatFilesystem& exfat = *fs.value();
        EXPECT_TRUE(exfat.info().warnings.empty()) << exfat.info().warnings.front();
        EXPECT_TRUE(exfat.hasAllocationBitmap());
        EXPECT_TRUE(exfat.usesVolumeUpcaseTable());
        const Result<FileScan> scan = exfat.scan({}, {});
        RECOVERY_ASSERT_OK(scan);
        EXPECT_TRUE(scan->issues.empty()) << scan->issues.front().detail;
        EXPECT_EQ(scan->crossLinkedClusters, 0u);
        const Result<ClusterUsage> usage = exfat.analyzeClusters({});
        RECOVERY_ASSERT_OK(usage);
        // Every allocated cluster belongs to a system structure, a directory or an active file.
        EXPECT_EQ(usage->allocated, scan->referencedClusters);

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
            ++checked;
            if (first == "label") {
                EXPECT_EQ(exfat.info().label, second);
                continue;
            }
            if (first == "cluster_size") {
                EXPECT_EQ(exfat.info().clusterSize, std::stoul(second));
                continue;
            }
            if (first == "deleted") {
                const bool found = std::any_of(scan->records.begin(), scan->records.end(), [&](const FileRecord& r) {
                    return r.entry.state == EntryState::Deleted && r.entry.name == second;
                });
                EXPECT_TRUE(found) << "deleted entry not found: " << second;
                continue;
            }
            const auto record = std::find_if(scan->records.begin(), scan->records.end(),
                                             [&](const FileRecord& r) { return r.path == first; });
            ASSERT_NE(record, scan->records.end()) << "missing " << first;
            EXPECT_EQ(record->entry.state, EntryState::Active) << first;
            EXPECT_TRUE(record->entry.issues.empty()) << first;
            EXPECT_EQ(record->entry.size, std::stoull(second)) << first;
            EXPECT_TRUE(record->allocation.issues.empty()) << first;
            const std::vector<std::byte> data = test::readExtents(part, record->allocation.extents);
            EXPECT_EQ(crc32(data), static_cast<std::uint32_t>(std::stoul(third, nullptr, 16))) << first;
        }
        EXPECT_GT(checked, 0u);
    }
}

// ---------------------------------------------------------------------------
// Builder images for external checking.
// ---------------------------------------------------------------------------

void exportImage(const std::filesystem::path& dir, const std::string& name, test::ExFatImageBuilder& builder) {
    test::writeFile(dir / (name + ".img"), builder.build());
}

TEST(ExFatBuilderExport, WritesImages) {
    const std::optional<std::filesystem::path> dir = directoryFromEnvironment(L"RECOVERY_EXFAT_EXPORT_DIR");
    if (!dir) {
        GTEST_SKIP() << "set RECOVERY_EXFAT_EXPORT_DIR to export builder images for fsck.exfat";
    }
    std::filesystem::create_directories(*dir);

    {
        test::ExFatImageBuilder builder;
        const auto dcim = builder.addDirectory(builder.root(), "DCIM");
        const auto camera = builder.addDirectory(dcim.clusters[0], "100MEDIA");
        (void)builder.addFile(camera.clusters[0], "IMG_0001.JPG", test::makePattern(5000, 1));
        (void)builder.addFile(builder.root(), "small.txt", test::makePattern(10, 2));
        (void)builder.addFile(builder.root(), "empty.bin", {});
        (void)builder.addFileInClusters(builder.root(), "fragmented.bin", test::makePattern(5 * 512 - 7, 3),
                                        {300, 301, 350, 351, 320});
        const auto gone = builder.addFile(builder.root(), "deleted photo.jpg", test::makePattern(3000, 5));
        builder.deleteEntry(gone);
        // Not exported: vendor extension entries. The specification allows
        // them, but fsck.exfat 1.2.2 reports them as missing name entries.
        exportImage(*dir, "builder-basic", builder);
    }
    {
        test::ExFatImageBuilder builder;
        (void)builder.addFile(builder.root(),
                              "\xC3\x89t\xC3\xA9 \xCE\xB1\xCE\xB2\xCE\xB3 \xD0\xB4\xD0\xB0 \xF0\x9F\x98\x80.jpeg",
                              test::makePattern(900, 6));
        (void)builder.addFile(builder.root(), std::string(255, 'n'), test::makePattern(10, 7));
        const auto folder = builder.addDirectory(builder.root(), "Many files");
        (void)builder.addFile(builder.root(), "blocker.bin",
                              test::makePattern(512, 8));  // forces a FAT chain on growth
        for (int i = 0; i < 40; ++i) {
            (void)builder.addFile(folder.clusters[0], "photo number " + std::to_string(i) + ".jpg",
                                  test::makePattern(100, static_cast<std::uint64_t>(i)));
        }
        exportImage(*dir, "builder-names", builder);
    }
    for (const auto& [bytesShift, clusterShift] : {std::pair{9, 3}, std::pair{12, 0}, std::pair{12, 3}}) {
        test::ExFatBuilderOptions options;
        options.bytesPerSectorShift = static_cast<std::uint8_t>(bytesShift);
        options.sectorsPerClusterShift = static_cast<std::uint8_t>(clusterShift);
        options.clusterCount = 512;
        options.fatOffsetSectors = 24;
        test::ExFatImageBuilder builder(options);
        (void)builder.addFile(builder.root(), "data.bin", test::makePattern(3 * builder.clusterSize() + 5, 9));
        exportImage(*dir,
                    "builder-geometry-" + std::to_string(1 << bytesShift) + "x" + std::to_string(1 << clusterShift),
                    builder);
    }
}

}  // namespace
}  // namespace recovery::filesystem::exfat
