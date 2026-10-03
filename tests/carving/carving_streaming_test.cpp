// Large-image behaviour: scans and carves of images far larger than memory
// (a 1 TiB virtual image), with memory measured on the heap, reads checked
// for being sequential and bounded, hits and candidates checked for being
// delivered while the scan is still reading, offsets beyond 4 GiB, and a
// flood of false signatures.

#include "carving/file_carver.hpp"

#include "carving/format_registry.hpp"
#include "carving/signature_scanner.hpp"
#include "heap_tracking.hpp"
#include "recovery/byte_order.hpp"
#include "support/carving_formats.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace recovery::carving {
namespace {

using test::VirtualSource;

constexpr std::uint64_t kTiB = 1ULL << 40;
constexpr std::uint64_t kGiB64 = 1ULL << 30;

// Heap growth during a scope, measured when tracking is available.
class HeapWatch {
public:
    HeapWatch() : baseline_(test::heapStats().current) { test::resetHeapPeak(); }

    // Largest amount of heap in use above the baseline so far.
    [[nodiscard]] std::int64_t peakGrowth() const { return test::heapStats().peak - baseline_; }

private:
    std::int64_t baseline_;
};

class CarvingStreamingTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(registry_.add(std::make_shared<test::SizedFormat>()).ok());
        ASSERT_TRUE(registry_.add(std::make_shared<test::MarkerFormat>()).ok());
        ASSERT_TRUE(registry_.add(std::make_shared<test::BoxFormat>()).ok());
        Result<SignatureScanner> scanner = SignatureScanner::create(registry_);
        ASSERT_TRUE(scanner.ok());
        scanner_ = std::make_unique<SignatureScanner>(std::move(scanner).value());
    }

    FormatRegistry registry_;
    std::unique_ptr<SignatureScanner> scanner_;
};

TEST_F(CarvingStreamingTest, ScanningTheTailOfATerabyteImageUsesBoundedMemory) {
    VirtualSource source(kTiB);
    const std::uint64_t start = kTiB - 64 * kMiB;
    const std::vector<std::uint64_t> expected = {start, start + 4 * kMiB - 6, start + 30 * kMiB + 1, kTiB - 22,
                                                 kTiB - 4};
    for (std::size_t i = 0; i + 1 < expected.size(); ++i) {
        source.plant(expected[i], test::makeSizedFile(10));
    }
    source.plant(kTiB - 4, test::bytesOf("SZD1"));  // the pattern ends at the end of the source
    RECOVERY_ASSERT_OK(source.open());

    ScanOptions options;
    options.startOffset = start;
    options.blockSize = 4 * kMiB;
    std::vector<std::uint64_t> hits;
    hits.reserve(16);
    const HeapWatch heap;
    Result<ScanReport> report = scanner_->scan(
        source,
        [&](const SignatureHit& hit) {
            hits.push_back(hit.fileOffset);
            return success();
        },
        options);
    const std::int64_t growth = heap.peakGrowth();
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(report->outcome, ScanOutcome::Completed);
    EXPECT_EQ(hits, expected);
    EXPECT_EQ(report->bytesRead, 64 * kMiB);

    const VirtualSource::Stats stats = source.stats();
    EXPECT_EQ(stats.backwardReads, 0u);
    EXPECT_LE(stats.maxReadLength, options.blockSize);
    EXPECT_EQ(stats.bytes, 64 * kMiB);
    EXPECT_EQ(stats.highestEnd, kTiB);
    if (test::heapTrackingAvailable()) {
        // One block plus bookkeeping, whatever the size of the source.
        EXPECT_LE(growth, static_cast<std::int64_t>(options.blockSize + 256 * kKiB)) << growth;
    }
}

TEST_F(CarvingStreamingTest, AFullScanReadsEachByteOnceAndStreamsItsHits) {
    constexpr std::uint64_t kSize = 128 * kMiB;
    VirtualSource source(kSize);
    std::vector<std::uint64_t> expected;
    for (std::uint64_t offset = 777; offset < kSize; offset += 2 * kMiB + 4099) {
        source.plant(offset, test::makeSizedFile(100));
        expected.push_back(offset);
    }
    RECOVERY_ASSERT_OK(source.open());

    ScanOptions options;
    options.blockSize = 1 * kMiB;
    std::uint64_t count = 0;
    bool inOrder = true;
    std::uint64_t latestDelivery = 0;  // how far past its hit the source had been read at delivery
    const HeapWatch heap;
    Result<ScanReport> report = scanner_->scan(
        source,
        [&](const SignatureHit& hit) {
            inOrder = inOrder && count < expected.size() && hit.fileOffset == expected[count];
            ++count;
            latestDelivery = std::max(latestDelivery, source.stats().highestEnd - hit.fileOffset);
            return success();
        },
        options);
    const std::int64_t growth = heap.peakGrowth();
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(count, expected.size());
    EXPECT_TRUE(inOrder);
    // Each hit is delivered as soon as the block holding it has been read.
    EXPECT_LE(latestDelivery, 2 * options.blockSize);
    EXPECT_EQ(report->bytesRead, kSize);
    const VirtualSource::Stats stats = source.stats();
    EXPECT_EQ(stats.bytes, kSize);
    EXPECT_EQ(stats.backwardReads, 0u);
    EXPECT_EQ(stats.reads, 128u);
    if (test::heapTrackingAvailable()) {
        EXPECT_LE(growth, static_cast<std::int64_t>(options.blockSize + 256 * kKiB)) << growth;
    }
}

TEST_F(CarvingStreamingTest, CarvingStreamsCandidatesWithBoundedMemory) {
    constexpr std::uint64_t kSize = 64 * kMiB;
    VirtualSource source(kSize);
    const std::vector<std::byte> file = test::makeSizedFile(100 * 1024);
    std::uint64_t planted = 0;
    for (std::uint64_t offset = 4096; offset + file.size() < kSize; offset += 300 * 1024 + 7) {
        source.plant(offset, file);
        ++planted;
    }
    RECOVERY_ASSERT_OK(source.open());

    CarveOptions options;
    std::uint64_t valid = 0;
    std::uint64_t previous = 0;
    bool increasing = true;
    FileCarver carver(source, options);
    const HeapWatch heap;
    Result<CarveReport> report = carver.run(*scanner_, [&](FileCandidate&& candidate) {
        valid += candidate.validation.status == ValidationStatus::Valid && candidate.length == file.size() ? 1 : 0;
        increasing = increasing && candidate.sourceOffset > previous;
        previous = candidate.sourceOffset;
        return success();
    });
    const std::int64_t growth = heap.peakGrowth();
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(report->candidates, planted);
    EXPECT_EQ(valid, planted);
    EXPECT_TRUE(increasing);
    EXPECT_EQ(report->scan.bytesRead, kSize);
    // The carve reads each file once more (end detection and validation), not the whole image.
    EXPECT_LE(report->carveBytesRead, planted * (file.size() + 8 * 1024));
    if (test::heapTrackingAvailable()) {
        // The scan block, one carve's cache and a candidate at a time.
        EXPECT_LE(growth, static_cast<std::int64_t>(options.scan.blockSize + options.readCacheSize + 256 * kKiB))
            << growth;
    }
}

TEST_F(CarvingStreamingTest, OffsetsBeyondFourGiBAreExact) {
    constexpr std::uint64_t kSize = 16 * kGiB64;
    VirtualSource source(kSize);
    const std::vector<std::byte> sized = test::makeSizedFile(70'000);
    const std::vector<std::byte> box = test::makeBoxFile(3, {100'000});
    const std::vector<std::byte> marker = test::makeMarkerFile(90);
    const std::uint64_t sizedAt = (1ULL << 32) - 6;  // straddles 4 GiB
    const std::uint64_t boxAt = (1ULL << 33) + 1;
    const std::uint64_t markerAt = kSize - marker.size();  // ends at the end of the source
    source.plant(sizedAt, sized);
    source.plant(boxAt, box);
    source.plant(markerAt, marker);
    RECOVERY_ASSERT_OK(source.open());

    const std::vector<std::pair<std::uint64_t, const std::vector<std::byte>*>> files = {
        {sizedAt, &sized}, {boxAt, &box}, {markerAt, &marker}};
    for (const auto& [offset, bytes] : files) {
        SCOPED_TRACE(offset);
        CarveOptions options;
        options.scan.startOffset = offset - 1000;
        options.scan.endOffset = std::min(offset + 1000, kSize);
        FileCarver carver(source, options);
        std::vector<FileCandidate> candidates;
        Result<CarveReport> report = carver.run(*scanner_, [&](FileCandidate&& candidate) {
            candidates.push_back(std::move(candidate));
            return success();
        });
        RECOVERY_ASSERT_OK(report);
        ASSERT_EQ(candidates.size(), 1u);
        EXPECT_EQ(candidates[0].sourceOffset, offset);
        EXPECT_EQ(candidates[0].length, bytes->size());
        EXPECT_EQ(candidates[0].validation.status, ValidationStatus::Valid);
        EXPECT_TRUE(source.contentAt(candidates[0].extents[0].sourceOffset,
                                     static_cast<std::size_t>(candidates[0].extents[0].length)) == *bytes);
    }
}

TEST_F(CarvingStreamingTest, AFloodOfFalseSignaturesCostsBoundedWork) {
    // 32,768 hits in 256 KiB, each declaring a length its header cannot have.
    std::vector<std::byte> flood;
    for (int i = 0; i < 32'768; ++i) {
        for (const char c : {'S', 'Z', 'D', '1'}) {
            flood.push_back(static_cast<std::byte>(c));
        }
        const std::size_t at = flood.size();
        flood.resize(at + 4);
        storeLe32(flood, at, 4);
    }
    VirtualSource source(4 * kMiB);
    source.plant(1 * kMiB, flood);
    RECOVERY_ASSERT_OK(source.open());

    FileCarver carver(source);
    std::uint64_t delivered = 0;
    const HeapWatch heap;
    Result<CarveReport> report = carver.run(*scanner_, [&](FileCandidate&&) {
        ++delivered;
        return success();
    });
    const std::int64_t growth = heap.peakGrowth();
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(delivered, 0u);
    EXPECT_EQ(report->scan.hits, 32'768u);
    EXPECT_EQ(report->count(RejectionReason::HeaderRejected), 32'768u);
    // Each rejected hit costs one small read.
    EXPECT_EQ(report->carveBytesRead, 32'768u * SourceContentReader::kMinCacheSize);
    if (test::heapTrackingAvailable()) {
        EXPECT_LE(growth, static_cast<std::int64_t>(1 * kMiB + 256 * kKiB)) << growth;
    }

    // The hit limit bounds the work of a scan whatever the image holds.
    CarveOptions limited;
    limited.scan.maxHits = 1000;
    FileCarver bounded(source, limited);
    Result<CarveReport> stopped = bounded.run(*scanner_, [](FileCandidate&&) { return success(); });
    RECOVERY_ASSERT_OK(stopped);
    EXPECT_EQ(stopped->scan.outcome, ScanOutcome::HitLimitReached);
    EXPECT_EQ(stopped->scan.hits, 1000u);
    EXPECT_EQ(stopped->scan.nextOffset, 1 * kMiB + 999u * 8u + 1u);
}

}  // namespace
}  // namespace recovery::carving
