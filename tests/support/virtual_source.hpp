#pragma once

#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace recovery::test {

// A source of any size (up to storage::kMaxAddressableOffset) whose content
// is computed, not stored: zeros, or deterministic noise, with byte strings
// planted at chosen offsets. Used to scan images far larger than memory.
//
// It records how it is read (number, size and order of reads), so tests can
// check that a scan streams: sequential reads of bounded size.
class VirtualSource final : public storage::IStorageSource {
public:
    static constexpr std::uint32_t kErrorCrc = 23;  // ERROR_CRC

    struct Stats {
        std::size_t reads = 0;
        std::size_t maxReadLength = 0;
        std::uint64_t bytes = 0;
        // Reads that started before the end of the previous read.
        std::size_t backwardReads = 0;
        std::uint64_t highestEnd = 0;
    };

    explicit VirtualSource(std::uint64_t size, std::uint32_t sectorSize = 512);

    Status open() override;
    void close() noexcept override;
    bool isOpen() const noexcept override;
    std::uint64_t size() const noexcept override;
    std::uint32_t sectorSize() const noexcept override;
    storage::SourceInfo getInfo() const override;

    // Bytes at [offset, offset + bytes.size()) read as `bytes`. Where plants
    // overlap, the one at the higher offset wins; a plant at the same offset
    // replaces the earlier one.
    void plant(std::uint64_t offset, std::vector<std::byte> bytes);
    // Unplanted bytes become deterministic noise instead of zeros.
    void setNoise(std::uint64_t seed);
    // Every read touching `sector` fails with an I/O error (bytes before it are delivered).
    void addBadSector(std::uint64_t sector, std::uint32_t errorCode = kErrorCrc);
    // Every read touching `sector` fails with InternalError (a failure that is not a bad sector).
    void addFatalSector(std::uint64_t sector);
    // Called before every read, outside the source's lock (e.g. to request cancellation).
    void setReadHook(std::function<void(std::uint64_t offset, std::size_t length)> hook);

    // The content, as a read would return it, ignoring faults.
    [[nodiscard]] std::vector<std::byte> contentAt(std::uint64_t offset, std::size_t length) const;

    [[nodiscard]] Stats stats() const;
    void resetStats();

protected:
    storage::ReadResult readValidated(std::uint64_t offset, std::span<std::byte> buffer) override;

private:
    void fill(std::uint64_t offset, std::span<std::byte> buffer) const;

    std::uint64_t size_;
    std::uint32_t sectorSize_;
    bool open_ = false;
    std::optional<std::uint64_t> noiseSeed_;
    std::map<std::uint64_t, std::vector<std::byte>> plants_;
    std::size_t longestPlant_ = 0;
    std::map<std::uint64_t, std::uint32_t> badSectors_;
    std::set<std::uint64_t> fatalSectors_;
    std::function<void(std::uint64_t, std::size_t)> hook_;
    mutable std::mutex mutex_;
    Stats stats_;
    std::optional<std::uint64_t> lastEnd_;
};

}  // namespace recovery::test
