// ImageWriter: sequential imaging, bad-sector narrowing, cancellation and
// resume. Sources are simulated (fault injection) or temporary image files;
// images are written to a temporary directory.

#include "imaging/image_writer.hpp"

#include "storage/disk_image_source.hpp"
#include "storage/physical_disk_source.hpp"
#include "support/fake_device.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <fstream>

namespace recovery::imaging {
namespace {

using storage::BadRegion;
using test::MemoryStorageSource;

constexpr std::size_t kSector = 512;
constexpr std::size_t kBlock = 64 * kKiB;
// Deliberately not a multiple of the block size.
constexpr std::size_t kSourceSize = 2 * kMiB + 3 * kSector;

class ImageWriterTest : public ::testing::Test {
protected:
    void SetUp() override { RECOVERY_ASSERT_OK(source_.open()); }

    ImagingOptions options() {
        ImagingOptions o;
        o.blockSize = kBlock;
        o.intermediateBlockSize = 8 * kKiB;
        o.sectorRetryCount = 2;
        o.checkpointIntervalBytes = 256 * kKiB;
        o.progressInterval = std::chrono::milliseconds{0};
        o.logger = &logger_;
        return o;
    }

    Result<ImagingSummary> image(ImagingOptions o) { return ImageWriter(source_, imagePath_, std::move(o)).run(); }

    // Source data with the given sectors zeroed, i.e. what the image must contain.
    std::vector<std::byte> expectedWithZeroedSectors(std::initializer_list<std::uint64_t> sectors) const {
        std::vector<std::byte> expected = data_;
        for (const std::uint64_t s : sectors) {
            std::fill_n(expected.begin() + static_cast<std::ptrdiff_t>(s * kSector), kSector, std::byte{0});
        }
        return expected;
    }

    ImageMetadata metadata() const {
        Result<ImageMetadata> m = readImageMetadata(metadataPathFor(imagePath_));
        EXPECT_TRUE(m.ok()) << (m.ok() ? "" : describe(m.error()));
        return m.ok() ? m.value() : ImageMetadata{};
    }

    test::TempDir dir_;
    std::filesystem::path imagePath_ = dir_ / "usb.img";
    std::vector<std::byte> data_ = test::makePattern(kSourceSize, 0xD15C);
    MemoryStorageSource source_{data_, kSector};
    diagnostics::Logger logger_{diagnostics::LogLevel::Debug};
};

TEST_F(ImageWriterTest, CreatesIdenticalImage) {
    std::vector<ImagingProgress> updates;
    ImagingOptions o = options();
    o.onProgress = [&](const ImagingProgress& p) { updates.push_back(p); };

    const Result<ImagingSummary> summary = image(std::move(o));
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, ImagingOutcome::Completed);
    EXPECT_EQ(summary->bytesCompleted, kSourceSize);
    EXPECT_EQ(summary->totalBytes, kSourceSize);
    EXPECT_EQ(summary->unreadableBytes, 0u);
    EXPECT_TRUE(summary->badRegions.empty());

    EXPECT_EQ(test::readFile(imagePath_), data_);

    const ImageMetadata m = metadata();
    EXPECT_EQ(m.state, ImageState::Completed);
    EXPECT_EQ(m.bytesCompleted, kSourceSize);
    EXPECT_EQ(m.sourceSize, kSourceSize);
    EXPECT_EQ(m.sectorSize, kSector);
    EXPECT_EQ(m.sourceType, storage::SourceType::Synthetic);
    EXPECT_FALSE(m.startedUtc.empty());

    ASSERT_FALSE(updates.empty());
    EXPECT_EQ(updates.back().bytesCompleted, kSourceSize);
    for (std::size_t i = 1; i < updates.size(); ++i) {
        EXPECT_GE(updates[i].bytesCompleted, updates[i - 1].bytesCompleted);
    }
}

TEST_F(ImageWriterTest, ReadsSourceSequentiallyInBlocks) {
    RECOVERY_ASSERT_OK(image(options()));
    const std::size_t blocks = (kSourceSize + kBlock - 1) / kBlock;
    EXPECT_EQ(source_.readCount(), blocks);
}

TEST_F(ImageWriterTest, DoesNotModifySource) {
    const std::vector<std::byte> before = source_.data();
    source_.addBadSector(7);
    RECOVERY_ASSERT_OK(image(options()));
    EXPECT_EQ(source_.data(), before);
}

TEST_F(ImageWriterTest, RefusesToOverwriteExistingImage) {
    const std::vector<std::byte> existing = test::makePattern(100, 1);
    test::writeFile(imagePath_, existing);

    RECOVERY_EXPECT_ERROR(image(options()), ErrorCode::DestinationError);
    EXPECT_EQ(test::readFile(imagePath_), existing);
    EXPECT_FALSE(std::filesystem::exists(metadataPathFor(imagePath_)));
}

TEST_F(ImageWriterTest, RefusesWhenMetadataAlreadyExists) {
    test::writeFile(metadataPathFor(imagePath_), test::makePattern(10));
    RECOVERY_EXPECT_ERROR(image(options()), ErrorCode::DestinationError);
    EXPECT_FALSE(std::filesystem::exists(imagePath_));
}

TEST_F(ImageWriterTest, RefusesMissingDestinationDirectory) {
    imagePath_ = dir_ / "missing" / "usb.img";
    RECOVERY_EXPECT_ERROR(image(options()), ErrorCode::DestinationError);
}

TEST_F(ImageWriterTest, RequiresOpenSource) {
    source_.close();
    RECOVERY_EXPECT_ERROR(image(options()), ErrorCode::InvalidInput);
}

TEST_F(ImageWriterTest, RejectsInvalidOptions) {
    ImagingOptions o = options();
    o.blockSize = kBlock + 1;
    RECOVERY_EXPECT_ERROR(image(o), ErrorCode::InvalidInput);

    o = options();
    o.blockSize = storage::kMaxReadSize + kSector;
    RECOVERY_EXPECT_ERROR(image(o), ErrorCode::InvalidInput);

    o = options();
    o.blockSize = 0;
    RECOVERY_EXPECT_ERROR(image(o), ErrorCode::InvalidInput);

    o = options();
    o.intermediateBlockSize = 1000;
    RECOVERY_EXPECT_ERROR(image(o), ErrorCode::InvalidInput);

    o = options();
    o.sectorRetryCount = ImagingOptions::kMaxSectorRetries + 1;
    RECOVERY_EXPECT_ERROR(image(o), ErrorCode::InvalidInput);

    EXPECT_FALSE(std::filesystem::exists(imagePath_));
}

TEST_F(ImageWriterTest, BadSectorsAreZeroFilledRecordedAndSkipped) {
    source_.addBadSector(100, MemoryStorageSource::kErrorCrc);
    source_.addBadSector(101, MemoryStorageSource::kErrorCrc);
    source_.addBadSector(500, MemoryStorageSource::kErrorSectorNotFound);
    // Last sector of the source, inside the final short block.
    const std::uint64_t lastSector = kSourceSize / kSector - 1;
    source_.addBadSector(lastSector, MemoryStorageSource::kErrorCrc);

    const Result<ImagingSummary> summary = image(options());
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, ImagingOutcome::Completed);

    const std::vector<BadRegion> expected = {
        {100 * kSector, 2 * kSector, MemoryStorageSource::kErrorCrc},
        {500 * kSector, kSector, MemoryStorageSource::kErrorSectorNotFound},
        {lastSector * kSector, kSector, MemoryStorageSource::kErrorCrc},
    };
    EXPECT_EQ(summary->badRegions, expected);
    EXPECT_EQ(summary->unreadableBytes, 4 * kSector);
    EXPECT_EQ(metadata().badRegions, expected);
    EXPECT_EQ(test::readFile(imagePath_), expectedWithZeroedSectors({100, 101, 500, lastSector}));
}

TEST_F(ImageWriterTest, FailureNarrowsFromBlockToIntermediateToSector) {
    // One block holding one bad sector: 64 KiB block, 8 KiB intermediate, 512 B sectors.
    data_.resize(kBlock);
    MemoryStorageSource small(data_, kSector);
    RECOVERY_ASSERT_OK(small.open());
    small.addBadSector(3);

    const Result<ImagingSummary> summary = ImageWriter(small, imagePath_, options()).run();
    RECOVERY_ASSERT_OK(summary);

    // 1 block read (fails)
    // + 1 intermediate read of [0, 8K) (fails)
    //   + 16 sector reads, sector 3 retried twice (+2)
    // + 7 intermediate reads of the remaining 8K pieces
    EXPECT_EQ(small.readCount(), 1u + 1u + 16u + 2u + 7u);
    ASSERT_EQ(summary->badRegions.size(), 1u);
    EXPECT_EQ(summary->badRegions[0], (BadRegion{3 * kSector, kSector, MemoryStorageSource::kErrorCrc}));
}

TEST_F(ImageWriterTest, TransientFailureRecoveredByRetry) {
    // Fails for: block, intermediate, first sector read, retry 1. Retry 2 succeeds.
    source_.addTransientFailure(10, 4);
    const Result<ImagingSummary> summary = image(options());
    RECOVERY_ASSERT_OK(summary);
    EXPECT_TRUE(summary->badRegions.empty());
    EXPECT_EQ(test::readFile(imagePath_), data_);
}

TEST_F(ImageWriterTest, PersistentFailureBeyondRetriesIsRecorded) {
    source_.addTransientFailure(10, 5);
    const Result<ImagingSummary> summary = image(options());
    RECOVERY_ASSERT_OK(summary);
    ASSERT_EQ(summary->badRegions.size(), 1u);
    EXPECT_EQ(summary->badRegions[0].offset, 10 * kSector);
}

TEST_F(ImageWriterTest, RetryCountIsConfigurable) {
    source_.addTransientFailure(10, 3);
    ImagingOptions o = options();
    o.sectorRetryCount = 0;
    const Result<ImagingSummary> summary = image(o);
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->badRegions.size(), 1u);
}

TEST_F(ImageWriterTest, WithoutIntermediateLevelFallsBackToSectors) {
    data_.resize(kBlock);
    MemoryStorageSource small(data_, kSector);
    RECOVERY_ASSERT_OK(small.open());
    small.addBadSector(3);
    ImagingOptions o = options();
    o.intermediateBlockSize = 0;

    RECOVERY_ASSERT_OK(ImageWriter(small, imagePath_, o).run());
    // 1 block read + 128 sector reads + 2 retries.
    EXPECT_EQ(small.readCount(), 1u + 128u + 2u);
}

TEST_F(ImageWriterTest, PartialReadsAreNarrowedDown) {
    source_.setShortReadLimit(4096);
    const Result<ImagingSummary> summary = image(options());
    RECOVERY_ASSERT_OK(summary);
    EXPECT_TRUE(summary->badRegions.empty());
    EXPECT_EQ(test::readFile(imagePath_), data_);
}

TEST_F(ImageWriterTest, NonIoErrorStopsImagingAndMarksFailed) {
    ImagingOptions o = options();
    o.onProgress = [this](const ImagingProgress& p) {
        if (p.bytesCompleted >= 4 * kBlock) {
            source_.close();  // subsequent reads fail with NotOpen
        }
    };
    RECOVERY_EXPECT_ERROR(image(std::move(o)), ErrorCode::InvalidInput);
    EXPECT_EQ(metadata().state, ImageState::Failed);
    EXPECT_EQ(metadata().bytesCompleted, 4 * kBlock);
}

TEST_F(ImageWriterTest, FailedImageMetadataOnlyListsCommittedBadRegions) {
    // Block 2 holds a bad sector followed by a sector that fails fatally, so
    // the block is abandoned after its bad sector was already recorded.
    const std::uint64_t blockStart = 2 * kBlock / kSector;
    source_.addBadSector(5);  // committed (block 0)
    source_.addBadSector(blockStart + 3);
    source_.addFatalSector(blockStart + 5);

    RECOVERY_EXPECT_ERROR(image(options()), ErrorCode::InternalError);

    const ImageMetadata m = metadata();
    EXPECT_EQ(m.state, ImageState::Failed);
    EXPECT_EQ(m.bytesCompleted, 2 * kBlock);
    const std::vector<BadRegion> expected = {{5 * kSector, kSector, MemoryStorageSource::kErrorCrc}};
    EXPECT_EQ(m.badRegions, expected);
}

TEST_F(ImageWriterTest, LogsBadSectors) {
    auto sink = std::make_shared<diagnostics::MemorySink>();
    logger_.addSink(sink);
    source_.addBadSector(42);
    RECOVERY_ASSERT_OK(image(options()));

    bool sawWarning = false;
    for (const auto& record : sink->records()) {
        if (record.level == diagnostics::LogLevel::Warning && record.message == "unreadable sector") {
            sawWarning = true;
        }
    }
    EXPECT_TRUE(sawWarning);
}

// ---------------------------------------------------------------------------
// Cancellation and resume
// ---------------------------------------------------------------------------

class ImageWriterResumeTest : public ImageWriterTest {
protected:
    // Runs until at least `bytes` have been imaged, then cancels.
    Result<ImagingSummary> imageUntil(std::uint64_t bytes) {
        CancellationSource cancel;
        ImagingOptions o = options();
        o.cancellation = cancel.token();
        o.onProgress = [cancel, bytes](const ImagingProgress& p) mutable {
            if (p.bytesCompleted >= bytes) {
                cancel.requestCancellation();
            }
        };
        return image(std::move(o));
    }

    Result<ImagingSummary> resume() {
        ImagingOptions o = options();
        o.resume = true;
        return image(std::move(o));
    }
};

TEST_F(ImageWriterResumeTest, CancellationStopsAtBlockBoundaryAndRecordsProgress) {
    const Result<ImagingSummary> summary = imageUntil(10 * kBlock);
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, ImagingOutcome::Cancelled);
    EXPECT_EQ(summary->bytesCompleted, 10 * kBlock);

    const ImageMetadata m = metadata();
    EXPECT_EQ(m.state, ImageState::Cancelled);
    EXPECT_EQ(m.bytesCompleted, 10 * kBlock);

    const std::vector<std::byte> partial = test::readFile(imagePath_);
    ASSERT_EQ(partial.size(), 10 * kBlock);
    EXPECT_TRUE(std::equal(partial.begin(), partial.end(), data_.begin()));
}

TEST_F(ImageWriterResumeTest, CancelledBeforeStartProducesEmptyResumableImage) {
    CancellationSource cancel;
    cancel.requestCancellation();
    ImagingOptions o = options();
    o.cancellation = cancel.token();
    const Result<ImagingSummary> summary = image(std::move(o));
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, ImagingOutcome::Cancelled);
    EXPECT_EQ(summary->bytesCompleted, 0u);
    EXPECT_EQ(source_.readCount(), 0u);

    RECOVERY_ASSERT_OK(resume());
    EXPECT_EQ(test::readFile(imagePath_), data_);
}

TEST_F(ImageWriterResumeTest, ResumeCompletesImageWithoutRereadingDoneBlocks) {
    RECOVERY_ASSERT_OK(imageUntil(12 * kBlock));
    const std::size_t readsBefore = source_.readCount();

    const Result<ImagingSummary> summary = resume();
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->outcome, ImagingOutcome::Completed);
    EXPECT_EQ(summary->resumedFrom, 12 * kBlock);
    EXPECT_EQ(summary->bytesCompleted, kSourceSize);

    const std::size_t remainingBlocks = (kSourceSize - 12 * kBlock + kBlock - 1) / kBlock;
    EXPECT_EQ(source_.readCount() - readsBefore, remainingBlocks);
    EXPECT_EQ(test::readFile(imagePath_), data_);
    EXPECT_EQ(metadata().state, ImageState::Completed);
}

TEST_F(ImageWriterResumeTest, ResumeKeepsBadRegionsFromEarlierRun) {
    source_.addBadSector(5);            // before the cancellation point
    source_.addBadSector(30 * 128 + 1);  // after it (block 30)
    RECOVERY_ASSERT_OK(imageUntil(8 * kBlock));

    const Result<ImagingSummary> summary = resume();
    RECOVERY_ASSERT_OK(summary);
    ASSERT_EQ(summary->badRegions.size(), 2u);
    EXPECT_EQ(summary->badRegions[0].offset, 5 * kSector);
    EXPECT_EQ(summary->badRegions[1].offset, (30 * 128 + 1) * kSector);
    EXPECT_EQ(test::readFile(imagePath_), expectedWithZeroedSectors({5, 30 * 128 + 1}));
}

TEST_F(ImageWriterResumeTest, ResumeAfterCrashDiscardsUncommittedData) {
    RECOVERY_ASSERT_OK(imageUntil(6 * kBlock));

    // Simulate a crash: data written past the last checkpoint, state never finalized.
    {
        std::ofstream out(imagePath_, std::ios::binary | std::ios::app);
        const std::vector<std::byte> garbage = test::makePattern(3 * kBlock, 99);
        out.write(reinterpret_cast<const char*>(garbage.data()), static_cast<std::streamsize>(garbage.size()));
    }
    ImageMetadata m = metadata();
    m.state = ImageState::InProgress;
    RECOVERY_ASSERT_OK(writeImageMetadata(metadataPathFor(imagePath_), m));

    RECOVERY_ASSERT_OK(resume());
    EXPECT_EQ(test::readFile(imagePath_), data_);
}

TEST_F(ImageWriterResumeTest, ResumeAfterFailureIsAllowed) {
    ImagingOptions o = options();
    o.onProgress = [this](const ImagingProgress& p) {
        if (p.bytesCompleted >= 3 * kBlock) {
            source_.close();
        }
    };
    EXPECT_FALSE(image(std::move(o)).ok());
    RECOVERY_ASSERT_OK(source_.open());

    RECOVERY_ASSERT_OK(resume());
    EXPECT_EQ(test::readFile(imagePath_), data_);
}

TEST_F(ImageWriterResumeTest, ResumeRejectsCompletedImage) {
    RECOVERY_ASSERT_OK(image(options()));
    RECOVERY_EXPECT_ERROR(resume(), ErrorCode::InvalidInput);
}

TEST_F(ImageWriterResumeTest, ResumeRequiresMetadata) {
    EXPECT_FALSE(resume().ok());
    EXPECT_FALSE(std::filesystem::exists(imagePath_));
}

TEST_F(ImageWriterResumeTest, ResumeRejectsDifferentSource) {
    RECOVERY_ASSERT_OK(imageUntil(4 * kBlock));

    MemoryStorageSource other(test::makePattern(kSourceSize + kSector), kSector);
    RECOVERY_ASSERT_OK(other.open());
    ImagingOptions o = options();
    o.resume = true;
    RECOVERY_EXPECT_ERROR(ImageWriter(other, imagePath_, o).run(), ErrorCode::InvalidInput);

    MemoryStorageSource otherSector(data_, 4096);
    RECOVERY_ASSERT_OK(otherSector.open());
    o.blockSize = kBlock;
    RECOVERY_EXPECT_ERROR(ImageWriter(otherSector, imagePath_, o).run(), ErrorCode::InvalidInput);
}

TEST_F(ImageWriterResumeTest, ResumeRejectsTruncatedImage) {
    RECOVERY_ASSERT_OK(imageUntil(8 * kBlock));
    std::filesystem::resize_file(imagePath_, 2 * kBlock);
    RECOVERY_EXPECT_ERROR(resume(), ErrorCode::InvalidFormat);
}

TEST_F(ImageWriterResumeTest, ResumeRejectsCorruptMetadata) {
    RECOVERY_ASSERT_OK(imageUntil(8 * kBlock));
    test::writeFile(metadataPathFor(imagePath_), test::makePattern(300, 3));
    EXPECT_FALSE(resume().ok());
}

// ---------------------------------------------------------------------------
// Destination safety with real source types
// ---------------------------------------------------------------------------

TEST(ImageWriterSafetyTest, RefusesToImagePhysicalDiskOntoItself) {
    auto config = std::make_shared<test::FakeDeviceConfig>();
    config->data = test::makePattern(1 * kMiB);
    config->geometry = test::diskGeometry(config->data.size());
    storage::PhysicalDiskSource source(4, std::make_shared<test::MockDeviceOpener>(config));
    RECOVERY_ASSERT_OK(source.open());

    const test::TempDir dir;
    ImagingOptions o;
    o.diskResolver = [](const std::filesystem::path&) -> Result<std::vector<std::uint32_t>> {
        return std::vector<std::uint32_t>{4};
    };
    RECOVERY_EXPECT_ERROR(ImageWriter(source, dir / "disk4.img", o).run(), ErrorCode::DestinationError);
    EXPECT_FALSE(std::filesystem::exists(dir / "disk4.img"));
}

TEST(ImageWriterSafetyTest, ImagesPhysicalDiskToOtherDisk) {
    auto config = std::make_shared<test::FakeDeviceConfig>();
    config->data = test::makePattern(1 * kMiB);
    config->geometry = test::diskGeometry(config->data.size(), 4096);
    storage::PhysicalDiskSource source(4, std::make_shared<test::MockDeviceOpener>(config));
    RECOVERY_ASSERT_OK(source.open());

    const test::TempDir dir;
    ImagingOptions o;
    o.blockSize = 256 * kKiB;
    o.diskResolver = [](const std::filesystem::path&) -> Result<std::vector<std::uint32_t>> {
        return std::vector<std::uint32_t>{0};
    };
    const Result<ImagingSummary> summary = ImageWriter(source, dir / "disk4.img", o).run();
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(test::readFile(dir / "disk4.img"), config->data);

    const Result<ImageMetadata> m = readImageMetadata(metadataPathFor(dir / "disk4.img"));
    RECOVERY_ASSERT_OK(m);
    EXPECT_EQ(m->sourceType, storage::SourceType::PhysicalDisk);
    EXPECT_EQ(m->sourcePath, "\\\\.\\PhysicalDrive4");
    EXPECT_EQ(m->sectorSize, 4096u);
}

TEST(ImageWriterSafetyTest, RefusesToImageFileOntoItself) {
    const test::TempDir dir;
    const auto path = dir / "source.img";
    test::writeFile(path, test::makePattern(64 * kKiB));
    storage::DiskImageSource source(path);
    RECOVERY_ASSERT_OK(source.open());

    RECOVERY_EXPECT_ERROR(ImageWriter(source, path, ImagingOptions{}).run(), ErrorCode::DestinationError);
}

TEST(ImageWriterSafetyTest, ImageOfImageFileIsIdenticalAndSourceUntouched) {
    const test::TempDir dir;
    const auto sourcePath = dir / "source.img";
    const std::vector<std::byte> data = test::makePattern(3 * kMiB + 1000);  // odd size: partial last sector
    test::writeFile(sourcePath, data);
    const auto writeTime = std::filesystem::last_write_time(sourcePath);

    {
        storage::DiskImageSource source(sourcePath);
        RECOVERY_ASSERT_OK(source.open());
        ImagingOptions o;
        o.blockSize = 1 * kMiB;
        const Result<ImagingSummary> summary = ImageWriter(source, dir / "copy.img", o).run();
        RECOVERY_ASSERT_OK(summary);
        EXPECT_EQ(summary->bytesCompleted, data.size());
    }

    EXPECT_EQ(test::readFile(dir / "copy.img"), data);
    EXPECT_EQ(test::readFile(sourcePath), data);
    EXPECT_EQ(std::filesystem::last_write_time(sourcePath), writeTime);
}

}  // namespace
}  // namespace recovery::imaging
