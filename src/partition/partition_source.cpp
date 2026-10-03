#include "partition/partition_source.hpp"

#include "recovery/checked_math.hpp"

namespace recovery::partition {

PartitionSource::PartitionSource(storage::IStorageSource& parent, std::uint64_t offset, std::uint64_t size) noexcept
    : parent_(parent), offset_(offset), size_(size) {}

PartitionSource::PartitionSource(storage::IStorageSource& parent, const Partition& partition) noexcept
    : PartitionSource(parent, partition.offset, partition.size) {}

Status PartitionSource::open() {
    if (!parent_.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "parent source is not open");
    }
    if (!rangeWithin(offset_, size_, parent_.size())) {
        return makeError(ErrorCode::InvalidInput, "partition range [" + std::to_string(offset_) + ", +" +
                                                      std::to_string(size_) + ") lies outside the source");
    }
    open_ = true;
    return success();
}

void PartitionSource::close() noexcept {
    open_ = false;
}

bool PartitionSource::isOpen() const noexcept {
    return open_ && parent_.isOpen();
}

std::uint64_t PartitionSource::size() const noexcept {
    return isOpen() ? size_ : 0;
}

std::uint32_t PartitionSource::sectorSize() const noexcept {
    return isOpen() ? parent_.sectorSize() : 0;
}

storage::SourceInfo PartitionSource::getInfo() const {
    storage::SourceInfo info = parent_.getInfo();
    info.sizeBytes = size();
    return info;
}

storage::ReadResult PartitionSource::readValidated(std::uint64_t offset, std::span<std::byte> buffer) {
    // offset + buffer.size() <= size_ (validated by IStorageSource::read) and
    // offset_ + size_ <= parent size (validated by open), so this cannot wrap.
    // The parent validates again, which also covers a parent reopened with a
    // different size.
    return parent_.read(ByteOffset{offset_ + offset}, buffer);
}

}  // namespace recovery::partition
