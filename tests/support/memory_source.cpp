#include "support/memory_source.hpp"

#include <algorithm>
#include <cstring>

namespace recovery::test {

MemoryStorageSource::MemoryStorageSource(std::vector<std::byte> data, std::uint32_t sectorSize)
    : data_(std::move(data)), sectorSize_(sectorSize) {}

Status MemoryStorageSource::open() {
    if (openError_.has_value()) {
        return *openError_;
    }
    open_ = true;
    return success();
}

void MemoryStorageSource::close() noexcept {
    open_ = false;
}

bool MemoryStorageSource::isOpen() const noexcept {
    return open_;
}

std::uint64_t MemoryStorageSource::size() const noexcept {
    return open_ ? data_.size() : 0;
}

std::uint32_t MemoryStorageSource::sectorSize() const noexcept {
    return open_ ? sectorSize_ : 0;
}

storage::SourceInfo MemoryStorageSource::getInfo() const {
    storage::SourceInfo info;
    info.type = storage::SourceType::Synthetic;
    info.path = "memory://test";
    info.sizeBytes = size();
    info.logicalSectorSize = sectorSize();
    info.physicalSectorSize = sectorSize();
    return info;
}

void MemoryStorageSource::addBadSector(std::uint64_t sector, std::uint32_t errorCode) {
    const std::scoped_lock lock(mutex_);
    badSectors_[sector] = errorCode;
}

void MemoryStorageSource::addTransientFailure(std::uint64_t sector, std::uint32_t failures, std::uint32_t errorCode) {
    const std::scoped_lock lock(mutex_);
    transient_[sector] = {failures, errorCode};
}

void MemoryStorageSource::addFatalSector(std::uint64_t sector) {
    const std::scoped_lock lock(mutex_);
    fatalSectors_.insert(sector);
}

void MemoryStorageSource::setShortReadLimit(std::optional<std::size_t> bytes) {
    const std::scoped_lock lock(mutex_);
    shortReadLimit_ = bytes;
}

void MemoryStorageSource::failOpenWith(Error error) {
    openError_ = std::move(error);
}

std::size_t MemoryStorageSource::readCount() const {
    const std::scoped_lock lock(mutex_);
    return readCount_;
}

storage::ReadResult MemoryStorageSource::readValidated(std::uint64_t offset, std::span<std::byte> buffer) {
    const std::scoped_lock lock(mutex_);
    ++readCount_;
    storage::ReadResult result;

    const std::uint64_t firstSector = offset / sectorSize_;
    const std::uint64_t lastSector = (offset + buffer.size() - 1) / sectorSize_;
    for (std::uint64_t sector = firstSector; sector <= lastSector; ++sector) {
        if (fatalSectors_.contains(sector)) {
            result.status = storage::ReadStatus::InternalError;
            return result;
        }
        std::optional<std::uint32_t> failure;
        if (const auto bad = badSectors_.find(sector); bad != badSectors_.end()) {
            failure = bad->second;
        } else if (const auto flaky = transient_.find(sector); flaky != transient_.end() && flaky->second.first > 0) {
            --flaky->second.first;
            failure = flaky->second.second;
        }
        if (failure.has_value()) {
            const std::uint64_t sectorStart = sector * sectorSize_;
            const std::size_t good = sectorStart > offset ? static_cast<std::size_t>(sectorStart - offset) : 0;
            std::memcpy(buffer.data(), data_.data() + offset, good);
            result.status = storage::ReadStatus::IoError;
            result.bytesRead = good;
            result.systemErrorCode = *failure;
            return result;
        }
    }

    std::size_t count = buffer.size();
    if (shortReadLimit_.has_value()) {
        count = std::min(count, *shortReadLimit_);
    }
    std::memcpy(buffer.data(), data_.data() + offset, count);
    result.status = storage::ReadStatus::Success;
    result.bytesRead = count;
    return result;
}

}  // namespace recovery::test
