// The scan's source (P15): blocks read once are served again from the cache,
// the cache keeps no more blocks than it may, a block with a bad sector is
// never cached and its reads get the source's own answer (the bad sectors
// found are counted once), reads wait while the scan is paused, and readers
// on several threads get the right bytes.

#include "scan/scan_source.hpp"

#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace recovery::scan {
namespace {

using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;

constexpr std::size_t kBlock = 4096;

Bytes readAt(storage::IStorageSource& source, std::uint64_t offset, std::size_t length,
             storage::ReadStatus expected = storage::ReadStatus::Success) {
    Bytes buffer(length);
    const storage::ReadResult result = source.read(ByteOffset{offset}, buffer);
    EXPECT_EQ(result.status, expected) << "at " << offset;
    return buffer;
}

TEST(ScanSourceTest, BlocksReadOnceAreServedFromTheCache) {
    ::recovery::test::VirtualSource inner(1 * kMiB);
    inner.setNoise(7);
    RECOVERY_ASSERT_OK(inner.open());
    ScanSource source(inner, ScanSourceOptions{kBlock, 16, {}});
    RECOVERY_ASSERT_OK(source.open());
    EXPECT_EQ(source.size(), inner.size());
    EXPECT_EQ(source.sectorSize(), inner.sectorSize());

    // Unaligned, across two blocks.
    EXPECT_EQ(readAt(source, 1000, 5000), inner.contentAt(1000, 5000));
    EXPECT_EQ(source.stats().bytesRead, 2 * kBlock);
    EXPECT_EQ(source.stats().bytesFromCache, 0U);
    // The same bytes again, and others in those blocks: no read of the source.
    EXPECT_EQ(readAt(source, 0, 8192), inner.contentAt(0, 8192));
    EXPECT_EQ(readAt(source, 4090, 10), inner.contentAt(4090, 10));
    const ScanSourceStats stats = source.stats();
    EXPECT_EQ(stats.bytesRead, 2 * kBlock);
    EXPECT_EQ(stats.reads, 2U);
    EXPECT_EQ(stats.bytesRequested, 5000U + 8192U + 10U);
    EXPECT_EQ(stats.bytesFromCache, 8192U + 10U);
}

TEST(ScanSourceTest, TheLastBlockMayBeShort) {
    ::recovery::test::VirtualSource inner(10 * kBlock + 512);
    inner.setNoise(3);
    RECOVERY_ASSERT_OK(inner.open());
    ScanSource source(inner, ScanSourceOptions{kBlock, 4, {}});
    RECOVERY_ASSERT_OK(source.open());
    EXPECT_EQ(readAt(source, 10 * kBlock - 100, 612), inner.contentAt(10 * kBlock - 100, 612));
    readAt(source, 10 * kBlock, 1024, storage::ReadStatus::OutOfRange);
}

TEST(ScanSourceTest, TheCacheKeepsNoMoreBlocksThanItMay) {
    ::recovery::test::VirtualSource inner(64 * kBlock);
    RECOVERY_ASSERT_OK(inner.open());
    ScanSource source(inner, ScanSourceOptions{kBlock, 4, {}});
    RECOVERY_ASSERT_OK(source.open());
    for (std::uint64_t block = 0; block < 6; ++block) {
        (void)readAt(source, block * kBlock, 16);
    }
    EXPECT_EQ(source.stats().reads, 6U);
    // Blocks 2-5 are still there; block 0 was dropped (the least recently used).
    (void)readAt(source, 5 * kBlock, 16);
    (void)readAt(source, 2 * kBlock, 16);
    EXPECT_EQ(source.stats().reads, 6U);
    (void)readAt(source, 0, 16);
    EXPECT_EQ(source.stats().reads, 7U);
}

TEST(ScanSourceTest, WithoutACacheEveryReadGoesToTheSource) {
    ::recovery::test::VirtualSource inner(64 * kBlock);
    RECOVERY_ASSERT_OK(inner.open());
    ScanSource source(inner, ScanSourceOptions{kBlock, 0, {}});
    RECOVERY_ASSERT_OK(source.open());
    (void)readAt(source, 0, 100);
    (void)readAt(source, 0, 100);
    EXPECT_EQ(source.stats().reads, 2U);
    EXPECT_EQ(source.stats().bytesRead, 200U);
}

TEST(ScanSourceTest, ABlockWithABadSectorGetsTheSourcesOwnAnswer) {
    const Bytes data = ::recovery::test::makePattern(16 * kBlock, 11);
    ::recovery::test::MemoryStorageSource inner(data);
    inner.addBadSector(3 * kBlock / 512 + 2);  // inside block 3
    RECOVERY_ASSERT_OK(inner.open());
    ScanSource source(inner, ScanSourceOptions{kBlock, 8, {}});
    RECOVERY_ASSERT_OK(source.open());

    // A large read across the bad sector fails as the source's read does.
    Bytes buffer(2 * kBlock);
    const storage::ReadResult failed = source.read(ByteOffset{2 * kBlock}, buffer);
    EXPECT_FALSE(failed.ok());
    EXPECT_EQ(failed.bytesRead, kBlock + 2 * 512);
    // Not a sector on its own: nothing is counted yet.
    EXPECT_EQ(source.stats().unreadableBytes, 0U);
    // Sector by sector, as every reader retries: the good ones read, the bad one is counted.
    for (std::uint64_t sector = 0; sector < kBlock / 512; ++sector) {
        Bytes one(512);
        const storage::ReadResult result = source.read(ByteOffset{3 * kBlock + sector * 512}, one);
        EXPECT_EQ(result.ok(), sector != 2) << sector;
        if (result.ok()) {
            const auto at = static_cast<std::ptrdiff_t>(3 * kBlock + sector * 512);
            EXPECT_TRUE(std::equal(one.begin(), one.end(), data.begin() + at));
        }
    }
    // Retried: still one sector.
    (void)readAt(source, 3 * kBlock + 2 * 512, 512, storage::ReadStatus::IoError);
    EXPECT_EQ(source.stats().unreadableBytes, 512U);
    const std::vector<storage::BadRegion> fresh = source.takeNewUnreadable();
    ASSERT_EQ(fresh.size(), 1U);
    EXPECT_EQ(fresh[0].offset, 3 * kBlock + 2 * 512);
    EXPECT_EQ(fresh[0].length, 512U);
    EXPECT_TRUE(source.takeNewUnreadable().empty());
    EXPECT_EQ(source.unreadableRegions().size(), 1U);
    // The blocks around it are cached as usual.
    const std::uint64_t reads = source.stats().reads;
    (void)readAt(source, 2 * kBlock, 512);
    EXPECT_EQ(source.stats().reads, reads);
}

TEST(ScanSourceTest, ReadsWaitWhileTheScanIsPaused) {
    ::recovery::test::VirtualSource inner(64 * kBlock);
    RECOVERY_ASSERT_OK(inner.open());
    JobControl control;
    ScanSource source(inner, ScanSourceOptions{kBlock, 8, control});
    RECOVERY_ASSERT_OK(source.open());
    control.pause();
    std::atomic<bool> done{false};
    std::thread reader([&] {
        (void)readAt(source, 0, 100);
        done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (control.waitingThreads() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(done.load());
    EXPECT_EQ(source.activeReads(), 0U);
    EXPECT_EQ(inner.stats().reads, 0U);
    control.resume();
    reader.join();
    EXPECT_TRUE(done.load());
    EXPECT_EQ(inner.stats().reads, 1U);
}

TEST(ScanSourceTest, ReadersOnSeveralThreadsGetTheRightBytes) {
    ::recovery::test::VirtualSource inner(4 * kMiB);
    inner.setNoise(99);
    RECOVERY_ASSERT_OK(inner.open());
    ScanSource source(inner, ScanSourceOptions{kBlock, 32, {}});
    RECOVERY_ASSERT_OK(source.open());
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&, t] {
            std::uint64_t state = 0x9E3779B97F4A7C15ULL * static_cast<std::uint64_t>(t + 1);
            for (int i = 0; i < 300; ++i) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                const std::uint64_t offset = (state >> 20) % (4 * kMiB - 20000);
                const std::size_t length = 1 + static_cast<std::size_t>((state >> 8) % 19000);
                Bytes buffer(length);
                const storage::ReadResult result = source.read(ByteOffset{offset}, buffer);
                if (!result.ok() || buffer != inner.contentAt(offset, length)) {
                    wrong.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(wrong.load(), 0);
}

TEST(ScanSourceTest, OptionsMustSuitTheSource) {
    ::recovery::test::VirtualSource inner(64 * kBlock);
    {
        ScanSource notOpen(inner, ScanSourceOptions{kBlock, 8, {}});
        RECOVERY_EXPECT_ERROR(notOpen.open(), ErrorCode::InvalidInput);
    }
    RECOVERY_ASSERT_OK(inner.open());
    ScanSource unaligned(inner, ScanSourceOptions{1000, 8, {}});
    RECOVERY_EXPECT_ERROR(unaligned.open(), ErrorCode::InvalidInput);
    ScanSource huge(inner, ScanSourceOptions{kBlock, ScanSourceOptions::kMaxCacheBlocks + 1, {}});
    RECOVERY_EXPECT_ERROR(huge.open(), ErrorCode::InvalidInput);
    ScanSource zero(inner, ScanSourceOptions{0, 8, {}});
    RECOVERY_EXPECT_ERROR(zero.open(), ErrorCode::InvalidInput);
}

}  // namespace
}  // namespace recovery::scan
