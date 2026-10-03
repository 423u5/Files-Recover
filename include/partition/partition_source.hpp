#pragma once

#include "partition/partition_table.hpp"
#include "storage/storage_source.hpp"

#include <cstdint>

namespace recovery::partition {

// Read-only view of a byte range of another source, e.g. one partition.
//
// Offsets are relative to the start of the range, and the shared
// IStorageSource validation confines every read to [0, size()), so code
// working on a partition cannot read outside it, whatever the metadata it
// parses claims.
//
// The parent is not owned and must outlive this object; it must be open
// before open() is called. Thread safety is that of the parent.
class PartitionSource final : public storage::IStorageSource {
public:
    PartitionSource(storage::IStorageSource& parent, std::uint64_t offset, std::uint64_t size) noexcept;
    PartitionSource(storage::IStorageSource& parent, const Partition& partition) noexcept;

    // Fails unless the parent is open and the range lies inside it.
    [[nodiscard]] Status open() override;
    void close() noexcept override;
    [[nodiscard]] bool isOpen() const noexcept override;
    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] std::uint32_t sectorSize() const noexcept override;
    [[nodiscard]] storage::SourceInfo getInfo() const override;

    [[nodiscard]] std::uint64_t offsetInParent() const noexcept { return offset_; }

protected:
    [[nodiscard]] storage::ReadResult readValidated(std::uint64_t offset, std::span<std::byte> buffer) override;

private:
    storage::IStorageSource& parent_;
    std::uint64_t offset_;
    std::uint64_t size_;
    bool open_ = false;
};

}  // namespace recovery::partition
