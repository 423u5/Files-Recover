// Signature scanning: every planted signature is found exactly once, in
// order; masked and offset signatures; alignment; scan ranges and their
// boundaries (source start and end, block boundaries, adjacent ranges); bad
// and known-bad sectors; fatal errors; sink errors; cancellation; the hit
// limit; progress and logging; sequential, bounded reads; and a simulated
// physical disk that enforces sector alignment.

#include "carving/signature_scanner.hpp"

#include "storage/physical_disk_source.hpp"
#include "support/carving_formats.hpp"
#include "support/fake_device.hpp"
#include "support/memory_source.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

namespace recovery::carving {
namespace {

using test::bytesOf;
using test::scriptedDescriptor;
using test::ScriptedFormat;
using test::VirtualSource;

std::vector<std::uint64_t> offsetsOf(const std::vector<SignatureHit>& hits) {
    std::vector<std::uint64_t> offsets;
    for (const SignatureHit& hit : hits) {
        offsets.push_back(hit.fileOffset);
    }
    return offsets;
}

class SignatureScannerTest : public ::testing::Test {
protected:
    void add(std::shared_ptr<const IFileFormat> format) { ASSERT_TRUE(registry_.add(std::move(format)).ok()); }

    void addStandardFormats() {
        add(std::make_shared<test::SizedFormat>());
        add(std::make_shared<test::MarkerFormat>());
        add(std::make_shared<test::BoxFormat>());
    }

    SignatureScanner makeScanner() {
        Result<SignatureScanner> scanner = SignatureScanner::create(registry_);
        EXPECT_TRUE(scanner.ok());
        return std::move(scanner).value();
    }

    // Scans with a fresh scanner and collects the hits.
    Result<ScanReport> scan(storage::IStorageSource& source, std::vector<SignatureHit>& hits,
                            const ScanOptions& options = {}) {
        const SignatureScanner scanner = makeScanner();
        return scanner.scan(
            source,
            [&](const SignatureHit& hit) {
                hits.push_back(hit);
                return success();
            },
            options);
    }

    std::vector<std::uint64_t> scanOffsets(storage::IStorageSource& source, const ScanOptions& options = {}) {
        std::vector<SignatureHit> hits;
        Result<ScanReport> report = scan(source, hits, options);
        EXPECT_TRUE(report.ok()) << describe(report.error());
        return offsetsOf(hits);
    }

    FormatRegistry registry_;
};

TEST_F(SignatureScannerTest, FindsEveryPlantedSignatureInOrder) {
    addStandardFormats();
    VirtualSource source(1 * kMiB);
    source.plant(0, test::makeSizedFile(100));
    source.plant(5000, test::makeMarkerFile(300));
    source.plant(70'001, test::makeBoxFile(10, {20, 30}));
    source.plant(700'000, test::makeSizedFile(1000, 9));
    RECOVERY_ASSERT_OK(source.open());

    std::vector<SignatureHit> hits;
    Result<ScanReport> report = scan(source, hits);
    RECOVERY_ASSERT_OK(report);
    ASSERT_EQ(offsetsOf(hits), (std::vector<std::uint64_t>{0, 5000, 70'001, 700'000}));
    EXPECT_EQ(hits[0].formatIndex, 0u);
    EXPECT_EQ(hits[1].formatIndex, 1u);
    EXPECT_EQ(hits[2].formatIndex, 2u);
    EXPECT_EQ(hits[3].formatIndex, 0u);
    for (const SignatureHit& hit : hits) {
        EXPECT_EQ(hit.format, registry_.formats()[hit.formatIndex].get());
        EXPECT_EQ(hit.signatureIndex, 0u);
    }
    EXPECT_EQ(report->outcome, ScanOutcome::Completed);
    EXPECT_EQ(report->hits, 4u);
    EXPECT_EQ(report->hitsPerFormat, (std::vector<std::uint64_t>{2, 1, 1}));
    EXPECT_EQ(report->startOffset, 0u);
    EXPECT_EQ(report->endOffset, 1 * kMiB);
    EXPECT_EQ(report->nextOffset, 1 * kMiB);
    EXPECT_EQ(report->bytesRead, 1 * kMiB);
    EXPECT_EQ(report->unreadableBytes, 0u);
}

TEST_F(SignatureScannerTest, SignaturesAtAnOffsetReportTheFileStart) {
    add(std::make_shared<test::BoxFormat>());  // "tbox" at offset 4
    VirtualSource source(64 * 1024);
    source.plant(1000, test::makeBoxFile(0, {}));
    // The pattern is 2 bytes into the source: the file would start before it.
    source.plant(2, bytesOf("tbox"));
    RECOVERY_ASSERT_OK(source.open());
    EXPECT_EQ(scanOffsets(source), (std::vector<std::uint64_t>{1000}));
}

TEST_F(SignatureScannerTest, MaskedSignaturesMatchUnderTheirMask) {
    add(std::make_shared<test::SyncFormat>());  // FF Ex
    VirtualSource source(4096);
    source.plant(100, {std::byte{0xFF}, std::byte{0xE0}});
    source.plant(200, {std::byte{0xFF}, std::byte{0xFB}});
    source.plant(300, {std::byte{0xFF}, std::byte{0xDF}});
    source.plant(400, {std::byte{0xFE}, std::byte{0xF0}});
    RECOVERY_ASSERT_OK(source.open());
    EXPECT_EQ(scanOffsets(source), (std::vector<std::uint64_t>{100, 200}));
}

TEST_F(SignatureScannerTest, OverlappingMatchesAreAllReported) {
    add(std::make_shared<ScriptedFormat>(scriptedDescriptor("abab", "ABAB")));
    VirtualSource source(4096);
    source.plant(10, bytesOf("ABABABAB"));
    RECOVERY_ASSERT_OK(source.open());
    EXPECT_EQ(scanOffsets(source), (std::vector<std::uint64_t>{10, 12, 14}));
}

TEST_F(SignatureScannerTest, OneHitPerFormatAndPositionWithItsFirstMatchingSignature) {
    // Both signatures match a file "PPQQRR"; signature 0 sits at offset 2.
    FormatDescriptor descriptor = scriptedDescriptor("two", "QQRR", 64 * 1024, 2);
    descriptor.signatures.push_back(textSignature("second", "PPQQ"));
    add(std::make_shared<ScriptedFormat>(descriptor));
    VirtualSource source(4096);
    source.plant(500, bytesOf("PPQQRR"));
    source.plant(900, bytesOf("PPQQ"));  // only the second signature
    RECOVERY_ASSERT_OK(source.open());
    std::vector<SignatureHit> hits;
    RECOVERY_ASSERT_OK(scan(source, hits));
    ASSERT_EQ(offsetsOf(hits), (std::vector<std::uint64_t>{500, 900}));
    EXPECT_EQ(hits[0].signatureIndex, 0u);
    EXPECT_EQ(hits[1].signatureIndex, 1u);
}

TEST_F(SignatureScannerTest, FormatsSharingASignatureEachGetAHitInRegistrationOrder) {
    add(std::make_shared<ScriptedFormat>(scriptedDescriptor("first", "SAME")));
    add(std::make_shared<ScriptedFormat>(scriptedDescriptor("second", "SAME")));
    VirtualSource source(4096);
    source.plant(64, bytesOf("SAME"));
    RECOVERY_ASSERT_OK(source.open());
    std::vector<SignatureHit> hits;
    RECOVERY_ASSERT_OK(scan(source, hits));
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].fileOffset, 64u);
    EXPECT_EQ(hits[0].formatIndex, 0u);
    EXPECT_EQ(hits[1].fileOffset, 64u);
    EXPECT_EQ(hits[1].formatIndex, 1u);
}

TEST_F(SignatureScannerTest, FindsSignaturesAroundEveryBlockBoundary) {
    // Short, offset, 64-byte and far-offset signatures (the last reaches
    // across several 512-byte blocks).
    const std::string longMagic(FileSignature::kMaxLength, 'L');
    add(std::make_shared<test::SizedFormat>());
    add(std::make_shared<test::BoxFormat>());
    add(std::make_shared<ScriptedFormat>(scriptedDescriptor("long", longMagic)));
    add(std::make_shared<ScriptedFormat>(scriptedDescriptor("far", "FARX", 64 * 1024, FileSignature::kMaxOffset)));
    const std::vector<std::vector<std::byte>> files = {
        test::makeSizedFile(4), test::makeBoxFile(0, {}), bytesOf(longMagic),
        [] {
            std::vector<std::byte> far(FileSignature::kMaxOffset, std::byte{1});
            const std::vector<std::byte> magic = bytesOf("FARX");
            far.insert(far.end(), magic.begin(), magic.end());
            return far;
        }()};
    ScanOptions options;
    options.blockSize = 512;
    // 520 consecutive offsets put the file start, and every byte of the
    // pattern, at every position relative to a block boundary.
    const SignatureScanner scanner = makeScanner();
    for (std::size_t format = 0; format < files.size(); ++format) {
        for (std::uint64_t offset = 1000; offset < 1520; ++offset) {
            VirtualSource source(16 * 1024);
            source.plant(offset, files[format]);
            RECOVERY_ASSERT_OK(source.open());
            std::vector<SignatureHit> hits;
            RECOVERY_ASSERT_OK(scanner.scan(
                source,
                [&](const SignatureHit& hit) {
                    hits.push_back(hit);
                    return success();
                },
                options));
            ASSERT_EQ(hits.size(), 1u) << "format " << format << " at " << offset;
            EXPECT_EQ(hits[0].fileOffset, offset);
            EXPECT_EQ(hits[0].formatIndex, format);
        }
    }
}

TEST_F(SignatureScannerTest, AlignmentRestrictsFileStarts) {
    addStandardFormats();
    VirtualSource source(64 * 1024);
    source.plant(1024, test::makeSizedFile(10));
    source.plant(3 * 512 + 1, test::makeSizedFile(10));  // not sector-aligned
    source.plant(8192, test::makeBoxFile(0, {}));       // pattern at 8196, file start aligned
    RECOVERY_ASSERT_OK(source.open());
    ScanOptions options;
    options.alignment = 512;
    EXPECT_EQ(scanOffsets(source, options), (std::vector<std::uint64_t>{1024, 8192}));
    options.alignment = 1;
    EXPECT_EQ(scanOffsets(source, options), (std::vector<std::uint64_t>{1024, 3 * 512 + 1, 8192}));
}

TEST_F(SignatureScannerTest, LargeAlignmentsSkipDataThatCannotMatter) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(4 * kMiB);
    for (std::uint64_t offset = 0; offset < 4 * kMiB; offset += 64 * 1024) {
        source.plant(offset, test::makeSizedFile(10));
        source.plant(offset + 512, test::makeSizedFile(10));  // off the alignment
    }
    RECOVERY_ASSERT_OK(source.open());
    ScanOptions options;
    options.alignment = 64 * 1024;
    options.blockSize = 4096;
    std::vector<SignatureHit> hits;
    Result<ScanReport> report = scan(source, hits, options);
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(hits.size(), 64u);
    // One block per aligned position, not the whole source.
    EXPECT_EQ(report->bytesRead, 64u * 4096u);
    EXPECT_EQ(source.stats().backwardReads, 0u);
}

TEST_F(SignatureScannerTest, RangeCoversFileStartsAndPatternsMayExtendBeyondIt) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    for (const std::uint64_t offset : {100ULL, 9998ULL, 20'000ULL, 30'000ULL}) {
        source.plant(offset, test::makeSizedFile(10));
    }
    RECOVERY_ASSERT_OK(source.open());
    ScanOptions options;
    options.startOffset = 101;
    options.endOffset = 10'000;
    std::vector<SignatureHit> hits;
    Result<ScanReport> report = scan(source, hits, options);
    RECOVERY_ASSERT_OK(report);
    // 9998 starts inside the range; its pattern ends beyond it.
    EXPECT_EQ(offsetsOf(hits), (std::vector<std::uint64_t>{9998}));
    EXPECT_EQ(report->startOffset, 101u);
    EXPECT_EQ(report->endOffset, 10'000u);
    EXPECT_EQ(report->nextOffset, 10'000u);
    // A file starting at the end of the range belongs to the next range.
    options.endOffset = 20'000;
    EXPECT_EQ(scanOffsets(source, options), (std::vector<std::uint64_t>{9998}));
}

TEST_F(SignatureScannerTest, AdjacentRangesTogetherFindEveryHitOnce) {
    addStandardFormats();
    VirtualSource source(32 * 1024);
    // 16-byte box files and 12-byte sized files, alternating, close to block boundaries.
    const std::vector<std::uint64_t> expected = {0, 500, 520, 4080, 4096, 8184, 16'384, 32 * 1024 - 16};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        source.plant(expected[i], i % 2 == 0 ? test::makeSizedFile(0) : test::makeBoxFile(0, {}));
    }
    RECOVERY_ASSERT_OK(source.open());
    ASSERT_EQ(scanOffsets(source), expected);

    ScanOptions options;
    options.blockSize = 1024;
    for (const std::uint64_t split : {1ULL, 499ULL, 500ULL, 501ULL, 519ULL, 520ULL, 521ULL, 4096ULL, 8185ULL, 30'000ULL}) {
        options.startOffset = 0;
        options.endOffset = split;
        std::vector<std::uint64_t> offsets = scanOffsets(source, options);
        options.startOffset = split;
        options.endOffset.reset();
        const std::vector<std::uint64_t> rest = scanOffsets(source, options);
        offsets.insert(offsets.end(), rest.begin(), rest.end());
        EXPECT_EQ(offsets, expected) << "split at " << split;
    }
}

TEST_F(SignatureScannerTest, SignaturesAtTheEdgesOfTheSource) {
    add(std::make_shared<ScriptedFormat>(scriptedDescriptor("edge", "EDGE")));
    VirtualSource complete(4096);
    complete.plant(0, bytesOf("EDGE"));
    complete.plant(4092, bytesOf("EDGE"));  // ends exactly at the end of the source
    RECOVERY_ASSERT_OK(complete.open());
    EXPECT_EQ(scanOffsets(complete), (std::vector<std::uint64_t>{0, 4092}));

    VirtualSource cut(4096);
    cut.plant(4093, bytesOf("EDG"));  // cut off by the end of the source
    RECOVERY_ASSERT_OK(cut.open());
    EXPECT_TRUE(scanOffsets(cut).empty());
}

TEST_F(SignatureScannerTest, AnEmptyRangeReadsNothing) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(4096);
    RECOVERY_ASSERT_OK(source.open());
    ScanOptions options;
    options.startOffset = 1000;
    options.endOffset = 1000;
    std::vector<SignatureHit> hits;
    Result<ScanReport> report = scan(source, hits, options);
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(report->outcome, ScanOutcome::Completed);
    EXPECT_EQ(report->nextOffset, 1000u);
    EXPECT_EQ(source.stats().reads, 0u);
    options.startOffset = 4096;
    options.endOffset.reset();
    RECOVERY_ASSERT_OK(scan(source, hits, options));
    EXPECT_EQ(source.stats().reads, 0u);
}

TEST_F(SignatureScannerTest, RejectsInvalidOptions) {
    add(std::make_shared<test::SizedFormat>());
    const SignatureScanner scanner = makeScanner();
    VirtualSource source(8192);
    const HitSink sink = [](const SignatureHit&) { return success(); };
    RECOVERY_EXPECT_ERROR(scanner.scan(source, sink), ErrorCode::InvalidInput);  // not open
    RECOVERY_ASSERT_OK(source.open());
    RECOVERY_EXPECT_ERROR(scanner.scan(source, HitSink{}), ErrorCode::InvalidInput);

    const auto expectInvalid = [&](const std::function<void(ScanOptions&)>& change) {
        ScanOptions options;
        change(options);
        RECOVERY_EXPECT_ERROR(scanner.scan(source, sink, options), ErrorCode::InvalidInput);
    };
    expectInvalid([](ScanOptions& o) { o.endOffset = 8193; });
    expectInvalid([](ScanOptions& o) { o.startOffset = 8193; });
    expectInvalid([](ScanOptions& o) {
        o.startOffset = 100;
        o.endOffset = 99;
    });
    expectInvalid([](ScanOptions& o) { o.blockSize = 0; });
    expectInvalid([](ScanOptions& o) { o.blockSize = 1000; });
    expectInvalid([](ScanOptions& o) { o.blockSize = storage::kMaxReadSize + 512; });
    expectInvalid([](ScanOptions& o) { o.alignment = 0; });
    expectInvalid([](ScanOptions& o) { o.alignment = 3; });
    expectInvalid([](ScanOptions& o) { o.alignment = ScanOptions::kMaxAlignment * 2; });
    expectInvalid([](ScanOptions& o) { o.maxHits = 0; });
    expectInvalid([](ScanOptions& o) { o.reads.sectorRetryCount = SourceReadOptions::kMaxSectorRetries + 1; });
}

TEST_F(SignatureScannerTest, BadSectorsAreSkippedAndReported) {
    addStandardFormats();
    VirtualSource source(256 * 1024);
    source.plant(1000, test::makeSizedFile(10));
    source.plant(40'000, test::makeMarkerFile(10));    // same 64 KiB block as the bad sector
    source.plant(41'470, test::makeSizedFile(10));     // pattern across the bad sector's start
    source.plant(200'000, test::makeBoxFile(0, {}));
    source.addBadSector(81, 23);                       // [41'472, 41'984)
    RECOVERY_ASSERT_OK(source.open());
    ScanOptions options;
    options.blockSize = 64 * 1024;
    std::vector<SignatureHit> hits;
    Result<ScanReport> report = scan(source, hits, options);
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(offsetsOf(hits), (std::vector<std::uint64_t>{1000, 40'000, 200'000}));
    EXPECT_EQ(report->outcome, ScanOutcome::Completed);
    EXPECT_EQ(report->unreadableBytes, 512u);
    EXPECT_EQ(report->unreadableRegions, (std::vector<storage::BadRegion>{{41'472, 512, 23}}));
}

TEST_F(SignatureScannerTest, UnreadableBytesNeverMatchEvenIfASignatureIsZeros) {
    add(std::make_shared<ScriptedFormat>(scriptedDescriptor("zeros", std::string(8, '\0'))));
    VirtualSource source(64 * 1024);
    source.setNoise(5);
    // Real zeros, between non-zero guards: a hit.
    std::vector<std::byte> zeros(10);
    zeros.front() = std::byte{1};
    zeros.back() = std::byte{1};
    source.plant(29'999, zeros);
    source.addBadSector(20);                          // read as zeros: no hit
    RECOVERY_ASSERT_OK(source.open());
    std::vector<SignatureHit> hits;
    Result<ScanReport> report = scan(source, hits);
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(offsetsOf(hits), (std::vector<std::uint64_t>{30'000}));
    EXPECT_EQ(report->unreadableBytes, 512u);
}

TEST_F(SignatureScannerTest, KnownBadRegionsAreNotRead) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    source.plant(1000, test::makeSizedFile(10));
    source.plant(10'100, test::makeSizedFile(10));  // inside the known bad region
    source.plant(30'000, test::makeSizedFile(10));
    source.addFatalSector(10'240 / 512);            // inside it: touching it would fail the scan
    RECOVERY_ASSERT_OK(source.open());
    storage::BadRegionMap known;
    RECOVERY_ASSERT_OK(known.add({10'000, 4096, 1117}));
    ScanOptions options;
    options.reads.knownBadRegions = &known;
    std::vector<SignatureHit> hits;
    Result<ScanReport> report = scan(source, hits, options);
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(offsetsOf(hits), (std::vector<std::uint64_t>{1000, 30'000}));
    EXPECT_EQ(report->unreadableRegions, (std::vector<storage::BadRegion>{{10'000, 4096, 1117}}));
}

TEST_F(SignatureScannerTest, FatalReadErrorsStopTheScan) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    source.addFatalSector(50);
    RECOVERY_ASSERT_OK(source.open());
    std::vector<SignatureHit> hits;
    const Result<ScanReport> report = scan(source, hits);
    ASSERT_FALSE(report.ok());
    EXPECT_NE(report.error().code, ErrorCode::Cancelled);
}

TEST_F(SignatureScannerTest, SinkErrorsStopTheScan) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    source.plant(100, test::makeSizedFile(10));
    source.plant(5000, test::makeSizedFile(10));
    RECOVERY_ASSERT_OK(source.open());
    const SignatureScanner scanner = makeScanner();
    int calls = 0;
    const Result<ScanReport> failed = scanner.scan(source, [&](const SignatureHit&) {
        ++calls;
        return Status(makeError(ErrorCode::DestinationError, "full"));
    });
    RECOVERY_EXPECT_ERROR(failed, ErrorCode::DestinationError);
    EXPECT_EQ(calls, 1);

    // Cancelled from the sink ends the scan as cancelled, at the hit it refused.
    std::vector<std::uint64_t> seen;
    const Result<ScanReport> cancelled = scanner.scan(source, [&](const SignatureHit& hit) {
        seen.push_back(hit.fileOffset);
        return hit.fileOffset == 5000 ? Status(makeError(ErrorCode::Cancelled, "stop")) : success();
    });
    RECOVERY_ASSERT_OK(cancelled);
    EXPECT_EQ(cancelled->outcome, ScanOutcome::Cancelled);
    EXPECT_EQ(cancelled->nextOffset, 5000u);
    EXPECT_EQ(seen, (std::vector<std::uint64_t>{100, 5000}));
}

TEST_F(SignatureScannerTest, CancellationBeforeTheScanReadsNothing) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    RECOVERY_ASSERT_OK(source.open());
    CancellationSource cancel;
    cancel.requestCancellation();
    ScanOptions options;
    options.startOffset = 512;
    options.reads.cancellation = cancel.token();
    std::vector<SignatureHit> hits;
    Result<ScanReport> report = scan(source, hits, options);
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(report->outcome, ScanOutcome::Cancelled);
    EXPECT_EQ(report->nextOffset, 512u);
    EXPECT_EQ(source.stats().reads, 0u);
}

TEST_F(SignatureScannerTest, CancellationStopsPromptlyAndTheRestCanBeScannedLater) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(8 * kMiB);
    std::vector<std::uint64_t> expected;
    for (std::uint64_t offset = 1000; offset < 8 * kMiB; offset += 100'000) {
        source.plant(offset, test::makeSizedFile(10));
        expected.push_back(offset);
    }
    RECOVERY_ASSERT_OK(source.open());
    CancellationSource cancel;
    std::size_t readsAtCancel = 0;
    source.setReadHook([&](std::uint64_t, std::size_t) {
        if (source.stats().reads == 3) {
            cancel.requestCancellation();
            readsAtCancel = 4;
        }
    });
    ScanOptions options;
    options.blockSize = 64 * 1024;
    options.reads.cancellation = cancel.token();
    std::vector<SignatureHit> hits;
    Result<ScanReport> report = scan(source, hits, options);
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(report->outcome, ScanOutcome::Cancelled);
    EXPECT_EQ(source.stats().reads, readsAtCancel);
    EXPECT_LT(report->nextOffset, 8 * kMiB);
    for (const SignatureHit& hit : hits) {
        EXPECT_LT(hit.fileOffset, report->nextOffset);
    }

    source.setReadHook({});
    options.reads.cancellation = {};
    options.startOffset = report->nextOffset;
    std::vector<SignatureHit> rest;
    RECOVERY_ASSERT_OK(scan(source, rest, options));
    std::vector<std::uint64_t> all = offsetsOf(hits);
    const std::vector<std::uint64_t> later = offsetsOf(rest);
    all.insert(all.end(), later.begin(), later.end());
    EXPECT_EQ(all, expected);
}

TEST_F(SignatureScannerTest, HitLimitStopsTheScanAndItCanContinue) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    std::vector<std::uint64_t> expected;
    for (std::uint64_t offset = 100; offset < 60'000; offset += 5000) {
        source.plant(offset, test::makeSizedFile(10));
        expected.push_back(offset);
    }
    RECOVERY_ASSERT_OK(source.open());
    ScanOptions options;
    options.maxHits = 3;
    std::vector<SignatureHit> hits;
    Result<ScanReport> report = scan(source, hits, options);
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(report->outcome, ScanOutcome::HitLimitReached);
    EXPECT_EQ(report->hits, 3u);
    EXPECT_EQ(report->nextOffset, expected[2] + 1);

    options.maxHits = 1'000;
    options.startOffset = report->nextOffset;
    std::vector<SignatureHit> rest;
    RECOVERY_ASSERT_OK(scan(source, rest, options));
    std::vector<std::uint64_t> all = offsetsOf(hits);
    const std::vector<std::uint64_t> later = offsetsOf(rest);
    all.insert(all.end(), later.begin(), later.end());
    EXPECT_EQ(all, expected);
}

TEST_F(SignatureScannerTest, ReportsProgressAndSurvivesThrowingCallbacks) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(1 * kMiB);
    source.plant(300'000, test::makeSizedFile(10));
    RECOVERY_ASSERT_OK(source.open());
    std::vector<ScanProgress> progress;
    ScanOptions options;
    options.blockSize = 64 * 1024;
    options.progressInterval = std::chrono::milliseconds(0);
    options.onProgress = [&](const ScanProgress& p) {
        progress.push_back(p);
        throw std::runtime_error("callback failure");
    };
    std::vector<SignatureHit> hits;
    Result<ScanReport> report = scan(source, hits, options);
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(hits.size(), 1u);
    ASSERT_GE(progress.size(), 16u);
    for (std::size_t i = 1; i < progress.size(); ++i) {
        EXPECT_GE(progress[i].position, progress[i - 1].position);
        EXPECT_GE(progress[i].bytesRead, progress[i - 1].bytesRead);
    }
    EXPECT_EQ(progress.back().position, 1 * kMiB);
    EXPECT_EQ(progress.back().bytesRead, 1 * kMiB);
    EXPECT_EQ(progress.back().hits, 1u);
    EXPECT_EQ(progress.back().endOffset, 1 * kMiB);
}

TEST_F(SignatureScannerTest, LogsStartEndAndUnreadableRanges) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    source.addBadSector(7);
    RECOVERY_ASSERT_OK(source.open());
    diagnostics::Logger logger(diagnostics::LogLevel::Debug);
    auto sink = std::make_shared<diagnostics::MemorySink>();
    logger.addSink(sink);
    ScanOptions options;
    options.logger = &logger;
    std::vector<SignatureHit> hits;
    RECOVERY_ASSERT_OK(scan(source, hits, options));
    std::vector<std::string> messages;
    for (const diagnostics::LogRecord& record : sink->records()) {
        messages.push_back(record.message);
        EXPECT_EQ(record.component, "carving");
    }
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(messages[0], "signature scan started");
    EXPECT_EQ(messages[1], "unreadable source range skipped by the signature scan");
    EXPECT_EQ(messages[2], "signature scan ended");
}

TEST_F(SignatureScannerTest, ReadsAreSequentialAndBounded) {
    addStandardFormats();
    VirtualSource source(3 * kMiB + 1234);
    source.plant(3 * kMiB + 1200, test::makeSizedFile(10));
    RECOVERY_ASSERT_OK(source.open());
    ScanOptions options;
    options.blockSize = 256 * 1024;
    options.startOffset = 777;  // unaligned: the first read starts at the sector before it
    EXPECT_EQ(scanOffsets(source, options), (std::vector<std::uint64_t>{3 * kMiB + 1200}));
    const VirtualSource::Stats stats = source.stats();
    EXPECT_EQ(stats.backwardReads, 0u);
    EXPECT_LE(stats.maxReadLength, options.blockSize);
    EXPECT_EQ(stats.bytes, 3 * kMiB + 1234 - 512);
    EXPECT_EQ(stats.highestEnd, 3 * kMiB + 1234);
}

TEST_F(SignatureScannerTest, WorksOnADiskThatEnforcesSectorAlignment) {
    addStandardFormats();
    auto config = std::make_shared<test::FakeDeviceConfig>();
    config->data.resize(1 * kMiB);
    const std::vector<std::byte> sized = test::makeSizedFile(50);
    const std::vector<std::byte> box = test::makeBoxFile(5, {7});
    std::copy(sized.begin(), sized.end(), config->data.begin() + 4093);
    std::copy(box.begin(), box.end(), config->data.begin() + 600'001);
    config->geometry = test::diskGeometry(config->data.size(), 4096);
    auto opener = std::make_shared<test::MockDeviceOpener>(config);
    storage::PhysicalDiskSource disk(2, opener);
    RECOVERY_ASSERT_OK(disk.open());
    ScanOptions options;
    options.startOffset = 3;
    options.blockSize = 64 * 1024;
    EXPECT_EQ(scanOffsets(disk, options), (std::vector<std::uint64_t>{4093, 600'001}));
    EXPECT_EQ(opener->stats()->alignmentViolations.load(), 0u);
}

}  // namespace
}  // namespace recovery::carving
