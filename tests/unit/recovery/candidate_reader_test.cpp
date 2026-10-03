// Reconstruction of hand-made candidates from an in-memory source: every
// region kind, chunking, bad sectors, known bad regions, a truncated source,
// malformed candidates and options, cancellation and sink errors.

#include "recovery/candidate_reader.hpp"

#include "support/candidate_helpers.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace recovery {
namespace {

constexpr std::size_t kSourceSize = 64 * 1024;

RecoveryCandidate candidateWith(std::vector<SourceRegion> regions, std::vector<std::byte> embedded = {}) {
    RecoveryCandidate candidate;
    candidate.id = CandidateId{7};
    candidate.filename = "x.bin";
    candidate.expectedSize = regions.empty() ? 0 : regions.back().fileOffset + regions.back().length;
    candidate.sourceRegions = std::move(regions);
    candidate.embeddedData = std::move(embedded);
    return candidate;
}

std::vector<std::byte> slice(const std::vector<std::byte>& data, std::size_t offset, std::size_t length) {
    return {data.begin() + static_cast<std::ptrdiff_t>(offset),
            data.begin() + static_cast<std::ptrdiff_t>(offset + length)};
}

void append(std::vector<std::byte>& out, const std::vector<std::byte>& more) {
    out.insert(out.end(), more.begin(), more.end());
}

class CandidateReaderTest : public ::testing::Test {
protected:
    CandidateReaderTest() : pattern_(test::makePattern(kSourceSize, 99)), source_(pattern_, 512) {
        EXPECT_TRUE(source_.open().ok());
    }

    std::vector<std::byte> pattern_;
    test::MemoryStorageSource source_;
};

TEST_F(CandidateReaderTest, EveryRegionKind) {
    const auto embedded = test::makePattern(100, 5);
    const RecoveryCandidate candidate = candidateWith(
        {
            {0, 1000, RegionKind::Stored, 5000, false},
            {1000, 500, RegionKind::Zeros, 0, false},
            {1500, 100, RegionKind::Embedded, 0, false},
            {1600, 400, RegionKind::Missing, 0, false},
            {2000, 600, RegionKind::Stored, 100, true},
            {2600, 400, RegionKind::Missing, 0, false},  // missing tail: left out
        },
        embedded);

    const test::Reconstructed rebuilt = test::reconstructToMemory(source_, candidate);
    ASSERT_TRUE(rebuilt.ok);
    std::vector<std::byte> expected = slice(pattern_, 5000, 1000);
    expected.resize(1500);
    append(expected, embedded);
    expected.resize(2000);
    append(expected, slice(pattern_, 100, 600));
    EXPECT_TRUE(rebuilt.data == expected);

    const ReconstructionReport& report = rebuilt.report;
    EXPECT_EQ(report.expectedSize, 3000u);
    EXPECT_EQ(report.outputSize, 2600u);
    EXPECT_EQ(report.storedBytes, 1600u);
    EXPECT_EQ(report.embeddedBytes, 100u);
    EXPECT_EQ(report.zeroBytes, 500u);
    EXPECT_EQ(report.missingBytes, 800u);
    EXPECT_EQ(report.unreadableBytes, 0u);
    EXPECT_EQ(report.reallocatedBytes, 600u);
    EXPECT_TRUE(report.unreadableRegions.empty());
    EXPECT_FALSE(report.allBytesRead());
}

TEST_F(CandidateReaderTest, ChunksAreDeliveredInOrderWithoutOverlap) {
    const RecoveryCandidate candidate =
        candidateWith({{0, 20000, RegionKind::Stored, 777, false}, {20000, 3000, RegionKind::Stored, 30001, false}});
    ReconstructionOptions options;
    options.chunkSize = 4096;
    std::vector<std::pair<std::uint64_t, std::size_t>> calls;
    std::vector<std::byte> out;
    const Result<ReconstructionReport> report = reconstructCandidate(
        source_, candidate,
        [&](std::uint64_t offset, std::span<const std::byte> data) {
            calls.emplace_back(offset, data.size());
            EXPECT_EQ(offset, out.size());
            out.insert(out.end(), data.begin(), data.end());
            return success();
        },
        options);
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(calls.size(), 6u);  // 4 + 1 chunks for the first region, 1 for the second
    for (const auto& call : calls) {
        EXPECT_LE(call.second, 4096u);
    }
    std::vector<std::byte> expected = slice(pattern_, 777, 20000);
    append(expected, slice(pattern_, 30001, 3000));
    EXPECT_TRUE(out == expected);
    EXPECT_TRUE(report->allBytesRead());
}

TEST_F(CandidateReaderTest, BadSectorCostsOnlyThatSector) {
    source_.addBadSector(20);  // bytes [10240, 10752)
    const RecoveryCandidate candidate = candidateWith({{0, 16384, RegionKind::Stored, 8192, false}});
    const test::Reconstructed rebuilt = test::reconstructToMemory(source_, candidate);
    ASSERT_TRUE(rebuilt.ok);
    std::vector<std::byte> expected = slice(pattern_, 8192, 16384);
    std::fill(expected.begin() + 2048, expected.begin() + 2560, std::byte{0});
    EXPECT_TRUE(rebuilt.data == expected);
    EXPECT_EQ(rebuilt.report.outputSize, 16384u);
    EXPECT_EQ(rebuilt.report.storedBytes, 16384u - 512u);
    EXPECT_EQ(rebuilt.report.unreadableBytes, 512u);
    ASSERT_EQ(rebuilt.report.unreadableRegions.size(), 1u);
    EXPECT_EQ(rebuilt.report.unreadableRegions[0],
              (storage::BadRegion{10240, 512, test::MemoryStorageSource::kErrorCrc}));
    EXPECT_FALSE(rebuilt.report.allBytesRead());
}

TEST_F(CandidateReaderTest, UnalignedRegionAroundBadSectors) {
    source_.addBadSector(20);
    source_.addBadSector(21);
    const RecoveryCandidate candidate = candidateWith({{0, 2000, RegionKind::Stored, 10000, false}});
    const test::Reconstructed rebuilt = test::reconstructToMemory(source_, candidate);
    ASSERT_TRUE(rebuilt.ok);
    // Source [10000, 12000): sectors 20-21 are [10240, 11264).
    std::vector<std::byte> expected = slice(pattern_, 10000, 2000);
    std::fill(expected.begin() + 240, expected.begin() + 1264, std::byte{0});
    EXPECT_TRUE(rebuilt.data == expected);
    EXPECT_EQ(rebuilt.report.unreadableBytes, 1024u);
    ASSERT_EQ(rebuilt.report.unreadableRegions.size(), 1u);  // adjacent sectors merged
    EXPECT_EQ(rebuilt.report.unreadableRegions[0].offset, 10240u);
    EXPECT_EQ(rebuilt.report.unreadableRegions[0].length, 1024u);
}

TEST_F(CandidateReaderTest, TransientFailuresAreRetried) {
    // The chunk read and the first sector read fail; the retry succeeds.
    source_.addTransientFailure(20, 2);
    const RecoveryCandidate candidate = candidateWith({{0, 4096, RegionKind::Stored, 8192, false}});
    const test::Reconstructed rebuilt = test::reconstructToMemory(source_, candidate);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_TRUE(rebuilt.data == slice(pattern_, 8192, 4096));
    EXPECT_TRUE(rebuilt.report.allBytesRead());

    test::MemoryStorageSource noRetry(pattern_, 512);
    RECOVERY_ASSERT_OK(noRetry.open());
    noRetry.addTransientFailure(20, 2);
    ReconstructionOptions options;
    options.sectorRetryCount = 0;
    const test::Reconstructed withoutRetry = test::reconstructToMemory(noRetry, candidate, options);
    ASSERT_TRUE(withoutRetry.ok);
    EXPECT_EQ(withoutRetry.report.unreadableBytes, 512u);
}

TEST_F(CandidateReaderTest, NonIoReadFailureStops) {
    source_.addFatalSector(20);
    const RecoveryCandidate candidate = candidateWith({{0, 4096, RegionKind::Stored, 8192, false}});
    const Result<ReconstructionReport> report =
        reconstructCandidate(source_, candidate, [](std::uint64_t, std::span<const std::byte>) { return success(); });
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.error().code, ErrorCode::InternalError);
}

TEST_F(CandidateReaderTest, KnownBadRegionsAreNotRead) {
    storage::BadRegionMap known;
    RECOVERY_ASSERT_OK(known.add({10240, 1024, 23}));
    RECOVERY_ASSERT_OK(known.add({60000, 100, 27}));  // elsewhere on the source
    ReconstructionOptions options;
    options.knownBadRegions = &known;
    const RecoveryCandidate candidate = candidateWith({{0, 16384, RegionKind::Stored, 8192, false}});
    const std::size_t readsBefore = source_.readCount();
    const test::Reconstructed rebuilt = test::reconstructToMemory(source_, candidate, options);
    ASSERT_TRUE(rebuilt.ok);
    std::vector<std::byte> expected = slice(pattern_, 8192, 16384);
    std::fill(expected.begin() + 2048, expected.begin() + 3072, std::byte{0});
    EXPECT_TRUE(rebuilt.data == expected);
    EXPECT_EQ(rebuilt.report.unreadableBytes, 1024u);
    ASSERT_EQ(rebuilt.report.unreadableRegions.size(), 1u);
    EXPECT_EQ(rebuilt.report.unreadableRegions[0], (storage::BadRegion{10240, 1024, 23}));
    EXPECT_EQ(source_.readCount() - readsBefore, 2u);  // the parts before and after the bad region
}

TEST_F(CandidateReaderTest, BytesBeyondTheEndOfTheSourceAreUnreadable) {
    const RecoveryCandidate candidate = candidateWith({{0, 3000, RegionKind::Stored, kSourceSize - 1000, false}});
    const test::Reconstructed rebuilt = test::reconstructToMemory(source_, candidate);
    ASSERT_TRUE(rebuilt.ok);
    std::vector<std::byte> expected = slice(pattern_, kSourceSize - 1000, 1000);
    expected.resize(3000);
    EXPECT_TRUE(rebuilt.data == expected);
    EXPECT_EQ(rebuilt.report.unreadableBytes, 2000u);
    EXPECT_EQ(rebuilt.report.outsideSourceBytes, 2000u);
    EXPECT_TRUE(rebuilt.report.unreadableRegions.empty());

    const RecoveryCandidate farAway = candidateWith({{0, 10, RegionKind::Stored, 1ULL << 40, false}});
    const test::Reconstructed nothing = test::reconstructToMemory(source_, farAway);
    ASSERT_TRUE(nothing.ok);
    EXPECT_EQ(nothing.report.outsideSourceBytes, 10u);
}

TEST_F(CandidateReaderTest, EmptyCandidate) {
    const RecoveryCandidate candidate = candidateWith({});
    int calls = 0;
    const Result<ReconstructionReport> report = reconstructCandidate(
        source_, candidate, [&](std::uint64_t, std::span<const std::byte>) {
            ++calls;
            return success();
        });
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(report->outputSize, 0u);
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(report->allBytesRead());
}

TEST_F(CandidateReaderTest, MalformedCandidatesAreRejected) {
    const auto sink = [](std::uint64_t, std::span<const std::byte>) { return success(); };
    const std::vector<std::vector<SourceRegion>> malformed{
        {{0, 10, RegionKind::Stored, 0, false}, {20, 10, RegionKind::Stored, 0, false}},  // gap
        {{0, 10, RegionKind::Stored, 0, false}, {5, 10, RegionKind::Stored, 0, false}},   // overlap
        {{0, 0, RegionKind::Stored, 0, false}},                                            // empty region
        {{0, 10, RegionKind::Embedded, 0, false}},                                         // no embedded data
        {{0, 10, RegionKind::Stored, ~0ULL - 5, false}},                                   // source offset overflows
        {{0, 10, RegionKind::Zeros, 0, true}},                                             // reallocated zeros
        {{1, 10, RegionKind::Stored, 0, false}},                                           // does not start at 0
    };
    for (const auto& regions : malformed) {
        RecoveryCandidate candidate = candidateWith(regions);
        RECOVERY_EXPECT_ERROR(reconstructCandidate(source_, candidate, sink), ErrorCode::InvalidInput);
    }
    RecoveryCandidate wrongSize = candidateWith({{0, 10, RegionKind::Stored, 0, false}});
    wrongSize.expectedSize = 11;
    RECOVERY_EXPECT_ERROR(reconstructCandidate(source_, wrongSize, sink), ErrorCode::InvalidInput);
    RecoveryCandidate overflow = candidateWith({{0, ~0ULL, RegionKind::Missing, 0, false}});
    overflow.sourceRegions.push_back({~0ULL, 10, RegionKind::Missing, 0, false});
    RECOVERY_EXPECT_ERROR(reconstructCandidate(source_, overflow, sink), ErrorCode::InvalidInput);
}

TEST_F(CandidateReaderTest, InvalidOptionsAndSources) {
    const RecoveryCandidate candidate = candidateWith({{0, 10, RegionKind::Stored, 0, false}});
    const CandidateSink sink = [](std::uint64_t, std::span<const std::byte>) { return success(); };
    ReconstructionOptions small;
    small.chunkSize = 1000;
    RECOVERY_EXPECT_ERROR(reconstructCandidate(source_, candidate, sink, small), ErrorCode::InvalidInput);
    ReconstructionOptions huge;
    huge.chunkSize = storage::kMaxReadSize + 1;
    RECOVERY_EXPECT_ERROR(reconstructCandidate(source_, candidate, sink, huge), ErrorCode::InvalidInput);
    ReconstructionOptions retries;
    retries.sectorRetryCount = ReconstructionOptions::kMaxSectorRetries + 1;
    RECOVERY_EXPECT_ERROR(reconstructCandidate(source_, candidate, sink, retries), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(reconstructCandidate(source_, candidate, CandidateSink{}), ErrorCode::InvalidInput);

    test::MemoryStorageSource closed(pattern_, 512);
    RECOVERY_EXPECT_ERROR(reconstructCandidate(closed, candidate, sink), ErrorCode::InvalidInput);
}

TEST_F(CandidateReaderTest, CancellationStopsReconstruction) {
    CancellationSource cancel;
    ReconstructionOptions options;
    options.chunkSize = 4096;
    options.cancellation = cancel.token();
    const RecoveryCandidate candidate = candidateWith({{0, 32768, RegionKind::Stored, 0, false}});
    int calls = 0;
    const Result<ReconstructionReport> report = reconstructCandidate(
        source_, candidate,
        [&](std::uint64_t, std::span<const std::byte>) {
            if (++calls == 2) {
                cancel.requestCancellation();
            }
            return success();
        },
        options);
    RECOVERY_EXPECT_ERROR(report, ErrorCode::Cancelled);
    EXPECT_EQ(calls, 2);
}

TEST_F(CandidateReaderTest, SinkErrorStopsReconstruction) {
    ReconstructionOptions options;
    options.chunkSize = 4096;
    const RecoveryCandidate candidate = candidateWith({{0, 32768, RegionKind::Stored, 0, false}});
    int calls = 0;
    const Result<ReconstructionReport> report = reconstructCandidate(
        source_, candidate,
        [&](std::uint64_t, std::span<const std::byte>) -> Status {
            if (++calls == 3) {
                return makeError(ErrorCode::DestinationError, "disk full");
            }
            return success();
        },
        options);
    RECOVERY_EXPECT_ERROR(report, ErrorCode::DestinationError);
    EXPECT_EQ(calls, 3);
}

}  // namespace
}  // namespace recovery
