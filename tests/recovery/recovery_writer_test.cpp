// Recovery output: candidates written below a temporary destination
// directory and compared with the original files; collision-safe and
// Windows-safe names; never overwriting; no partial files; destination
// safety against the source.

#include "recovery/recovery_writer.hpp"

#include "recovery/filesystem_recovery.hpp"
#include "storage/disk_image_source.hpp"
#include "storage/physical_disk_source.hpp"
#include "support/candidate_helpers.hpp"
#include "support/fake_device.hpp"
#include "support/fat32_builder.hpp"
#include "support/memory_source.hpp"
#include "support/ntfs_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>

namespace recovery {
namespace {

using test::candidateAt;
using test::candidatesAt;

std::vector<std::byte> bytesOf(std::size_t size, std::uint64_t seed) {
    return test::makePattern(size, seed);
}

// Every regular file below `root`, as paths relative to it.
std::set<std::filesystem::path> filesBelow(const std::filesystem::path& root) {
    std::set<std::filesystem::path> files;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
        if (item.is_regular_file()) {
            files.insert(std::filesystem::relative(item.path(), root));
        }
    }
    return files;
}

std::filesystem::path longPath(const std::filesystem::path& path) {
    return std::filesystem::path(L"\\\\?\\" + std::filesystem::absolute(path).native());
}

RecoveryCandidate handMade(std::string name, std::string path, std::vector<SourceRegion> regions) {
    RecoveryCandidate candidate;
    candidate.id = CandidateId{1};
    candidate.filename = std::move(name);
    candidate.filesystemEvidence.path = std::move(path);
    candidate.expectedSize = regions.empty() ? 0 : regions.back().fileOffset + regions.back().length;
    candidate.sourceRegions = std::move(regions);
    return candidate;
}

class RecoveryWriterTest : public ::testing::Test {
protected:
    RecoveryWriterTest() : pattern_(bytesOf(64 * 1024, 77)), source_(pattern_, 512) {
        EXPECT_TRUE(source_.open().ok());
    }

    [[nodiscard]] RecoveryWriter writer(RecoveryWriterOptions options = {}) {
        Result<RecoveryWriter> created = RecoveryWriter::create(source_, dir_.path() / "out", std::move(options));
        EXPECT_TRUE(created.ok()) << (created.ok() ? "" : describe(created.error()));
        return std::move(created).value();
    }

    [[nodiscard]] std::filesystem::path out() const { return dir_.path() / "out"; }

    std::vector<std::byte> pattern_;
    test::MemoryStorageSource source_;
    test::TempDir dir_;
};

TEST_F(RecoveryWriterTest, RecoversAFilesystemIntoTheOriginalTree) {
    test::Fat32ImageBuilder builder;
    const auto root = test::Fat32ImageBuilder::root();
    const auto dcim = builder.addDirectory(root, "DCIM");
    const auto camera = builder.addDirectory(dcim.clusters.front(), "100MEDIA");
    const auto first = bytesOf(3000, 1);
    const auto second = bytesOf(4500, 2);
    const auto unicode = bytesOf(1700, 3);
    const auto older = bytesOf(900, 4);
    const auto newer = bytesOf(1100, 5);
    (void)builder.addFile(camera.clusters.front(), "IMG_0001.JPG", first);
    builder.deleteEntry(builder.addFileInClusters(camera.clusters.front(), "IMG_0002.JPG", second,
                                                  {900, 901, 902, 903, 904, 905, 906, 907, 908}));
    (void)builder.addFile(root, "Отпуск 😀.mp4", unicode);
    builder.deleteEntry(builder.addFile(root, "My Photo.jpg", older));
    (void)builder.addFile(root, "My Photo.jpg", newer);
    (void)builder.addFile(root, "empty.txt", {});

    test::MemoryStorageSource image(builder.build(), 512);
    RECOVERY_ASSERT_OK(image.open());
    Result<std::unique_ptr<FilesystemRecovery>> recovery = openFilesystemRecovery(image, 0);
    RECOVERY_ASSERT_OK(recovery);
    const Result<CandidateScan> scan = recovery.value()->findCandidates({}, {});
    RECOVERY_ASSERT_OK(scan);

    Result<RecoveryWriter> created = RecoveryWriter::create(image, out());
    RECOVERY_ASSERT_OK(created);
    RecoveryWriter& output = created.value();
    std::map<std::uint64_t, RecoveredFile> written;
    for (const RecoveryCandidate& candidate : scan->candidates) {
        Result<RecoveredFile> file = output.recover(candidate);
        RECOVERY_ASSERT_OK(file);
        EXPECT_EQ(file->candidate, candidate.id);
        EXPECT_TRUE(file->report.allBytesRead());
        written.emplace(candidate.id.value(), std::move(file).value());
    }

    const std::filesystem::path camDir = out() / "DCIM" / "100MEDIA";
    EXPECT_EQ(test::readFile(camDir / "IMG_0001.JPG"), first);
    // A deleted 8.3-only name loses its first character; the candidate says so.
    const RecoveryCandidate* renamed = candidateAt(scan.value(), "/DCIM/100MEDIA/_MG_0002.JPG");
    ASSERT_NE(renamed, nullptr);
    EXPECT_TRUE(renamed->hasWarning(CandidateWarning::NameUncertain));
    EXPECT_EQ(test::readFile(camDir / "_MG_0002.JPG"), second);
    EXPECT_EQ(test::readFile(out() / std::filesystem::path(u8"Отпуск 😀.mp4")), unicode);
    EXPECT_TRUE(test::readFile(out() / "empty.txt").empty());

    // Duplicate names: the first candidate keeps the name, the second is numbered.
    const auto photos = candidatesAt(scan.value(), "/My Photo.jpg");
    ASSERT_EQ(photos.size(), 2u);
    const RecoveredFile& firstPhoto = written.at(photos[0]->id.value());
    const RecoveredFile& secondPhoto = written.at(photos[1]->id.value());
    EXPECT_EQ(firstPhoto.path, out() / "My Photo.jpg");
    EXPECT_EQ(secondPhoto.path, out() / "My Photo (1).jpg");
    EXPECT_EQ(test::readFile(firstPhoto.path), photos[0]->isDeleted() ? older : newer);
    EXPECT_EQ(test::readFile(secondPhoto.path), photos[1]->isDeleted() ? older : newer);
    EXPECT_EQ(filesBelow(out()).size(), scan->candidates.size());
}

TEST_F(RecoveryWriterTest, FlatOutputNumbersSameNamesFromDifferentDirectories) {
    test::NtfsImageBuilder builder;
    const auto a = builder.addDirectory(test::NtfsImageBuilder::root(), "A");
    const auto b = builder.addDirectory(test::NtfsImageBuilder::root(), "B");
    const auto inA = bytesOf(2000, 6);
    const auto inB = bytesOf(2100, 7);
    (void)builder.addFile(a.record, "same.txt", inA);
    (void)builder.addFile(b.record, "same.txt", inB);
    test::MemoryStorageSource image(builder.build(), 512);
    RECOVERY_ASSERT_OK(image.open());
    Result<std::unique_ptr<FilesystemRecovery>> recovery = openFilesystemRecovery(image, 0);
    RECOVERY_ASSERT_OK(recovery);
    const Result<CandidateScan> scan = recovery.value()->findCandidates({}, {});
    RECOVERY_ASSERT_OK(scan);

    RecoveryWriterOptions options;
    options.preserveDirectories = false;
    Result<RecoveryWriter> output = RecoveryWriter::create(image, out(), options);
    RECOVERY_ASSERT_OK(output);
    const Result<RecoveredFile> first = output->recover(*candidateAt(scan.value(), "/A/same.txt"));
    const Result<RecoveredFile> second = output->recover(*candidateAt(scan.value(), "/B/same.txt"));
    RECOVERY_ASSERT_OK(first);
    RECOVERY_ASSERT_OK(second);
    EXPECT_EQ(first->path, out() / "same.txt");
    EXPECT_EQ(second->path, out() / "same (1).txt");
    EXPECT_EQ(test::readFile(first->path), inA);
    EXPECT_EQ(test::readFile(second->path), inB);
}

TEST_F(RecoveryWriterTest, NeverOverwritesExistingFiles) {
    std::filesystem::create_directories(out());
    const auto existing = bytesOf(10, 8);
    test::writeFile(out() / "photo.jpg", existing);
    test::writeFile(out() / "photo (1).jpg", existing);

    RecoveryWriter output = writer();
    const RecoveryCandidate candidate = handMade("photo.jpg", "/photo.jpg", {{0, 1000, RegionKind::Stored, 0, false}});
    const Result<RecoveredFile> file = output.recover(candidate);
    RECOVERY_ASSERT_OK(file);
    EXPECT_EQ(file->path, out() / "photo (2).jpg");
    EXPECT_EQ(test::readFile(file->path), std::vector<std::byte>(pattern_.begin(), pattern_.begin() + 1000));
    EXPECT_EQ(test::readFile(out() / "photo.jpg"), existing);
    EXPECT_EQ(test::readFile(out() / "photo (1).jpg"), existing);
}

TEST_F(RecoveryWriterTest, UnsafeNamesStayInsideTheDestination) {
    RecoveryWriter output = writer();
    const std::vector<SourceRegion> data{{0, 100, RegionKind::Stored, 0, false}};
    const std::vector<std::pair<RecoveryCandidate, std::filesystem::path>> cases{
        {handMade("../../evil.txt", "/../../evil.txt", data), ".._.._evil.txt"},
        {handMade("x.txt", "/..//sub/x.txt", data), std::filesystem::path("_") / "sub" / "x.txt"},
        {handMade("CON", "/CON", data), "_CON"},
        {handMade("a:b|c?.jpg", "/a:b|c?.jpg", data), "a_b_c_.jpg"},
        {handMade("dots. . ", "/dots. . ", data), "dots"},
        {handMade("in c.txt", "/C:/in c.txt", data), std::filesystem::path("C_") / "in c.txt"},
        {handMade("", "/", data), "unnamed"},
    };
    for (const auto& [candidate, expected] : cases) {
        const Result<RecoveredFile> file = output.recover(candidate);
        RECOVERY_ASSERT_OK(file);
        EXPECT_EQ(file->path, out() / expected) << candidate.filename;
        EXPECT_TRUE(std::filesystem::is_regular_file(file->path)) << candidate.filename;
    }
    // Nothing was written next to the destination.
    std::set<std::filesystem::path> outside;
    for (const auto& item : std::filesystem::directory_iterator(dir_.path())) {
        outside.insert(item.path().filename());
    }
    EXPECT_EQ(outside, std::set<std::filesystem::path>{"out"});
}

TEST_F(RecoveryWriterTest, DirectoryNameHeldByAFileGetsANumber) {
    std::filesystem::create_directories(out());
    test::writeFile(out() / "DCIM", bytesOf(5, 9));
    RecoveryWriter output = writer();
    const Result<RecoveredFile> file =
        output.recover(handMade("a.jpg", "/DCIM/a.jpg", {{0, 100, RegionKind::Stored, 0, false}}));
    RECOVERY_ASSERT_OK(file);
    EXPECT_EQ(file->path, out() / "DCIM (1)" / "a.jpg");
    // Later files of the same original directory go to the same place.
    const Result<RecoveredFile> next =
        output.recover(handMade("b.jpg", "/DCIM/b.jpg", {{0, 100, RegionKind::Stored, 0, false}}));
    RECOVERY_ASSERT_OK(next);
    EXPECT_EQ(next->path, out() / "DCIM (1)" / "b.jpg");
}

TEST_F(RecoveryWriterTest, LinksInTheDestinationAreNotFollowed) {
    std::filesystem::create_directories(out());
    const std::filesystem::path elsewhere = dir_.path() / "elsewhere";
    std::filesystem::create_directories(elsewhere);
    std::error_code ec;
    std::filesystem::create_directory_symlink(elsewhere, out() / "DCIM", ec);
    if (ec) {
        GTEST_SKIP() << "creating a symbolic link needs Developer Mode or administrator rights";
    }
    RecoveryWriter output = writer();
    const Result<RecoveredFile> file =
        output.recover(handMade("a.jpg", "/DCIM/a.jpg", {{0, 100, RegionKind::Stored, 0, false}}));
    RECOVERY_ASSERT_OK(file);
    EXPECT_EQ(file->path, out() / "DCIM (1)" / "a.jpg");
    EXPECT_TRUE(std::filesystem::is_empty(elsewhere));
}

TEST_F(RecoveryWriterTest, MissingDataAndZeros) {
    RecoveryWriter output = writer();
    // Missing inside the file is zero-filled; missing at the end is left out.
    const Result<RecoveredFile> gaps = output.recover(handMade("gaps.bin", "/gaps.bin",
                                                               {{0, 100, RegionKind::Stored, 0, false},
                                                                {100, 50, RegionKind::Missing, 0, false},
                                                                {150, 100, RegionKind::Stored, 1000, false},
                                                                {250, 500, RegionKind::Missing, 0, false}}));
    RECOVERY_ASSERT_OK(gaps);
    std::vector<std::byte> expected(pattern_.begin(), pattern_.begin() + 100);
    expected.resize(150);
    expected.insert(expected.end(), pattern_.begin() + 1000, pattern_.begin() + 1100);
    EXPECT_EQ(test::readFile(gaps->path), expected);
    EXPECT_EQ(gaps->report.outputSize, 250u);
    EXPECT_EQ(gaps->report.missingBytes, 550u);

    // Known zeros at the end extend the file.
    const Result<RecoveredFile> zeros = output.recover(handMade(
        "zeros.bin", "/zeros.bin", {{0, 100, RegionKind::Stored, 0, false}, {100, 4000, RegionKind::Zeros, 0, false}}));
    RECOVERY_ASSERT_OK(zeros);
    std::vector<std::byte> padded(pattern_.begin(), pattern_.begin() + 100);
    padded.resize(4100);
    EXPECT_EQ(test::readFile(zeros->path), padded);
}

TEST_F(RecoveryWriterTest, CandidateWithoutLocatedDataIsRefused) {
    RecoveryWriter output = writer();
    RECOVERY_EXPECT_ERROR(output.recover(handMade("lost.jpg", "/lost.jpg", {{0, 5000, RegionKind::Missing, 0, false}})),
                          ErrorCode::InvalidInput);
    RecoveryCandidate malformed = handMade("bad.jpg", "/bad.jpg", {{0, 10, RegionKind::Stored, 0, false}});
    malformed.expectedSize = 20;
    RECOVERY_EXPECT_ERROR(output.recover(malformed), ErrorCode::InvalidInput);
    EXPECT_TRUE(filesBelow(out()).empty());
}

TEST_F(RecoveryWriterTest, FailedRecoveryLeavesNoFile) {
    source_.addFatalSector(4);
    RecoveryWriter output = writer();
    const RecoveryCandidate candidate =
        handMade("broken.jpg", "/DCIM/broken.jpg", {{0, 8192, RegionKind::Stored, 0, false}});
    const Result<RecoveredFile> file = output.recover(candidate);
    ASSERT_FALSE(file.ok());
    EXPECT_EQ(file.error().code, ErrorCode::InternalError);
    EXPECT_TRUE(filesBelow(out()).empty());

    CancellationSource cancel;
    cancel.requestCancellation();
    RecoveryWriterOptions options;
    options.reconstruction.cancellation = cancel.token();
    Result<RecoveryWriter> cancelled = RecoveryWriter::create(source_, out(), options);
    RECOVERY_ASSERT_OK(cancelled);
    RECOVERY_EXPECT_ERROR(cancelled->recover(handMade("c.jpg", "/c.jpg", {{0, 10, RegionKind::Stored, 0, false}})),
                          ErrorCode::Cancelled);
    EXPECT_TRUE(filesBelow(out()).empty());
}

TEST_F(RecoveryWriterTest, BadSectorsAreZeroFilledAndReported) {
    source_.addBadSector(3);  // bytes [1536, 2048)
    RecoveryWriter output = writer();
    const Result<RecoveredFile> file =
        output.recover(handMade("scratched.jpg", "/scratched.jpg", {{0, 4096, RegionKind::Stored, 0, false}}));
    RECOVERY_ASSERT_OK(file);
    std::vector<std::byte> expected(pattern_.begin(), pattern_.begin() + 4096);
    std::fill(expected.begin() + 1536, expected.begin() + 2048, std::byte{0});
    EXPECT_EQ(test::readFile(file->path), expected);
    EXPECT_EQ(file->report.unreadableBytes, 512u);
    ASSERT_EQ(file->report.unreadableRegions.size(), 1u);
    EXPECT_EQ(file->report.unreadableRegions[0].offset, 1536u);
}

TEST_F(RecoveryWriterTest, DeepTreesAreNotLimitedToMaxPath) {
    std::string path;
    for (int i = 0; i < 12; ++i) {
        path += "/directory level number " + std::to_string(i);
    }
    path += "/deep file.bin";
    ASSERT_GT(path.size(), 300u);
    RecoveryWriter output = writer();
    const Result<RecoveredFile> file =
        output.recover(handMade("deep file.bin", path, {{0, 3000, RegionKind::Stored, 0, false}}));
    RECOVERY_ASSERT_OK(file);
    EXPECT_GT(file->path.native().size(), 300u);
    EXPECT_EQ(test::readFile(longPath(file->path)), std::vector<std::byte>(pattern_.begin(), pattern_.begin() + 3000));
}

TEST_F(RecoveryWriterTest, LogsRecoveredFiles) {
    auto sink = std::make_shared<diagnostics::MemorySink>();
    diagnostics::Logger logger;
    logger.addSink(sink);
    RecoveryWriterOptions options;
    options.logger = &logger;
    RecoveryWriter output = writer(options);
    RECOVERY_ASSERT_OK(output.recover(handMade("a.bin", "/a.bin", {{0, 10, RegionKind::Stored, 0, false}})));
    const auto records = sink->records();
    EXPECT_TRUE(
        std::any_of(records.begin(), records.end(), [](const auto& r) { return r.message == "recovered file"; }));
}

// ---------------------------------------------------------------------------
// Destination safety
// ---------------------------------------------------------------------------

TEST(RecoveryWriterSafetyTest, RefusesTheSourceImageAsDestination) {
    const test::TempDir dir;
    const auto imagePath = dir / "card.img";
    test::writeFile(imagePath, test::makePattern(64 * 1024));
    storage::DiskImageSource source(imagePath);
    RECOVERY_ASSERT_OK(source.open());
    RECOVERY_EXPECT_ERROR(RecoveryWriter::create(source, imagePath), ErrorCode::DestinationError);
    RECOVERY_EXPECT_ERROR(RecoveryWriter::create(source, "\\\\.\\PhysicalDrive0"), ErrorCode::DestinationError);
    RECOVERY_EXPECT_ERROR(RecoveryWriter::create(source, ""), ErrorCode::InvalidInput);

    // Next to the image is fine.
    Result<RecoveryWriter> beside = RecoveryWriter::create(source, dir / "recovered");
    RECOVERY_ASSERT_OK(beside);
    RECOVERY_ASSERT_OK(beside->recover(handMade("a.bin", "/a.bin", {{0, 10, RegionKind::Stored, 0, false}})));
}

TEST(RecoveryWriterSafetyTest, RefusesADestinationOnTheSourceDisk) {
    auto config = std::make_shared<test::FakeDeviceConfig>();
    config->data = test::makePattern(1 * kMiB);
    config->geometry = test::diskGeometry(config->data.size());
    storage::PhysicalDiskSource source(4, std::make_shared<test::MockDeviceOpener>(config));
    RECOVERY_ASSERT_OK(source.open());

    const test::TempDir dir;
    RecoveryWriterOptions sameDisk;
    sameDisk.diskResolver = [](const std::filesystem::path&) -> Result<std::vector<std::uint32_t>> {
        return std::vector<std::uint32_t>{4};
    };
    RECOVERY_EXPECT_ERROR(RecoveryWriter::create(source, dir / "recovered", sameDisk), ErrorCode::DestinationError);
    EXPECT_FALSE(std::filesystem::exists(dir / "recovered"));

    RecoveryWriterOptions unknown;
    unknown.diskResolver = [](const std::filesystem::path&) -> Result<std::vector<std::uint32_t>> {
        return makeError(ErrorCode::IoError, "cannot resolve");
    };
    RECOVERY_EXPECT_ERROR(RecoveryWriter::create(source, dir / "recovered", unknown), ErrorCode::DestinationError);

    RecoveryWriterOptions otherDisk;
    int checks = 0;
    otherDisk.diskResolver = [&checks](const std::filesystem::path&) -> Result<std::vector<std::uint32_t>> {
        ++checks;
        return std::vector<std::uint32_t>{0};
    };
    Result<RecoveryWriter> output = RecoveryWriter::create(source, dir / "recovered", otherDisk);
    RECOVERY_ASSERT_OK(output);
    const int afterCreate = checks;
    const Result<RecoveredFile> file =
        output->recover(handMade("a.jpg", "/DCIM/a.jpg", {{0, 4096, RegionKind::Stored, 0, false}}));
    RECOVERY_ASSERT_OK(file);
    EXPECT_GT(checks, afterCreate);  // the new directory was checked too
    EXPECT_EQ(test::readFile(file->path), std::vector<std::byte>(config->data.begin(), config->data.begin() + 4096));
}

TEST(RecoveryWriterSafetyTest, DestinationMustBeADirectory) {
    const test::TempDir dir;
    test::writeFile(dir / "file", test::makePattern(10));
    test::MemoryStorageSource source(test::makePattern(4096), 512);
    RECOVERY_ASSERT_OK(source.open());
    RECOVERY_EXPECT_ERROR(RecoveryWriter::create(source, dir / "file"), ErrorCode::DestinationError);

    test::MemoryStorageSource closed(test::makePattern(4096), 512);
    RECOVERY_EXPECT_ERROR(RecoveryWriter::create(closed, dir / "out"), ErrorCode::InvalidInput);
}

}  // namespace
}  // namespace recovery
