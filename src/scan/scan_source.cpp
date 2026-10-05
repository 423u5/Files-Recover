#include "scan/scan_source.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace recovery::scan {

ScanSource::ScanSource(storage::IStorageSource& inner, ScanSourceOptions options)
    : inner_(inner), options_(std::move(options)) {}

Status ScanSource::open() {
    if (!inner_.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "the scan's source is not open");
    }
    const std::uint32_t sectorSize = inner_.sectorSize();
    if (!storage::isValidSectorSize(sectorSize)) {
        return makeError(ErrorCode::InvalidInput, "the scan's source has an invalid sector size");
    }
    if (options_.blockSize == 0 || options_.blockSize % sectorSize != 0 ||
        options_.blockSize > storage::kMaxReadSize) {
        return makeError(ErrorCode::InvalidInput, "the cache block size must be a multiple of the sector size (" +
                                                      std::to_string(sectorSize) + ") and at most " +
                                                      std::to_string(storage::kMaxReadSize));
    }
    if (options_.cacheBlocks > ScanSourceOptions::kMaxCacheBlocks) {
        return makeError(ErrorCode::InvalidInput,
                         "the cache holds at most " + std::to_string(ScanSourceOptions::kMaxCacheBlocks) + " blocks");
    }
    sectorSize_ = sectorSize;
    size_ = inner_.size();
    open_ = true;
    return success();
}

void ScanSource::close() noexcept {
    open_ = false;
    const std::lock_guard lock(mutex_);
    cache_.clear();
    order_.clear();
}

bool ScanSource::isOpen() const noexcept {
    return open_ && inner_.isOpen();
}

std::uint64_t ScanSource::size() const noexcept {
    return open_ ? size_ : 0;
}

std::uint32_t ScanSource::sectorSize() const noexcept {
    return open_ ? sectorSize_ : 0;
}

storage::SourceInfo ScanSource::getInfo() const {
    return inner_.getInfo();
}

ScanSourceStats ScanSource::stats() const {
    ScanSourceStats stats;
    stats.bytesRead = bytesRead_.load(std::memory_order_relaxed);
    stats.reads = reads_.load(std::memory_order_relaxed);
    stats.bytesRequested = bytesRequested_.load(std::memory_order_relaxed);
    stats.bytesFromCache = bytesFromCache_.load(std::memory_order_relaxed);
    const std::lock_guard lock(mutex_);
    stats.unreadableBytes = unreadable_.totalBytes();
    return stats;
}

std::vector<storage::BadRegion> ScanSource::unreadableRegions() const {
    const std::lock_guard lock(mutex_);
    return unreadable_.regions();
}

storage::ReadResult ScanSource::readDirect(std::uint64_t offset, std::span<std::byte> buffer) {
    const storage::ReadResult result = inner_.read(ByteOffset{offset}, buffer);
    bytesRead_.fetch_add(result.bytesRead, std::memory_order_relaxed);
    reads_.fetch_add(1, std::memory_order_relaxed);
    const bool ioFailure =
        result.status == storage::ReadStatus::IoError || result.status == storage::ReadStatus::PartialRead;
    if (ioFailure && buffer.size() <= sectorSize_) {
        // A sector read on its own failed: these bytes cannot be read.
        const storage::BadRegion region{offset, buffer.size(), result.systemErrorCode};
        const std::lock_guard lock(mutex_);
        if (!unreadable_.intersects(region.offset, region.length)) {
            fresh_.push_back(region);
        }
        (void)unreadable_.add(region);
    }
    return result;
}

std::vector<storage::BadRegion> ScanSource::takeNewUnreadable() {
    const std::lock_guard lock(mutex_);
    return std::exchange(fresh_, {});
}

ScanSource::Block ScanSource::block(std::uint64_t index, bool& cached) {
    cached = false;
    {
        const std::lock_guard lock(mutex_);
        if (badBlocks_.contains(index)) {
            return nullptr;
        }
        if (const auto found = cache_.find(index); found != cache_.end()) {
            order_.splice(order_.begin(), order_, found->second.second);
            cached = true;
            return found->second.first;
        }
    }
    // Read outside the lock, so readers of other blocks do not wait for this one.
    const std::uint64_t start = index * options_.blockSize;
    const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(options_.blockSize, size_ - start));
    auto data = std::make_shared<std::vector<std::byte>>(length);
    const storage::ReadResult result = inner_.read(ByteOffset{start}, *data);
    bytesRead_.fetch_add(result.bytesRead, std::memory_order_relaxed);
    reads_.fetch_add(1, std::memory_order_relaxed);
    const std::lock_guard lock(mutex_);
    if (!result.ok()) {
        if (badBlocks_.size() < kMaxRememberedBadBlocks) {
            badBlocks_.insert(index);
        }
        return nullptr;
    }
    if (const auto found = cache_.find(index); found != cache_.end()) {
        // Another reader loaded it meanwhile.
        order_.splice(order_.begin(), order_, found->second.second);
        return found->second.first;
    }
    order_.push_front(index);
    Block loaded = std::move(data);
    cache_.emplace(index, std::make_pair(loaded, order_.begin()));
    while (cache_.size() > options_.cacheBlocks) {
        cache_.erase(order_.back());
        order_.pop_back();
    }
    return loaded;
}

storage::ReadResult ScanSource::readValidated(std::uint64_t offset, std::span<std::byte> buffer) {
    // A paused scan stops here; a cancelled one goes on to its next check.
    (void)options_.control.waitWhilePaused();
    struct Active {
        std::atomic<std::uint32_t>& count;
        explicit Active(std::atomic<std::uint32_t>& reads) : count(reads) {
            count.fetch_add(1, std::memory_order_acq_rel);
        }
        ~Active() { count.fetch_sub(1, std::memory_order_acq_rel); }
        Active(const Active&) = delete;
        Active& operator=(const Active&) = delete;
        Active(Active&&) = delete;
        Active& operator=(Active&&) = delete;
    } active(activeReads_);
    bytesRequested_.fetch_add(buffer.size(), std::memory_order_relaxed);
    if (options_.cacheBlocks == 0) {
        return readDirect(offset, buffer);
    }
    const std::uint64_t end = offset + buffer.size();
    std::uint64_t position = offset;
    while (position < end) {
        const std::uint64_t index = position / options_.blockSize;
        bool cached = false;
        const Block data = block(index, cached);
        if (data == nullptr) {
            // The source's own answer to the request as it was made.
            return readDirect(offset, buffer);
        }
        const std::uint64_t within = position - index * options_.blockSize;
        if (within >= data->size()) {
            return readDirect(offset, buffer);
        }
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(end - position, data->size() - within));
        std::memcpy(buffer.data() + (position - offset), data->data() + within, take);
        if (cached) {
            bytesFromCache_.fetch_add(take, std::memory_order_relaxed);
        }
        position += take;
    }
    storage::ReadResult result;
    result.status = storage::ReadStatus::Success;
    result.requestedBytes = buffer.size();
    result.bytesRead = buffer.size();
    result.offset = offset;
    return result;
}

}  // namespace recovery::scan
