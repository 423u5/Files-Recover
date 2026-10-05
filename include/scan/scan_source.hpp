#pragma once

// The source as a scan reads it (P15): reads wait while the scan is paused,
// blocks read once are served from a bounded cache to every stage that
// needs them again, and what the device delivered, and could not deliver, is
// counted.

#include "recovery/config.hpp"
#include "recovery/job_control.hpp"
#include "recovery/result.hpp"
#include "storage/bad_region.hpp"
#include "storage/storage_source.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace recovery::scan {

struct ScanSourceOptions {
    static constexpr std::size_t kMaxCacheBlocks = 4096;

    // Size of a cached block: a multiple of the source's sector size, at most
    // storage::kMaxReadSize.
    std::size_t blockSize = 1 * kMiB;
    // Blocks kept (at most kMaxCacheBlocks); 0 turns the cache off.
    std::size_t cacheBlocks = 64;
    // Reads wait while this job is paused.
    JobControl control;
};

struct ScanSourceStats {
    // Bytes the source (the device or image) delivered, and its reads.
    std::uint64_t bytesRead = 0;
    std::uint64_t reads = 0;
    // Bytes the scan's readers asked for, and those served from the cache.
    std::uint64_t bytesRequested = 0;
    std::uint64_t bytesFromCache = 0;
    // Source bytes that could not be read: sectors whose read failed when
    // read on their own (as every reader of the engine retries a failed read).
    std::uint64_t unreadableBytes = 0;
};

// A read-only view of another source for one scan.
//
// Reads are served block by block: a block is read from the source as a
// whole (aligned to blockSize) and kept in a cache of the blocks used most
// recently, so the signature scan, the carves, the analyses and the
// validations that read the same part of the source one after the other
// read it from the device once. A block whose read fails (a bad sector in
// it) is never cached: the request is passed to the source as it was made,
// so the reader sees the source's own result and its sector-by-sector retry
// finds the bad sectors, which are recorded. Memory: cacheBlocks blocks,
// plus one block per reader loading one.
//
// Thread safety: reads may run concurrently (the cache has a lock, held only
// to look up and insert). open() and close() must not overlap other calls.
// The source must stay open and outlive this object.
class ScanSource final : public storage::IStorageSource {
public:
    static constexpr std::size_t kMaxRememberedBadBlocks = 65536;

    ScanSource(storage::IStorageSource& inner, ScanSourceOptions options);

    // Fails with InvalidInput when the source is not open or the options do
    // not suit it.
    [[nodiscard]] Status open() override;
    void close() noexcept override;
    [[nodiscard]] bool isOpen() const noexcept override;
    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] std::uint32_t sectorSize() const noexcept override;
    [[nodiscard]] storage::SourceInfo getInfo() const override;

    [[nodiscard]] ScanSourceStats stats() const;
    // The unreadable sectors found so far, merged.
    [[nodiscard]] std::vector<storage::BadRegion> unreadableRegions() const;
    // The unreadable sectors found since the last call (not merged).
    [[nodiscard]] std::vector<storage::BadRegion> takeNewUnreadable();
    // Reads under way now (a read waiting while the scan is paused is not).
    [[nodiscard]] std::uint32_t activeReads() const noexcept { return activeReads_.load(std::memory_order_acquire); }

protected:
    [[nodiscard]] storage::ReadResult readValidated(std::uint64_t offset, std::span<std::byte> buffer) override;

private:
    using Block = std::shared_ptr<const std::vector<std::byte>>;

    // A read passed to the source as it is.
    [[nodiscard]] storage::ReadResult readDirect(std::uint64_t offset, std::span<std::byte> buffer);
    // The block at `index`, cached (`cached` set) or read now; nullptr when it
    // cannot be read whole.
    [[nodiscard]] Block block(std::uint64_t index, bool& cached);

    storage::IStorageSource& inner_;
    ScanSourceOptions options_;
    bool open_ = false;
    std::uint64_t size_ = 0;
    std::uint32_t sectorSize_ = 0;

    mutable std::mutex mutex_;
    // Most recently used first.
    std::list<std::uint64_t> order_;
    std::unordered_map<std::uint64_t, std::pair<Block, std::list<std::uint64_t>::iterator>> cache_;
    std::unordered_set<std::uint64_t> badBlocks_;
    storage::BadRegionMap unreadable_;
    std::vector<storage::BadRegion> fresh_;

    std::atomic<std::uint64_t> bytesRead_{0};
    std::atomic<std::uint64_t> reads_{0};
    std::atomic<std::uint64_t> bytesRequested_{0};
    std::atomic<std::uint64_t> bytesFromCache_{0};
    std::atomic<std::uint32_t> activeReads_{0};
};

}  // namespace recovery::scan
