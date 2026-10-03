// A candidate's data as a file's content (CandidateContentReader, P12):
// every region kind read as reconstruction delivers it, reads inside one
// region and across several, bad sectors, stored bytes beyond the end of a
// truncated source, offsets beyond 4 GiB, fatal source errors, and
// malformed candidates and options.

#include "recovery/candidate_content.hpp"

#include "support/candidate_helpers.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace recovery {
namespace {

using Bytes = std::vector<std::byte>;

constexpr std::size_t kSourceSize = 64 * 1024;

RecoveryCandidate candidateWith(std::vector<SourceRegion> regions, Bytes embedded = {}) {
    RecoveryCandidate candidate;
    candidate.id = CandidateId{9};
    candidate.filename = "x.mp4";
    candidate.expectedSize = regions.empty() ? 0 : regions.back().fileOffset + regions.back().length;
    candidate.sourceRegions = std::move(regions);
    candidate.embeddedData = std::move(embedded);
    return candidate;
}

Bytes slice(const Bytes& data, std::size_t offset, std::size_t length) {
    return {data.begin() + static_cast<std::ptrdiff_t>(offset),
            data.begin() + static_cast<std::ptrdiff_t>(offset + length)};
}

Bytes bytesOf(std::span<const std::byte> span) {
    return {span.begin(), span.end()};
}

class CandidateContentTest : public ::testing::Test {
protected:
    CandidateContentTest() : pattern_(test::makePattern(kSourceSize, 17)), source_(pattern_, 512) {
        EXPECT_TRUE(source_.open().ok());
    }

    [[nodiscard]] std::unique_ptr<CandidateContentReader> open(const RecoveryCandidate& candidate) {
        Result<std::unique_ptr<CandidateContentReader>> reader = CandidateContentReader::open(source_, candidate);
        EXPECT_TRUE(reader.ok()) << (reader.ok() ? "" : describe(reader.error()));
        return reader.ok() ? std::move(reader).value() : nullptr;
    }

    Bytes pattern_;
    test::MemoryStorageSource source_;
};

// Every region kind; a Missing region inside the file reads as zeros, one at the end is not part of it.
RecoveryCandidate everyKind(const Bytes& embedded) {
    return candidateWith(
        {
            {0, 1000, RegionKind::Stored, 5000, false},
            {1000, 500, RegionKind::Zeros, 0, false},
            {1500, 300, RegionKind::Embedded, 10, false},
            {1800, 1000, RegionKind::Stored, 20000, true},
            {2800, 200, RegionKind::Missing, 0, false},
            {3000, 500, RegionKind::Stored, 100, false},
            {3500, 700, RegionKind::Missing, 0, false},
        },
        embedded);
}

TEST_F(CandidateContentTest, ReadsWhatReconstructionDelivers) {
    const Bytes embedded = test::makePattern(400, 3);
    const RecoveryCandidate candidate = everyKind(embedded);
    const std::unique_ptr<CandidateContentReader> content = open(candidate);
    ASSERT_NE(content, nullptr);
    EXPECT_EQ(content->size(), 3500u);

    Bytes expected = slice(pattern_, 5000, 1000);
    expected.resize(1500);
    const Bytes resident = slice(embedded, 10, 300);
    expected.insert(expected.end(), resident.begin(), resident.end());
    const Bytes reused = slice(pattern_, 20000, 1000);
    expected.insert(expected.end(), reused.begin(), reused.end());
    expected.resize(3000);
    const Bytes last = slice(pattern_, 100, 500);
    expected.insert(expected.end(), last.begin(), last.end());

    const Result<std::span<const std::byte>> whole = content->read(0, 3500);
    RECOVERY_ASSERT_OK(whole);
    EXPECT_TRUE(bytesOf(*whole) == expected);
    // What a recovered file would hold.
    const test::Reconstructed rebuilt = test::reconstructToMemory(source_, candidate);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_TRUE(rebuilt.data == expected);
    EXPECT_TRUE(content->outsideSource().empty());
    EXPECT_TRUE(content->unreadable().empty());
    EXPECT_FALSE(content->sourceFailure().has_value());
    EXPECT_GT(content->bytesRead(), 0u);
}

TEST_F(CandidateContentTest, ReadsInsideOneRegionAndAcrossSeveral) {
    const Bytes embedded = test::makePattern(400, 4);
    const RecoveryCandidate candidate = everyKind(embedded);
    const test::Reconstructed rebuilt = test::reconstructToMemory(source_, candidate);
    ASSERT_TRUE(rebuilt.ok);
    const std::unique_ptr<CandidateContentReader> content = open(candidate);
    ASSERT_NE(content, nullptr);
    // Every region boundary, and reads of every length around it.
    const std::vector<std::uint64_t> boundaries = {0, 1000, 1500, 1800, 2800, 3000, 3499};
    for (const std::uint64_t boundary : boundaries) {
        for (const std::uint64_t before : {0U, 1U, 7U, 300U}) {
            for (const std::size_t length : {1U, 2U, 64U, 777U}) {
                const std::uint64_t offset = boundary >= before ? boundary - before : 0;
                if (offset + length > content->size()) {
                    continue;
                }
                const Result<std::span<const std::byte>> bytes = content->read(offset, length);
                RECOVERY_ASSERT_OK(bytes);
                ASSERT_TRUE(bytesOf(*bytes) == slice(rebuilt.data, offset, length))
                    << "offset " << offset << ", length " << length;
            }
        }
    }
    const Result<std::span<const std::byte>> none = content->read(3500, 0);
    RECOVERY_ASSERT_OK(none);
    EXPECT_TRUE(none->empty());
}

TEST_F(CandidateContentTest, RefusesReadsOutsideTheFile) {
    const std::unique_ptr<CandidateContentReader> content =
        open(candidateWith({{0, 4000, RegionKind::Stored, 0, false}}));
    ASSERT_NE(content, nullptr);
    RECOVERY_EXPECT_ERROR(content->read(4000, 1), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(content->read(3999, 2), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(content->read(UINT64_MAX, 2), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(content->read(0, carving::IContentReader::kMaxReadLength + 1), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_OK(content->read(3999, 1));
}

TEST_F(CandidateContentTest, UnreadableSectorsReadAsZerosAndAreRecorded) {
    test::MemoryStorageSource failing(pattern_, 512);
    failing.addBadSector(12);  // source bytes [6144, 6656)
    RECOVERY_ASSERT_OK(failing.open());
    const RecoveryCandidate candidate =
        candidateWith({{0, 2000, RegionKind::Stored, 5000, false}, {2000, 1000, RegionKind::Stored, 30000, false}});
    Result<std::unique_ptr<CandidateContentReader>> content = CandidateContentReader::open(failing, candidate);
    RECOVERY_ASSERT_OK(content);
    const Result<std::span<const std::byte>> bytes = (*content)->read(0, 3000);
    RECOVERY_ASSERT_OK(bytes);
    Bytes expected = slice(pattern_, 5000, 2000);
    std::fill(expected.begin() + (6144 - 5000), expected.begin() + (6656 - 5000), std::byte{0});
    const Bytes second = slice(pattern_, 30000, 1000);
    expected.insert(expected.end(), second.begin(), second.end());
    EXPECT_TRUE(bytesOf(*bytes) == expected);
    EXPECT_EQ((*content)->unreadable().totalBytes(), 512u);
    EXPECT_TRUE((*content)->unreadable().intersects(6144, 512));
    // A bad sector is not a failure of the source.
    EXPECT_FALSE((*content)->sourceFailure().has_value());
}

TEST_F(CandidateContentTest, StoredBytesBeyondTheEndOfTheSourceReadAsZeros) {
    // A truncated image: the file's regions reach past its end.
    const RecoveryCandidate candidate = candidateWith({
        {0, 1000, RegionKind::Stored, kSourceSize - 400, false},
        {1000, 500, RegionKind::Zeros, 0, false},
        {1500, 300, RegionKind::Stored, kSourceSize + 1000, false},
        {1800, 200, RegionKind::Stored, 0, false},
    });
    const std::unique_ptr<CandidateContentReader> content = open(candidate);
    ASSERT_NE(content, nullptr);
    const Result<std::span<const std::byte>> bytes = content->read(0, 2000);
    RECOVERY_ASSERT_OK(bytes);
    Bytes expected = slice(pattern_, kSourceSize - 400, 400);
    expected.resize(1800);
    const Bytes tail = slice(pattern_, 0, 200);
    expected.insert(expected.end(), tail.begin(), tail.end());
    EXPECT_TRUE(bytesOf(*bytes) == expected);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> outside = {{400, 1000}, {1500, 1800}};
    EXPECT_EQ(content->outsideSource(), outside);
    // Read again, in pieces: the ranges are merged, not repeated.
    for (std::uint64_t offset = 0; offset < 2000; offset += 250) {
        RECOVERY_ASSERT_OK(content->read(offset, 250));
    }
    EXPECT_EQ(content->outsideSource(), outside);
    // Inside one stored region that lies entirely beyond the source.
    const Result<std::span<const std::byte>> beyond = content->read(1600, 100);
    RECOVERY_ASSERT_OK(beyond);
    EXPECT_TRUE(std::all_of(beyond->begin(), beyond->end(), [](std::byte b) { return b == std::byte{0}; }));
}

TEST(CandidateContentLargeTest, ReadsBeyond4GiB) {
    test::VirtualSource source(12 * kGiB);
    const Bytes first = test::makePattern(3000, 21);
    const Bytes second = test::makePattern(5000, 22);
    source.plant(5 * kGiB + 123, first);
    source.plant(11 * kGiB + 7, second);
    RECOVERY_ASSERT_OK(source.open());
    const RecoveryCandidate candidate = candidateWith(
        {{0, 3000, RegionKind::Stored, 5 * kGiB + 123, false}, {3000, 5000, RegionKind::Stored, 11 * kGiB + 7, false}});
    Result<std::unique_ptr<CandidateContentReader>> content = CandidateContentReader::open(source, candidate);
    RECOVERY_ASSERT_OK(content);
    EXPECT_EQ((*content)->size(), 8000u);
    const Result<std::span<const std::byte>> bytes = (*content)->read(2000, 3000);
    RECOVERY_ASSERT_OK(bytes);
    Bytes expected = slice(first, 2000, 1000);
    const Bytes more = slice(second, 0, 2000);
    expected.insert(expected.end(), more.begin(), more.end());
    EXPECT_TRUE(bytesOf(*bytes) == expected);
    EXPECT_LT((*content)->bytesRead(), 4 * kMiB);
}

TEST_F(CandidateContentTest, FatalSourceErrorsArePassedOn) {
    test::MemoryStorageSource failing(pattern_, 512);
    failing.addFatalSector(20);  // source bytes [10240, 10752)
    RECOVERY_ASSERT_OK(failing.open());
    Result<std::unique_ptr<CandidateContentReader>> content =
        CandidateContentReader::open(failing, candidateWith({{0, 4000, RegionKind::Stored, 9000, false}}));
    RECOVERY_ASSERT_OK(content);
    RECOVERY_EXPECT_ERROR((*content)->read(0, 4000), ErrorCode::InternalError);
    EXPECT_TRUE((*content)->sourceFailure().has_value());
}

TEST_F(CandidateContentTest, RefusesMalformedCandidatesClosedSourcesAndInvalidOptions) {
    // Regions that leave a gap.
    const RecoveryCandidate gap =
        candidateWith({{0, 100, RegionKind::Stored, 0, false}, {200, 100, RegionKind::Stored, 0, false}});
    RECOVERY_EXPECT_ERROR(CandidateContentReader::open(source_, gap), ErrorCode::InvalidInput);
    // Embedded bytes the candidate does not hold.
    const RecoveryCandidate resident =
        candidateWith({{0, 100, RegionKind::Embedded, 0, false}}, test::makePattern(50, 1));
    RECOVERY_EXPECT_ERROR(CandidateContentReader::open(source_, resident), ErrorCode::InvalidInput);

    const RecoveryCandidate valid = candidateWith({{0, 100, RegionKind::Stored, 0, false}});
    test::MemoryStorageSource closed(pattern_, 512);
    RECOVERY_EXPECT_ERROR(CandidateContentReader::open(closed, valid), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(CandidateContentReader::open(source_, valid, {}, 16), ErrorCode::InvalidInput);
    carving::SourceReadOptions retries;
    retries.sectorRetryCount = carving::SourceReadOptions::kMaxSectorRetries + 1;
    RECOVERY_EXPECT_ERROR(CandidateContentReader::open(source_, valid, retries), ErrorCode::InvalidInput);

    // Nothing located: an empty file.
    const std::unique_ptr<CandidateContentReader> empty =
        open(candidateWith({{0, 500, RegionKind::Missing, 0, false}}));
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(empty->size(), 0u);
    RECOVERY_EXPECT_ERROR(empty->read(0, 1), ErrorCode::InvalidInput);
}

}  // namespace
}  // namespace recovery
