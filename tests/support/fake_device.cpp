#include "support/fake_device.hpp"

#include <algorithm>
#include <cstring>

namespace recovery::test {

namespace {
constexpr std::uint32_t kErrorInvalidParameter = 87;
}  // namespace

MemoryDeviceIo::MemoryDeviceIo(std::shared_ptr<const FakeDeviceConfig> config, std::shared_ptr<FakeDeviceStats> stats)
    : config_(std::move(config)), stats_(std::move(stats)) {}

Result<storage::DeviceGeometry> MemoryDeviceIo::queryGeometry() {
    if (config_->geometryError.has_value()) {
        return *config_->geometryError;
    }
    return config_->geometry;
}

storage::DeviceDescription MemoryDeviceIo::describe() {
    return config_->description;
}

storage::DeviceReadResult MemoryDeviceIo::readAt(std::uint64_t offset, std::span<std::byte> buffer) {
    ++stats_->reads;
    storage::DeviceReadResult result;

    const std::uint32_t alignment =
        std::max<std::uint32_t>(config_->enforcedAlignment.value_or(config_->geometry.requiredAlignment), 1);
    if (alignment > 1 && (offset % alignment != 0 || buffer.size() % alignment != 0 ||
                          reinterpret_cast<std::uintptr_t>(buffer.data()) % alignment != 0)) {
        ++stats_->alignmentViolations;
        result.systemErrorCode = kErrorInvalidParameter;
        return result;
    }

    const std::uint64_t dataSize = config_->data.size();
    if (offset >= dataSize) {
        result.success = true;  // end of file
        return result;
    }
    std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), dataSize - offset));
    if (config_->maxBytesPerRead.has_value()) {
        count = std::min(count, *config_->maxBytesPerRead);
    }

    const std::uint64_t end = offset + count;
    for (const auto& [failStart, failEnd] : config_->failingRanges) {
        if (failStart < end && offset < failEnd) {
            const std::size_t good = failStart > offset ? static_cast<std::size_t>(failStart - offset) : 0;
            std::memcpy(buffer.data(), config_->data.data() + offset, good);
            result.bytesRead = good;
            result.systemErrorCode = config_->failureCode;
            return result;
        }
    }

    std::memcpy(buffer.data(), config_->data.data() + offset, count);
    result.bytesRead = count;
    result.success = true;
    return result;
}

MockDeviceOpener::MockDeviceOpener(std::shared_ptr<const FakeDeviceConfig> config) : config_(std::move(config)) {}

Result<std::unique_ptr<storage::IDeviceIo>> MockDeviceOpener::openReadOnly(const std::filesystem::path& path,
                                                                          storage::DeviceKind kind) {
    {
        const std::scoped_lock lock(mutex_);
        calls_.push_back(Call{path, kind});
    }
    if (openError_.has_value()) {
        return *openError_;
    }
    return std::unique_ptr<storage::IDeviceIo>(std::make_unique<MemoryDeviceIo>(config_, stats_));
}

std::vector<MockDeviceOpener::Call> MockDeviceOpener::calls() const {
    const std::scoped_lock lock(mutex_);
    return calls_;
}

storage::DeviceGeometry diskGeometry(std::uint64_t size, std::uint32_t sectorSize) {
    storage::DeviceGeometry geometry;
    geometry.sizeBytes = size;
    geometry.logicalSectorSize = sectorSize;
    geometry.physicalSectorSize = sectorSize;
    geometry.requiredAlignment = sectorSize;
    return geometry;
}

}  // namespace recovery::test
