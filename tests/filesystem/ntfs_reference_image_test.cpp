// Cross-checks against another NTFS implementation, which guards against the
// test builder and the parser sharing a misreading of the format.
//
// NtfsReferenceImageTest reads images written by other software (ntfs-3g,
// Windows, ...). It is skipped unless RECOVERY_NTFS_REFERENCE_DIR names a
// directory holding pairs of "<name>.img" and "<name>.manifest". Manifest
// lines, tab-separated:
//   label\t<volume label>
//   cluster_size\t<bytes>
//   file\t<path>\t<size>\t<CRC-32 as 8 hex digits>      an active file
//   dir\t<path>                                        an active directory
//   fragmented\t<path>                                 an active file stored in several runs
//   deleted\t<path>\t<size>\t<CRC-32>                  a deleted file whose data is intact
//   deleted_dir\t<path>                                a deleted directory
//   deleted_any\t<path>                                a deleted file whose clusters may be reused
//   sparse\t<path>                                     an active file with holes (flagged SparseRuns)
//
// NtfsBuilderExport writes the builder's images to RECOVERY_NTFS_EXPORT_DIR
// (skipped otherwise), each with a manifest of
// "<file|deleted>\t<record>\t<size>\t<CRC-32>\t<name>" lines, so that ntfs-3g
// (ntfscat, ntfsundelete) can read them independently. See docs/testing/testing.md.

#include "filesystem/ntfs/ntfs_filesystem.hpp"

#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/crc32.hpp"
#include "storage/disk_image_source.hpp"
#include "support/fat32_builder.hpp"  // readExtents
#include "support/ntfs_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

namespace recovery::filesystem::ntfs {
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
    const std::optional<std::filesystem::path> dir = directoryFromEnvironment(L"RECOVERY_NTFS_REFERENCE_DIR");
    if (!dir) {
        return images;
    }
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(*dir, ec)) {
        if (item.path().extension() == ".img") {
            images.push_back(item.path());
        }
    }
    std::sort(images.begin(), images.end());
    return images;
}

std::vector<std::byte> contentOf(storage::IStorageSource& volume, const FileAllocation& allocation) {
    if (allocation.method == AllocationMethod::Resident) {
        return allocation.residentData;
    }
    return test::readExtents(volume, allocation.extents);
}

std::string issuesOf(const FileRecord& record) {
    std::string text;
    for (const EntryIssue issue : record.entry.issues) {
        text += std::string(toString(issue)) + " ";
    }
    for (const AllocationIssue issue : record.allocation.issues) {
        text += std::string(toString(issue)) + " ";
    }
    return text;
}

TEST(NtfsReferenceImageTest, MatchesManifests) {
    const std::vector<std::filesystem::path> images = referenceImages();
    if (images.empty()) {
        GTEST_SKIP() << "set RECOVERY_NTFS_REFERENCE_DIR to run against externally created images";
    }
    for (const std::filesystem::path& imagePath : images) {
        SCOPED_TRACE(imagePath.string());
        storage::DiskImageSource device(imagePath);
        RECOVERY_ASSERT_OK(device.open());
        const Result<partition::PartitionTable> table = partition::readPartitionTable(device);
        RECOVERY_ASSERT_OK(table);
        const auto volume = std::find_if(table->partitions.begin(), table->partitions.end(),
                                         [](const partition::Partition& p) { return p.candidates.ntfs; });
        ASSERT_NE(volume, table->partitions.end()) << "no NTFS partition found";
        partition::PartitionSource part(device, *volume);
        RECOVERY_ASSERT_OK(part.open());

        Result<std::unique_ptr<NtfsFilesystem>> fs = NtfsFilesystem::open(part);
        RECOVERY_ASSERT_OK(fs);
        NtfsFilesystem& ntfs = *fs.value();
        EXPECT_TRUE(ntfs.info().warnings.empty()) << ntfs.info().warnings.front();
        EXPECT_TRUE(ntfs.hasClusterBitmap());
        const Result<FileScan> scan = ntfs.scan({}, {});
        RECOVERY_ASSERT_OK(scan);
        for (const ScanIssue& issue : scan->issues) {
            ADD_FAILURE() << "scan issue: " << toString(issue.kind) << " " << issue.path << ": " << issue.detail;
        }
        EXPECT_EQ(scan->crossLinkedClusters, 0u);
        const Result<ClusterUsage> usage = ntfs.analyzeClusters({});
        RECOVERY_ASSERT_OK(usage);
        // Every allocated cluster belongs to an attribute of an in-use record.
        EXPECT_EQ(usage->allocated, scan->referencedClusters);

        std::filesystem::path manifestPath = imagePath;
        manifestPath.replace_extension(".manifest");
        std::ifstream manifest(manifestPath);
        ASSERT_TRUE(manifest.is_open());
        std::vector<std::vector<std::string>> lines;
        std::set<std::string> sparse;
        for (std::string line; std::getline(manifest, line);) {
            std::istringstream fields(line);
            std::vector<std::string> field;
            for (std::string value; std::getline(fields, value, '\t');) {
                field.push_back(value);
            }
            if (field.size() >= 2) {
                if (field[0] == "sparse") {
                    sparse.insert(field[1]);
                }
                lines.push_back(std::move(field));
            }
        }
        ASSERT_FALSE(lines.empty());

        // Active entries are consistent (deleted ones may legitimately carry
        // issues, and sparse files are flagged as such).
        for (const FileRecord& record : scan->records) {
            if (record.entry.state == EntryState::Active && !sparse.contains(record.path)) {
                EXPECT_TRUE(record.entry.issues.empty() && record.allocation.issues.empty())
                    << record.path << ": " << issuesOf(record);
            }
        }

        const auto find = [&](const std::string& path) -> const FileRecord* {
            const auto record = std::find_if(scan->records.begin(), scan->records.end(),
                                             [&](const FileRecord& r) { return r.path == path; });
            return record == scan->records.end() ? nullptr : &*record;
        };
        std::size_t checked = 0;
        for (const std::vector<std::string>& field : lines) {
            ++checked;
            const std::string& kind = field[0];
            if (kind == "label") {
                EXPECT_EQ(ntfs.info().label, field.at(1));
                continue;
            }
            if (kind == "cluster_size") {
                EXPECT_EQ(ntfs.info().clusterSize, std::stoul(field.at(1)));
                continue;
            }
            const std::string& path = field.at(1);
            const FileRecord* record = find(path);
            if (record == nullptr) {
                ADD_FAILURE() << "missing " << kind << " " << path;
                continue;
            }
            const bool deleted = kind.starts_with("deleted");
            EXPECT_EQ(record->entry.state, deleted ? EntryState::Deleted : EntryState::Active) << path;
            EXPECT_EQ(record->entry.isDirectory, kind == "dir" || kind == "deleted_dir") << path;
            if (kind == "fragmented") {
                EXPECT_GT(record->allocation.fragmentCount(), 1u) << path;
            }
            if (kind == "sparse") {
                EXPECT_TRUE(record->allocation.hasIssue(AllocationIssue::SparseRuns)) << path;
            }
            if (kind == "file" || kind == "deleted") {
                EXPECT_EQ(record->entry.size, std::stoull(field.at(2))) << path;
                if (kind == "deleted") {
                    EXPECT_FALSE(record->allocation.hasIssue(AllocationIssue::ClustersInUse)) << path;
                }
                const std::vector<std::byte> data = contentOf(part, record->allocation);
                EXPECT_EQ(std::format("{:08x}", crc32(data)), field.at(3)) << path << ": " << issuesOf(*record);
            }
        }
        EXPECT_GT(checked, 0u);
    }
}

// ---------------------------------------------------------------------------
// Builder images for external checking.
// ---------------------------------------------------------------------------

struct Exported {
    bool deleted = false;
    std::uint64_t record = 0;
    std::string name;
    std::vector<std::byte> data;
};

void exportImage(const std::filesystem::path& dir, const std::string& name, test::NtfsImageBuilder& builder,
                 const std::vector<Exported>& files) {
    test::writeFile(dir / (name + ".img"), builder.build());
    std::ofstream manifest(dir / (name + ".manifest"), std::ios::binary);
    for (const Exported& file : files) {
        manifest << (file.deleted ? "deleted" : "file") << '\t' << file.record << '\t' << file.data.size() << '\t' << std::format("{:08x}", crc32(file.data)) << '\t'
                 << file.name << '\n';
    }
}

Exported add(test::NtfsImageBuilder& builder, std::uint64_t parent, std::string name, std::vector<std::byte> data) {
    const auto entry = builder.addFile(parent, name, data);
    return Exported{false, entry.record, std::move(name), std::move(data)};
}

TEST(NtfsBuilderExport, WritesImages) {
    const std::optional<std::filesystem::path> dir = directoryFromEnvironment(L"RECOVERY_NTFS_EXPORT_DIR");
    if (!dir) {
        GTEST_SKIP() << "set RECOVERY_NTFS_EXPORT_DIR to export builder images for ntfs-3g";
    }
    std::filesystem::create_directories(*dir);
    constexpr std::uint64_t kRoot = test::NtfsImageBuilder::root();

    {
        test::NtfsImageBuilder builder;
        std::vector<Exported> files;
        const auto dcim = builder.addDirectory(kRoot, "DCIM");
        const auto camera = builder.addDirectory(dcim.record, "100MEDIA");
        files.push_back(add(builder, camera.record, "IMG_0001.JPG", test::makePattern(5000, 1)));
        files.push_back(add(builder, kRoot, "small.txt", test::makePattern(100, 2)));
        files.push_back(add(builder, kRoot, "resident across a block end.bin", test::makePattern(650, 3)));
        files.push_back(add(builder, kRoot, "empty.bin", {}));
        files.push_back(add(builder, kRoot, "\xC3\x89t\xC3\xA9 \xCE\xB1\xCE\xB2\xCE\xB3 \xF0\x9F\x98\x80.jpeg",
                            test::makePattern(900, 4)));
        const auto fragmented = test::makePattern(6 * 512 - 7, 5);
        const auto frag = builder.addFileInClusters(kRoot, "fragmented.bin", fragmented,
                                                    {4400, 4401, 4402, 4900, 4901, 4600});
        files.push_back(Exported{false, frag.record, "fragmented.bin", fragmented});
        const auto goneData = test::makePattern(3000, 6);
        const auto gone = builder.addFile(kRoot, "deleted photo.jpg", goneData);
        builder.deleteEntry(gone);
        files.push_back(Exported{true, gone.record, "deleted photo.jpg", goneData});
        builder.markBadClusters(7000, 2);
        exportImage(*dir, "builder-basic", builder, files);
    }
    {
        test::NtfsBuilderOptions options;
        options.fragmentedMft = true;
        test::NtfsImageBuilder builder(options);
        std::vector<Exported> files;
        for (int i = 0; i < 20; ++i) {
            files.push_back(add(builder, kRoot, "file " + std::to_string(i) + ".bin",
                                test::makePattern(700 * static_cast<std::size_t>(i + 1), static_cast<std::uint64_t>(i))));
        }
        exportImage(*dir, "builder-fragmented-mft", builder, files);
    }
    struct Geometry {
        std::uint16_t bytesPerSector;
        std::uint32_t sectorsPerCluster;
        std::uint32_t recordSize;
        std::uint64_t clusterCount;
    };
    for (const Geometry g : {Geometry{512, 8, 1024, 4096}, Geometry{4096, 1, 4096, 4096}, Geometry{512, 128, 1024, 256}}) {
        test::NtfsBuilderOptions options;
        options.bytesPerSector = g.bytesPerSector;
        options.sectorsPerCluster = g.sectorsPerCluster;
        options.recordSize = g.recordSize;
        options.clusterCount = g.clusterCount;
        test::NtfsImageBuilder builder(options);
        std::vector<Exported> files;
        files.push_back(add(builder, kRoot, "data.bin", test::makePattern(3 * builder.clusterSize() + 5, 9)));
        files.push_back(add(builder, kRoot, "note.txt", test::makePattern(200, 10)));
        exportImage(*dir,
                    std::format("builder-geometry-{}x{}-record{}", g.bytesPerSector, g.sectorsPerCluster,
                                g.recordSize),
                    builder, files);
    }
}

}  // namespace
}  // namespace recovery::filesystem::ntfs
