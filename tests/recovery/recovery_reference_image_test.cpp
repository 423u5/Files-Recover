// Recovery of files written (and deleted) by other software: the reference
// images of the filesystem cross-checks, recovered end to end through the
// partition table, filesystem recovery and RecoveryWriter into a temporary
// directory, each recovered file checked against the manifest's size and
// CRC-32.
//
// Skipped unless RECOVERY_NTFS_REFERENCE_DIR or RECOVERY_FAT32_REFERENCE_DIR
// names a directory of "<name>.img" / "<name>.manifest" pairs, in the formats
// documented in tests/filesystem/ntfs_reference_image_test.cpp and
// fat32_reference_image_test.cpp. See docs/testing/testing.md.

#include "recovery/filesystem_recovery.hpp"
#include "recovery/recovery_writer.hpp"

#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/crc32.hpp"
#include "storage/disk_image_source.hpp"
#include "support/candidate_helpers.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace recovery {
namespace {

std::vector<std::filesystem::path> imagesIn(const wchar_t* variable) {
    std::vector<std::filesystem::path> images;
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, variable) != 0 || value == nullptr) {
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
    std::sort(images.begin(), images.end());
    return images;
}

std::vector<std::vector<std::string>> manifestOf(std::filesystem::path image) {
    std::ifstream manifest(image.replace_extension(".manifest"));
    EXPECT_TRUE(manifest.is_open()) << image.string();
    std::vector<std::vector<std::string>> lines;
    for (std::string line; std::getline(manifest, line);) {
        std::istringstream fields(line);
        std::vector<std::string> field;
        for (std::string value; std::getline(fields, value, '\t');) {
            field.push_back(value);
        }
        if (!field.empty()) {
            lines.push_back(std::move(field));
        }
    }
    return lines;
}

// Candidates of every volume on the image (partitions, or the whole image).
struct Recovered {
    CandidateScan scan;
    std::uint64_t volumeOffset = 0;
};

std::vector<Recovered> candidatesOnImage(storage::IStorageSource& device) {
    std::vector<Recovered> volumes;
    const Result<partition::PartitionTable> table = partition::readPartitionTable(device);
    EXPECT_TRUE(table.ok());
    if (!table.ok()) {
        return volumes;
    }
    for (const partition::Partition& p : table->partitions) {
        partition::PartitionSource volume(device, p);
        EXPECT_TRUE(volume.open().ok());
        Result<std::unique_ptr<FilesystemRecovery>> recovery = openFilesystemRecovery(volume, p.offset);
        if (!recovery.ok()) {
            continue;  // not a supported filesystem
        }
        Result<CandidateScan> scan = recovery.value()->findCandidates({}, {});
        EXPECT_TRUE(scan.ok()) << (scan.ok() ? "" : describe(scan.error()));
        if (scan.ok()) {
            volumes.push_back({std::move(scan).value(), p.offset});
        }
    }
    return volumes;
}

const RecoveryCandidate* find(const std::vector<Recovered>& volumes, std::string_view path, bool deleted) {
    for (const Recovered& volume : volumes) {
        for (const RecoveryCandidate& candidate : volume.scan.candidates) {
            if (candidate.filesystemEvidence.path == path && candidate.isDeleted() == deleted) {
                return &candidate;
            }
        }
    }
    return nullptr;
}

// Recovers `candidate` with `writer` and checks the written file.
void expectRecoveredFile(RecoveryWriter& writer, const RecoveryCandidate& candidate, std::uint64_t size,
                         const std::string& crc) {
    const Result<RecoveredFile> file = writer.recover(candidate);
    ASSERT_TRUE(file.ok()) << describe(file.error()) << "\n" << test::describeCandidate(candidate);
    EXPECT_TRUE(file->report.allBytesRead()) << test::describeCandidate(candidate);
    EXPECT_EQ(file->report.reallocatedBytes, 0u) << test::describeCandidate(candidate);
    // Read through \\?\: some reference names make the path longer than MAX_PATH (L44).
    const std::filesystem::path longPath(L"\\\\?\\" + file->path.native());
    const std::vector<std::byte> data = test::readFile(longPath);
    EXPECT_EQ(data.size(), size) << candidate.filesystemEvidence.path;
    EXPECT_EQ(crc32(data), std::stoul(crc, nullptr, 16)) << test::describeCandidate(candidate);
}

TEST(RecoveryReferenceImageTest, RecoversNtfsManifestFiles) {
    const std::vector<std::filesystem::path> images = imagesIn(L"RECOVERY_NTFS_REFERENCE_DIR");
    if (images.empty()) {
        GTEST_SKIP() << "set RECOVERY_NTFS_REFERENCE_DIR to run against externally created images";
    }
    for (const std::filesystem::path& imagePath : images) {
        SCOPED_TRACE(imagePath.string());
        storage::DiskImageSource device(imagePath);
        RECOVERY_ASSERT_OK(device.open());
        const std::vector<Recovered> volumes = candidatesOnImage(device);
        ASSERT_FALSE(volumes.empty());
        const test::TempDir out;
        Result<RecoveryWriter> writer = RecoveryWriter::create(device, out.path());
        RECOVERY_ASSERT_OK(writer);

        std::size_t recovered = 0;
        for (const std::vector<std::string>& field : manifestOf(imagePath)) {
            const std::string& kind = field[0];
            if (field.size() < 2 || kind == "label" || kind == "cluster_size" || kind == "dir" ||
                kind == "deleted_dir") {
                continue;
            }
            const std::string& path = field[1];
            const bool deleted = kind.starts_with("deleted");
            const RecoveryCandidate* candidate = find(volumes, path, deleted);
            if (candidate == nullptr) {
                ADD_FAILURE() << "no candidate for " << kind << " " << path;
                continue;
            }
            EXPECT_NE(candidate->filesystemEvidence.allocation.layout, LayoutEvidence::Guessed) << path;
            if (kind == "fragmented") {
                EXPECT_GT(candidate->fragmentation.fragmentCount, 1u) << path;
                EXPECT_TRUE(candidate->fragmentation.known) << path;
            }
            if (kind == "sparse") {
                EXPECT_GT(candidate->bytes(RegionKind::Zeros), 0u) << test::describeCandidate(*candidate);
                EXPECT_FALSE(candidate->hasWarning(CandidateWarning::DataMissing)) << path;
            }
            if (kind == "file" || kind == "deleted") {
                // Deleted records may carry entry issues; their data must still be whole.
                if (kind == "file") {
                    EXPECT_TRUE(candidate->warnings.empty()) << test::describeCandidate(*candidate);
                }
                EXPECT_FALSE(candidate->hasWarning(CandidateWarning::DataMissing)) << path;
                EXPECT_FALSE(candidate->hasWarning(CandidateWarning::ClustersReallocated)) << path;
                expectRecoveredFile(writer.value(), *candidate, std::stoull(field.at(2)), field.at(3));
                ++recovered;
            }
        }
        EXPECT_GT(recovered, 0u);
    }
}

TEST(RecoveryReferenceImageTest, RecoversFat32ManifestFiles) {
    const std::vector<std::filesystem::path> images = imagesIn(L"RECOVERY_FAT32_REFERENCE_DIR");
    if (images.empty()) {
        GTEST_SKIP() << "set RECOVERY_FAT32_REFERENCE_DIR to run against externally created images";
    }
    for (const std::filesystem::path& imagePath : images) {
        SCOPED_TRACE(imagePath.string());
        storage::DiskImageSource device(imagePath);
        RECOVERY_ASSERT_OK(device.open());
        const std::vector<Recovered> volumes = candidatesOnImage(device);
        ASSERT_FALSE(volumes.empty());
        const test::TempDir out;
        Result<RecoveryWriter> writer = RecoveryWriter::create(device, out.path());
        RECOVERY_ASSERT_OK(writer);

        std::size_t recovered = 0;
        for (const std::vector<std::string>& field : manifestOf(imagePath)) {
            if (field.size() != 3 || field[0] == "deleted") {
                continue;  // deleted entries are only listed by name
            }
            const RecoveryCandidate* candidate = find(volumes, field[0], false);
            if (candidate == nullptr) {
                ADD_FAILURE() << "no candidate for " << field[0];
                continue;
            }
            expectRecoveredFile(writer.value(), *candidate, std::stoull(field[1]), field[2]);
            ++recovered;
        }
        EXPECT_GT(recovered, 0u);
    }
}

}  // namespace
}  // namespace recovery
