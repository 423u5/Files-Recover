// Large images and bounded memory (P15): a 6 GiB image (an MBR with a FAT32
// partition and an empty one, files in the space no partition holds, beyond
// 4 GiB) scanned whole, and interrupted far into its source pass and
// resumed; memory measured on the heap while scanning images of 1 and 4 GiB
// stays below what the run options allow and does not grow with the image.

#include "scan/scan_coordinator.hpp"

#include "carving/heap_tracking.hpp"
#include "scan_test_support.hpp"
#include "support/audio_builders.hpp"
#include "support/image_builders.hpp"
#include "support/mp4_builders.hpp"
#include "support/partition_builder.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <map>

namespace recovery::scan {
namespace {

using test::Bytes;
using test::Collector;

constexpr std::uint64_t kGiB64 = 1ULL << 30;
constexpr std::uint64_t kPartitionStart = 1 * kMiB;

// Where the files no partition holds are, and what they are.
struct Planted {
    std::uint64_t offset = 0;
    Bytes bytes;
    std::string format;
};

// In the last third of the image (beyond 4 GiB in one of 6 GiB), at sectors.
std::vector<Planted> plantedFiles(std::uint64_t size) {
    ::recovery::test::JpegOptions jpeg;
    jpeg.width = 96;
    jpeg.height = 80;
    jpeg.seed = 42;
    const auto at = [&](std::uint64_t sixteenths, std::uint64_t sectors) {
        return size / 16 * sixteenths / 512 * 512 + 512 * sectors;
    };
    return {
        {at(11, 1001), ::recovery::test::makeJpeg(jpeg), "jpeg"},
        {at(12, 1400), ::recovery::test::makePng({}), "png"},
        {at(13, 77), ::recovery::test::makeMp4({}).bytes, "mp4"},
        {size - 64 * kKiB, ::recovery::test::makeWav({}), "wav"},
    };
}

// An image: an MBR, a FAT32 card as its first partition, an empty second
// partition (no filesystem) in its middle, and `files` after it.
void build(::recovery::test::VirtualSource& source, std::uint64_t size, const std::vector<Planted>& files) {
    const Bytes& card = [] () -> const Bytes& {
        static const Bytes bytes = test::makeCard(2048, 0x1A46E);
        return bytes;
    }();
    Bytes mbr(512);
    const auto lba = [](std::uint64_t offset) { return static_cast<std::uint32_t>(offset / 512); };
    ::recovery::test::writeMbrSector(
        mbr, 512, 0,
        {{0x00, 0x0C, lba(kPartitionStart), lba(card.size())},
         {0x00, 0x07, lba(size / 2 / kMiB * kMiB), lba(256 * kMiB)}});
    source.plant(0, mbr);
    source.plant(kPartitionStart, card);
    for (const Planted& file : files) {
        source.plant(file.offset, file.bytes);
    }
}

ScanConfiguration sectorAligned() {
    ScanConfiguration configuration;
    configuration.alignment = 512;  // files on disks start at sectors
    return configuration;
}

ScanRunOptions largeRunOptions(std::uint32_t workers = 4) {
    ScanRunOptions options;
    options.workerThreads = workers;
    options.blockSize = 1 * kMiB;
    options.cacheBlocks = 16;
    options.checkpointBytes = 256 * kMiB;
    options.progressInterval = std::chrono::milliseconds{0};
    return options;
}

TEST(ScanLargeImageTest, ASixGibImageIsScannedWholeAndResumedBeyondFourGib) {
    constexpr std::uint64_t kSize = 6 * kGiB64;
    const std::vector<Planted> files = plantedFiles(kSize);
    ::recovery::test::VirtualSource source(kSize);
    build(source, kSize, files);
    RECOVERY_ASSERT_OK(source.open());

    Collector whole;
    const Result<ScanSummary> summary =
        ScanCoordinator(source, test::allFormats(), test::allMedia(), sectorAligned(), largeRunOptions())
            .run(whole.sink());
    RECOVERY_ASSERT_OK(summary);
    EXPECT_EQ(summary->metrics.bytesScanned, kSize);
    EXPECT_GE(summary->metrics.bytesRead, kSize);
    // Two partitions: the card's FAT32, and one with no filesystem.
    const std::vector<VolumeRecord>& volumes = whole.checkpoint.volumes();
    ASSERT_EQ(volumes.size(), 2U);
    EXPECT_EQ(volumes[0].offset, kPartitionStart);
    EXPECT_TRUE(volumes[0].candidates.has_value());
    EXPECT_FALSE(volumes[1].candidates.has_value());
    EXPECT_TRUE(volumes[1].error.has_value());
    // The files no partition holds are found by carving, at their offsets.
    std::map<std::uint64_t, const evaluation::EvaluatedCandidate*> byOffset;
    for (const evaluation::EvaluatedCandidate& candidate : whole.candidates) {
        if (candidate.sourceOffset().has_value()) {
            byOffset.emplace(*candidate.sourceOffset(), &candidate);
        }
    }
    for (const Planted& file : files) {
        SCOPED_TRACE(file.format);
        const auto found = byOffset.find(file.offset);
        ASSERT_NE(found, byOffset.end());
        EXPECT_EQ(found->second->formatId, file.format);
        EXPECT_EQ(found->second->recoveredSize(), file.bytes.size());
        EXPECT_EQ(found->second->data.method, RecoveryMethod::Carving);
    }
    // And the card's files, inside the partition.
    std::size_t named = 0;
    for (const evaluation::EvaluatedCandidate& candidate : whole.candidates) {
        named += candidate.hasFilesystemEvidence() ? 1 : 0;
    }
    EXPECT_GE(named, 7U);

    // Interrupted far into the pass (beyond 4 GiB), then resumed with another worker count.
    Collector resumed;
    ScanRunOptions options = largeRunOptions(2);
    JobControl control = options.control;
    bool fired = false;
    options.onProgress = [&](const ScanProgress& progress) {
        if (!fired && progress.stage == ScanStage::SourcePass && progress.stageDone > 4 * kGiB64 + 600 * kMiB) {
            fired = true;
            control.requestCancellation();
        }
    };
    const Result<ScanSummary> first =
        ScanCoordinator(source, test::allFormats(), test::allMedia(), sectorAligned(), options).run(resumed.sink());
    RECOVERY_ASSERT_OK(first);
    ASSERT_TRUE(fired);
    EXPECT_EQ(first->outcome, ScanOutcome::Cancelled);
    ASSERT_TRUE(resumed.checkpoint.pass().has_value());
    EXPECT_GT(resumed.checkpoint.pass()->position, 4 * kGiB64);
    const std::uint64_t readBefore = resumed.checkpoint.pass()->scan.bytesRead;

    source.resetStats();
    const Result<ScanSummary> second =
        ScanCoordinator(source, test::allFormats(), test::allMedia(), sectorAligned(), largeRunOptions(3))
            .run(resumed.sink(), &resumed.checkpoint);
    RECOVERY_ASSERT_OK(second);
    EXPECT_EQ(second->outcome, ScanOutcome::Completed);
    EXPECT_EQ(test::describe(resumed.candidates), test::describe(whole.candidates));
    // The pass went on where it stopped: what it read before is not read again.
    ASSERT_TRUE(resumed.checkpoint.pass().has_value());
    EXPECT_LT(resumed.checkpoint.pass()->scan.bytesRead - readBefore, kSize - 4 * kGiB64);
    EXPECT_LT(source.stats().bytes, kSize - 4 * kGiB64 + 64 * kMiB);
}

// Heap used while scanning a virtual image of `size`, above what was in use before.
std::int64_t peakHeapOfAScan(std::uint64_t size) {
    const std::vector<Planted> files = plantedFiles(size);
    ::recovery::test::VirtualSource source(size);
    build(source, size, files);
    EXPECT_TRUE(source.open().ok());
    std::uint64_t candidates = 0;
    const std::int64_t baseline = ::recovery::test::heapStats().current;
    ::recovery::test::resetHeapPeak();
    {
        ScanCoordinator coordinator(source, test::allFormats(), test::allMedia(), sectorAligned(), largeRunOptions());
        const Result<ScanSummary> summary = coordinator.run([&](const ScanUpdate& update) {
            candidates += update.candidates.size();
            return success();
        });
        EXPECT_TRUE(summary.ok());
    }
    EXPECT_GE(candidates, files.size());
    return ::recovery::test::heapStats().peak - baseline;
}

TEST(ScanLargeImageTest, MemoryStaysBoundedWhateverTheImageSize) {
    if (!::recovery::test::heapTrackingAvailable()) {
        GTEST_SKIP() << "heap tracking is not available under AddressSanitizer";
    }
    // Warm the registries and the card first, so that they are not counted.
    (void)peakHeapOfAScan(5 * kGiB64 / 4);
    const std::int64_t small = peakHeapOfAScan(5 * kGiB64 / 4);
    const std::int64_t large = peakHeapOfAScan(5 * kGiB64);
    RecordProperty("peak_heap_1_25_gib", std::to_string(small));
    RecordProperty("peak_heap_5_gib", std::to_string(large));
    const ScanRunOptions options = largeRunOptions();
    // The cache, the pass's block, the workers' carve caches and the queues.
    const auto bound = static_cast<std::int64_t>(options.cacheBlocks * options.blockSize + 24 * kMiB);
    EXPECT_LE(small, bound) << small;
    EXPECT_LE(large, bound) << large;
    // Four times the image, about the same memory.
    EXPECT_LE(large - small, static_cast<std::int64_t>(2 * kMiB)) << small << " " << large;
}

}  // namespace
}  // namespace recovery::scan
