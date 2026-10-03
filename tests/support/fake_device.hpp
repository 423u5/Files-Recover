#pragma once

#include "storage/device_io.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace recovery::test {

// Behaviour of a simulated device. Shared by the opener and every device it
// hands out, so a test can inspect or change it after opening.
struct FakeDeviceConfig {
    std::vector<std::byte> data;
    storage::DeviceGeometry geometry;
    storage::DeviceDescription description;
    std::optional<Error> geometryError;
    // Byte ranges [first, second) whose reads fail with failureCode.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> failingRanges;
    std::uint32_t failureCode = 23;  // ERROR_CRC
    // Alignment actually enforced by readAt; defaults to geometry.requiredAlignment.
    // Lets a test simulate a driver that under-reports its requirement.
    std::optional<std::uint32_t> enforcedAlignment;
    // Maximum bytes returned per readAt (simulates short reads).
    std::optional<std::size_t> maxBytesPerRead;
};

struct FakeDeviceStats {
    std::atomic<std::size_t> reads{0};
    // readAt calls that ignored the device's alignment requirement.
    std::atomic<std::size_t> alignmentViolations{0};
};

// In-memory IDeviceIo that enforces the configured alignment exactly like
// a raw Windows disk handle would (ERROR_INVALID_PARAMETER).
class MemoryDeviceIo final : public storage::IDeviceIo {
public:
    MemoryDeviceIo(std::shared_ptr<const FakeDeviceConfig> config, std::shared_ptr<FakeDeviceStats> stats);

    Result<storage::DeviceGeometry> queryGeometry() override;
    storage::DeviceDescription describe() override;
    storage::DeviceReadResult readAt(std::uint64_t offset, std::span<std::byte> buffer) override;

private:
    std::shared_ptr<const FakeDeviceConfig> config_;
    std::shared_ptr<FakeDeviceStats> stats_;
};

// Opener that never touches the operating system.
class MockDeviceOpener final : public storage::IDeviceOpener {
public:
    struct Call {
        std::filesystem::path path;
        storage::DeviceKind kind;
    };

    explicit MockDeviceOpener(std::shared_ptr<const FakeDeviceConfig> config);

    Result<std::unique_ptr<storage::IDeviceIo>> openReadOnly(const std::filesystem::path& path,
                                                             storage::DeviceKind kind) override;

    void failOpenWith(Error error) { openError_ = std::move(error); }
    [[nodiscard]] std::vector<Call> calls() const;
    [[nodiscard]] const std::shared_ptr<FakeDeviceStats>& stats() const noexcept { return stats_; }

private:
    std::shared_ptr<const FakeDeviceConfig> config_;
    std::shared_ptr<FakeDeviceStats> stats_ = std::make_shared<FakeDeviceStats>();
    std::optional<Error> openError_;
    mutable std::mutex mutex_;
    std::vector<Call> calls_;
};

// Geometry of a healthy disk holding `size` bytes.
[[nodiscard]] storage::DeviceGeometry diskGeometry(std::uint64_t size, std::uint32_t sectorSize = 512);

}  // namespace recovery::test
