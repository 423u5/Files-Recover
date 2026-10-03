#include "support/virtual_source.hpp"

#include <algorithm>
#include <cstring>

namespace recovery::test {

namespace {

std::uint64_t splitMix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

}  // namespace

VirtualSource::VirtualSource(std::uint64_t size, std::uint32_t sectorSize) : size_(size), sectorSize_(sectorSize) {}

Status VirtualSource::open() {
    open_ = true;
    return success();
}

void VirtualSource::close() noexcept {
    open_ = false;
}

bool VirtualSource::isOpen() const noexcept {
    return open_;
}

std::uint64_t VirtualSource::size() const noexcept {
    return open_ ? size_ : 0;
}

std::uint32_t VirtualSource::sectorSize() const noexcept {
    return open_ ? sectorSize_ : 0;
}

storage::SourceInfo VirtualSource::getInfo() const {
    storage::SourceInfo info;
    info.type = storage::SourceType::Synthetic;
    info.path = "virtual://test";
    info.sizeBytes = size();
    info.logicalSectorSize = sectorSize();
    info.physicalSectorSize = sectorSize();
    return info;
}

void VirtualSource::plant(std::uint64_t offset, std::vector<std::byte> bytes) {
    const std::scoped_lock lock(mutex_);
    longestPlant_ = std::max(longestPlant_, bytes.size());
    plants_[offset] = std::move(bytes);
}

void VirtualSource::setNoise(std::uint64_t seed) {
    const std::scoped_lock lock(mutex_);
    noiseSeed_ = seed;
}

void VirtualSource::addBadSector(std::uint64_t sector, std::uint32_t errorCode) {
    const std::scoped_lock lock(mutex_);
    badSectors_[sector] = errorCode;
}

void VirtualSource::addFatalSector(std::uint64_t sector) {
    const std::scoped_lock lock(mutex_);
    fatalSectors_.insert(sector);
}

void VirtualSource::setReadHook(std::function<void(std::uint64_t, std::size_t)> hook) {
    const std::scoped_lock lock(mutex_);
    hook_ = std::move(hook);
}

std::vector<std::byte> VirtualSource::contentAt(std::uint64_t offset, std::size_t length) const {
    std::vector<std::byte> bytes(length);
    const std::scoped_lock lock(mutex_);
    fill(offset, bytes);
    return bytes;
}

VirtualSource::Stats VirtualSource::stats() const {
    const std::scoped_lock lock(mutex_);
    return stats_;
}

void VirtualSource::resetStats() {
    const std::scoped_lock lock(mutex_);
    stats_ = Stats{};
    lastEnd_.reset();
}

// Called with the lock held.
void VirtualSource::fill(std::uint64_t offset, std::span<std::byte> buffer) const {
    if (buffer.empty()) {
        return;
    }
    if (noiseSeed_.has_value()) {
        for (std::size_t i = 0; i < buffer.size();) {
            const std::uint64_t position = offset + i;
            const std::uint64_t word = splitMix64(*noiseSeed_ ^ (position / 8));
            for (std::uint64_t byte = position % 8; byte < 8 && i < buffer.size(); ++byte, ++i) {
                buffer[i] = static_cast<std::byte>((word >> (8 * byte)) & 0xFF);
            }
        }
    } else {
        std::memset(buffer.data(), 0, buffer.size());
    }
    const std::uint64_t end = offset + buffer.size();
    auto plant = plants_.lower_bound(offset - std::min<std::uint64_t>(offset, longestPlant_));
    for (; plant != plants_.end() && plant->first < end; ++plant) {
        const std::uint64_t plantEnd = plant->first + plant->second.size();
        const std::uint64_t from = std::max(plant->first, offset);
        const std::uint64_t to = std::min(plantEnd, end);
        if (from < to) {
            std::memcpy(buffer.data() + (from - offset), plant->second.data() + (from - plant->first), to - from);
        }
    }
}

storage::ReadResult VirtualSource::readValidated(std::uint64_t offset, std::span<std::byte> buffer) {
    std::function<void(std::uint64_t, std::size_t)> hook;
    {
        const std::scoped_lock lock(mutex_);
        hook = hook_;
    }
    if (hook) {
        hook(offset, buffer.size());
    }

    const std::scoped_lock lock(mutex_);
    ++stats_.reads;
    stats_.maxReadLength = std::max(stats_.maxReadLength, buffer.size());
    stats_.bytes += buffer.size();
    if (lastEnd_.has_value() && offset < *lastEnd_) {
        ++stats_.backwardReads;
    }
    lastEnd_ = offset + buffer.size();
    stats_.highestEnd = std::max(stats_.highestEnd, *lastEnd_);

    storage::ReadResult result;
    const std::uint64_t firstSector = offset / sectorSize_;
    const std::uint64_t lastSector = (offset + buffer.size() - 1) / sectorSize_;
    const auto fatal = fatalSectors_.lower_bound(firstSector);
    if (fatal != fatalSectors_.end() && *fatal <= lastSector) {
        result.status = storage::ReadStatus::InternalError;
        return result;
    }
    const auto bad = badSectors_.lower_bound(firstSector);
    if (bad != badSectors_.end() && bad->first <= lastSector) {
        const std::uint64_t sectorStart = bad->first * sectorSize_;
        const std::size_t good = sectorStart > offset ? static_cast<std::size_t>(sectorStart - offset) : 0;
        fill(offset, buffer.subspan(0, good));
        result.status = storage::ReadStatus::IoError;
        result.bytesRead = good;
        result.systemErrorCode = bad->second;
        return result;
    }
    fill(offset, buffer);
    result.status = storage::ReadStatus::Success;
    result.bytesRead = buffer.size();
    return result;
}

}  // namespace recovery::test
