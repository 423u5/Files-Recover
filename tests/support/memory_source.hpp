#pragma once

#include "storage/storage_source.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace recovery::test {

// In-memory IStorageSource with deterministic fault injection, used to
// exercise bad-sector handling without real failing hardware.
//
// Faults are per logical sector. A read fails at the first faulty sector it
// touches; bytes before that sector are delivered (like a real partial read).
class MemoryStorageSource final : public storage::IStorageSource {
public:
    static constexpr std::uint32_t kErrorCrc = 23;              // ERROR_CRC
    static constexpr std::uint32_t kErrorSectorNotFound = 27;   // ERROR_SECTOR_NOT_FOUND

    explicit MemoryStorageSource(std::vector<std::byte> data, std::uint32_t sectorSize = 512);

    Status open() override;
    void close() noexcept override;
    bool isOpen() const noexcept override;
    std::uint64_t size() const noexcept override;
    std::uint32_t sectorSize() const noexcept override;
    storage::SourceInfo getInfo() const override;

    // Every read touching `sector` fails.
    void addBadSector(std::uint64_t sector, std::uint32_t errorCode = kErrorCrc);
    // The next `failures` reads touching `sector` fail, later reads succeed.
    void addTransientFailure(std::uint64_t sector, std::uint32_t failures, std::uint32_t errorCode = kErrorCrc);
    // Every read touching `sector` fails with a non-I/O status (InternalError),
    // which callers must treat as fatal rather than as a bad sector.
    void addFatalSector(std::uint64_t sector);
    // Every read returns at most `bytes` bytes (reported as success).
    void setShortReadLimit(std::optional<std::size_t> bytes);
    // Makes open() fail.
    void failOpenWith(Error error);

    [[nodiscard]] std::size_t readCount() const;
    [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return data_; }

protected:
    storage::ReadResult readValidated(std::uint64_t offset, std::span<std::byte> buffer) override;

private:
    std::vector<std::byte> data_;
    std::uint32_t sectorSize_;
    bool open_ = false;
    std::optional<Error> openError_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::uint32_t> badSectors_;
    std::map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> transient_;  // sector -> (remaining, code)
    std::set<std::uint64_t> fatalSectors_;
    std::optional<std::size_t> shortReadLimit_;
    std::size_t readCount_ = 0;
};

}  // namespace recovery::test
