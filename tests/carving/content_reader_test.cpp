// Content readers: bounds of every read, the cache of SourceContentReader,
// bad and known-bad sectors (zeros, recorded), fatal read errors,
// cancellation, and findPattern across chunk boundaries.

#include "carving/content_reader.hpp"

#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

namespace recovery::carving {
namespace {

constexpr std::size_t kSourceSize = 256 * 1024;

std::vector<std::byte> slice(const std::vector<std::byte>& data, std::uint64_t offset, std::size_t length) {
    return {data.begin() + static_cast<std::ptrdiff_t>(offset),
            data.begin() + static_cast<std::ptrdiff_t>(offset + length)};
}

std::vector<std::byte> toVector(std::span<const std::byte> bytes) {
    return {bytes.begin(), bytes.end()};
}

class SourceContentReaderTest : public ::testing::Test {
protected:
    SourceContentReaderTest() : pattern_(test::makePattern(kSourceSize, 17)), source_(pattern_, 512) {
        EXPECT_TRUE(source_.open().ok());
    }

    std::unique_ptr<SourceContentReader> open(std::uint64_t start, std::uint64_t size, SourceReadOptions options = {},
                                              std::size_t cacheSize = SourceContentReader::kDefaultCacheSize) {
        Result<std::unique_ptr<SourceContentReader>> reader =
            SourceContentReader::open(source_, start, size, std::move(options), cacheSize);
        EXPECT_TRUE(reader.ok()) << describe(reader.error());
        return reader.ok() ? std::move(reader).value() : nullptr;
    }

    std::vector<std::byte> pattern_;
    test::MemoryStorageSource source_;
};

TEST(MemoryContentReaderTest, ReadsInsideItsDataOnly) {
    const std::vector<std::byte> data = test::makePattern(1000, 3);
    MemoryContentReader reader(data);
    EXPECT_EQ(reader.size(), 1000u);
    Result<std::span<const std::byte>> read = reader.read(990, 10);
    RECOVERY_ASSERT_OK(read);
    EXPECT_EQ(toVector(*read), slice(data, 990, 10));
    RECOVERY_EXPECT_OK(reader.read(1000, 0));
    RECOVERY_EXPECT_ERROR(reader.read(991, 10), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(reader.read(1001, 0), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(reader.read(std::numeric_limits<std::uint64_t>::max(), 2), ErrorCode::InvalidInput);
}

TEST(MemoryContentReaderTest, RejectsReadsLongerThanTheLimit) {
    const std::vector<std::byte> data(IContentReader::kMaxReadLength + 10);
    MemoryContentReader reader(data);
    RECOVERY_EXPECT_OK(reader.read(0, IContentReader::kMaxReadLength));
    RECOVERY_EXPECT_ERROR(reader.read(0, IContentReader::kMaxReadLength + 1), ErrorCode::InvalidInput);
}

TEST_F(SourceContentReaderTest, OpenChecksTheSourceTheRangeAndTheOptions) {
    RECOVERY_EXPECT_ERROR(SourceContentReader::open(source_, kSourceSize - 10, 11), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(SourceContentReader::open(source_, std::numeric_limits<std::uint64_t>::max(), 2),
                          ErrorCode::InvalidInput);
    RECOVERY_EXPECT_OK(SourceContentReader::open(source_, kSourceSize, 0));
    RECOVERY_EXPECT_ERROR(SourceContentReader::open(source_, 0, 10, {}, SourceContentReader::kMinCacheSize - 1),
                          ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(SourceContentReader::open(source_, 0, 10, {}, IContentReader::kMaxReadLength + 1),
                          ErrorCode::InvalidInput);
    SourceReadOptions retries;
    retries.sectorRetryCount = SourceReadOptions::kMaxSectorRetries + 1;
    RECOVERY_EXPECT_ERROR(SourceContentReader::open(source_, 0, 10, retries), ErrorCode::InvalidInput);
    source_.close();
    RECOVERY_EXPECT_ERROR(SourceContentReader::open(source_, 0, 10), ErrorCode::InvalidInput);
}

TEST_F(SourceContentReaderTest, ReadsTheWindowInContentOffsets) {
    auto reader = open(1000, 50'000);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->size(), 50'000u);
    EXPECT_EQ(reader->start(), 1000u);
    for (const std::uint64_t offset : {0ULL, 1ULL, 4095ULL, 4096ULL, 30'000ULL, 49'990ULL}) {
        Result<std::span<const std::byte>> read = reader->read(offset, 10);
        RECOVERY_ASSERT_OK(read);
        EXPECT_EQ(toVector(*read), slice(pattern_, 1000 + offset, 10)) << offset;
    }
    // Never beyond the window, even though the source continues.
    RECOVERY_EXPECT_ERROR(reader->read(49'991, 10), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(reader->read(50'000, 1), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_OK(reader->read(50'000, 0));
}

TEST_F(SourceContentReaderTest, SmallReadsAreServedFromTheCache) {
    auto reader = open(0, 200'000, {}, 64 * 1024);
    ASSERT_NE(reader, nullptr);
    RECOVERY_ASSERT_OK(reader->read(0, 16));
    // The first load is small (most carves stop at the header).
    EXPECT_EQ(reader->bytesRead(), SourceContentReader::kMinCacheSize);
    const std::size_t readsBefore = source_.readCount();
    RECOVERY_ASSERT_OK(reader->read(4000, 50));  // inside the first load
    EXPECT_EQ(source_.readCount(), readsBefore);
    RECOVERY_ASSERT_OK(reader->read(4090, 50));  // crosses its end: a cache-sized load
    EXPECT_EQ(reader->bytesRead(), SourceContentReader::kMinCacheSize + 64 * 1024);
    for (std::uint64_t offset = 4090; offset + 100 <= 4090 + 64 * 1024; offset += 1000) {
        Result<std::span<const std::byte>> read = reader->read(offset, 100);
        RECOVERY_ASSERT_OK(read);
        EXPECT_EQ(toVector(*read), slice(pattern_, offset, 100));
    }
    EXPECT_EQ(source_.readCount(), readsBefore + 1);
}

TEST_F(SourceContentReaderTest, ReadsLargerThanTheCacheLoadWhatTheyNeed) {
    auto reader = open(0, kSourceSize, {}, SourceContentReader::kMinCacheSize);
    ASSERT_NE(reader, nullptr);
    Result<std::span<const std::byte>> read = reader->read(777, 100'000);
    RECOVERY_ASSERT_OK(read);
    EXPECT_EQ(toVector(*read), slice(pattern_, 777, 100'000));
}

TEST_F(SourceContentReaderTest, TruncateOnlyShrinks) {
    auto reader = open(0, 10'000);
    ASSERT_NE(reader, nullptr);
    RECOVERY_ASSERT_OK(reader->read(0, 8000));
    reader->truncate(5000);
    EXPECT_EQ(reader->size(), 5000u);
    RECOVERY_EXPECT_OK(reader->read(4990, 10));
    // Cached, but no longer part of the content.
    RECOVERY_EXPECT_ERROR(reader->read(4991, 10), ErrorCode::InvalidInput);
    reader->truncate(9000);
    EXPECT_EQ(reader->size(), 5000u);
}

TEST_F(SourceContentReaderTest, BadSectorsReadAsZerosAndAreRecorded) {
    source_.addBadSector(10, 23);  // [5120, 5632)
    auto reader = open(4096, 8192);
    ASSERT_NE(reader, nullptr);
    Result<std::span<const std::byte>> read = reader->read(0, 8192);
    RECOVERY_ASSERT_OK(read);
    std::vector<std::byte> expected = slice(pattern_, 4096, 8192);
    std::fill(expected.begin() + 1024, expected.begin() + 1536, std::byte{0});
    EXPECT_EQ(toVector(*read), expected);
    EXPECT_EQ(reader->unreadable().regions(), (std::vector<storage::BadRegion>{{5120, 512, 23}}));
    EXPECT_FALSE(reader->sourceFailure().has_value());
}

TEST_F(SourceContentReaderTest, TransientFailuresAreRetried) {
    source_.addTransientFailure(12, 1);
    auto reader = open(0, 16'384);
    ASSERT_NE(reader, nullptr);
    Result<std::span<const std::byte>> read = reader->read(0, 16'384);
    RECOVERY_ASSERT_OK(read);
    EXPECT_EQ(toVector(*read), slice(pattern_, 0, 16'384));
    EXPECT_TRUE(reader->unreadable().empty());
}

TEST_F(SourceContentReaderTest, KnownBadRegionsAreNotRead) {
    storage::BadRegionMap known;
    RECOVERY_ASSERT_OK(known.add({2000, 1000, 1117}));
    // Reading it would fail fatally, so the test fails if the reader touches it.
    source_.addFatalSector(2000 / 512 + 1);
    SourceReadOptions options;
    options.knownBadRegions = &known;
    auto reader = open(0, 8000, options);
    ASSERT_NE(reader, nullptr);
    Result<std::span<const std::byte>> read = reader->read(0, 8000);
    RECOVERY_ASSERT_OK(read);
    std::vector<std::byte> expected = slice(pattern_, 0, 8000);
    std::fill(expected.begin() + 2000, expected.begin() + 3000, std::byte{0});
    EXPECT_EQ(toVector(*read), expected);
    EXPECT_EQ(reader->unreadable().regions(), (std::vector<storage::BadRegion>{{2000, 1000, 1117}}));
}

TEST_F(SourceContentReaderTest, FatalReadErrorsFailAndAreRemembered) {
    source_.addFatalSector(3);
    auto reader = open(0, 8000);
    ASSERT_NE(reader, nullptr);
    Result<std::span<const std::byte>> read = reader->read(0, 100);
    ASSERT_FALSE(read.ok());
    EXPECT_NE(read.error().code, ErrorCode::Cancelled);
    EXPECT_TRUE(reader->sourceFailure().has_value());
}

TEST_F(SourceContentReaderTest, CancellationStopsReads) {
    CancellationSource cancel;
    SourceReadOptions options;
    options.cancellation = cancel.token();
    auto reader = open(0, 100'000, options);
    ASSERT_NE(reader, nullptr);
    RECOVERY_ASSERT_OK(reader->read(0, 100));
    cancel.requestCancellation();
    // Still cached.
    RECOVERY_EXPECT_OK(reader->read(50, 50));
    RECOVERY_EXPECT_ERROR(reader->read(90'000, 100), ErrorCode::Cancelled);
    EXPECT_FALSE(reader->sourceFailure().has_value());
}

class FindPatternTest : public ::testing::Test {
protected:
    // Larger than two search chunks (256 KiB), zeros except for the planted patterns.
    FindPatternTest() : data_(600 * 1024) {}

    void plant(std::size_t offset) { std::copy(pattern_.begin(), pattern_.end(), data_.begin() + offset); }

    std::optional<std::uint64_t> find(std::uint64_t from, std::uint64_t to) {
        MemoryContentReader reader(data_);
        Result<std::optional<std::uint64_t>> found = findPattern(reader, pattern_, from, to);
        EXPECT_TRUE(found.ok()) << describe(found.error());
        return found.ok() ? *found : std::nullopt;
    }

    std::vector<std::byte> data_;
    std::vector<std::byte> pattern_ = {std::byte{'E'}, std::byte{'N'}, std::byte{'D'}, std::byte{'!'}};
};

TEST_F(FindPatternTest, FindsTheFirstOccurrence) {
    plant(1000);
    plant(5000);
    EXPECT_EQ(find(0, data_.size()), 1000u);
    EXPECT_EQ(find(1001, data_.size()), 5000u);
    EXPECT_EQ(find(5001, data_.size()), std::nullopt);
}

TEST_F(FindPatternTest, FindsPatternsAcrossChunkBoundaries) {
    for (const std::size_t boundary : {256u * 1024u, 512u * 1024u - 3u}) {
        for (std::size_t shift = 0; shift < pattern_.size(); ++shift) {
            std::fill(data_.begin(), data_.end(), std::byte{0});
            const std::size_t offset = boundary - shift;
            plant(offset);
            EXPECT_EQ(find(0, data_.size()), offset) << "at " << offset;
        }
    }
}

TEST_F(FindPatternTest, OnlyMatchesEntirelyInsideTheRange) {
    plant(2000);
    EXPECT_EQ(find(0, 2004), 2000u);
    EXPECT_EQ(find(0, 2003), std::nullopt);
    EXPECT_EQ(find(2001, 10'000), std::nullopt);
    EXPECT_EQ(find(10'000, 5), std::nullopt);
    // `to` is clipped to the content.
    plant(data_.size() - 4);
    EXPECT_EQ(find(3000, std::numeric_limits<std::uint64_t>::max()), data_.size() - 4);
}

TEST_F(FindPatternTest, RejectsAnEmptyPattern) {
    MemoryContentReader reader(data_);
    RECOVERY_EXPECT_ERROR(findPattern(reader, {}, 0, 100), ErrorCode::InvalidInput);
}

}  // namespace
}  // namespace recovery::carving
