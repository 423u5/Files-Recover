#include "storage/device_backed_source.hpp"

#include "recovery/text.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>
#include <string>

namespace recovery::storage {

namespace {

// Largest single request handed to the device (fits a Win32 DWORD and is a
// multiple of every supported sector size).
constexpr std::size_t kMaxDeviceChunk = 16 * kMiB;
// Bounce buffer used for unaligned requests on devices that require alignment.
constexpr std::size_t kBounceChunk = 1 * kMiB;

static_assert(kMaxDeviceChunk % kMaxSectorSize == 0);
static_assert(kBounceChunk % kMaxSectorSize == 0);

struct AlignedDelete {
    std::align_val_t alignment;
    void operator()(std::byte* p) const noexcept { ::operator delete[](p, alignment); }
};
using AlignedBuffer = std::unique_ptr<std::byte[], AlignedDelete>;

AlignedBuffer allocateAligned(std::size_t size, std::size_t alignment) {
    const auto align = std::align_val_t{alignment};
    return AlignedBuffer(static_cast<std::byte*>(::operator new[](size, align)), AlignedDelete{align});
}

bool isAligned(std::uint64_t value, std::uint32_t alignment) noexcept {
    return value % alignment == 0;
}

}  // namespace

DeviceBackedSource::DeviceBackedSource(SourceType type, std::filesystem::path path, DeviceKind kind,
                                       std::shared_ptr<IDeviceOpener> opener)
    : type_(type), path_(std::move(path)), kind_(kind), opener_(std::move(opener)) {}

DeviceBackedSource::~DeviceBackedSource() = default;

Status DeviceBackedSource::open() {
    if (device_) {
        return success();
    }
    if (Status valid = validateConfiguration(); !valid.ok()) {
        return valid;
    }
    if (!opener_) {
        return makeError(ErrorCode::InternalError, "no device opener configured");
    }

    Result<std::unique_ptr<IDeviceIo>> opened = opener_->openReadOnly(path_, kind_);
    if (!opened.ok()) {
        return opened.error();
    }
    std::unique_ptr<IDeviceIo> device = std::move(opened).value();
    if (!device) {
        return makeError(ErrorCode::InternalError, "device opener returned no device");
    }

    const Result<DeviceGeometry> reported = device->queryGeometry();
    if (!reported.ok()) {
        return reported.error();
    }
    Result<DeviceGeometry> finalized = finalizeGeometry(reported.value());
    if (!finalized.ok()) {
        return finalized.error();
    }
    DeviceGeometry geometry = finalized.value();

    const std::string where = " for " + toUtf8(path_);
    if (!isValidSectorSize(geometry.logicalSectorSize)) {
        return makeError(ErrorCode::IoError,
                         "invalid logical sector size " + std::to_string(geometry.logicalSectorSize) + where);
    }
    if (geometry.physicalSectorSize == 0) {
        geometry.physicalSectorSize = geometry.logicalSectorSize;
    }
    if (!isValidSectorSize(geometry.physicalSectorSize)) {
        return makeError(ErrorCode::IoError,
                         "invalid physical sector size " + std::to_string(geometry.physicalSectorSize) + where);
    }
    if (geometry.requiredAlignment == 0 || geometry.requiredAlignment > kMaxSectorSize ||
        !std::has_single_bit(geometry.requiredAlignment)) {
        return makeError(ErrorCode::IoError,
                         "invalid I/O alignment " + std::to_string(geometry.requiredAlignment) + where);
    }
    if (geometry.sizeBytes > kMaxAddressableOffset) {
        return makeError(ErrorCode::IoError, "device size exceeds the addressable range" + where);
    }
    if (geometry.requiredAlignment > 1 && !isAligned(geometry.sizeBytes, geometry.requiredAlignment)) {
        return makeError(ErrorCode::IoError, "device size " + std::to_string(geometry.sizeBytes) +
                                                 " is not a multiple of its I/O alignment" + where);
    }

    description_ = device->describe();
    geometry_ = geometry;
    device_ = std::move(device);
    return success();
}

void DeviceBackedSource::close() noexcept {
    device_.reset();
    geometry_ = DeviceGeometry{};
    description_ = DeviceDescription{};
}

bool DeviceBackedSource::isOpen() const noexcept {
    return device_ != nullptr;
}

std::uint64_t DeviceBackedSource::size() const noexcept {
    return device_ ? geometry_.sizeBytes : 0;
}

std::uint32_t DeviceBackedSource::sectorSize() const noexcept {
    return device_ ? geometry_.logicalSectorSize : 0;
}

SourceInfo DeviceBackedSource::getInfo() const {
    SourceInfo info;
    info.type = type_;
    info.path = path_;
    info.sizeBytes = size();
    info.logicalSectorSize = sectorSize();
    info.physicalSectorSize = device_ ? geometry_.physicalSectorSize : 0;
    info.readOnly = true;
    info.vendor = description_.vendor;
    info.product = description_.product;
    info.removable = description_.removable;
    decorateInfo(info);
    return info;
}

void DeviceBackedSource::decorateInfo(SourceInfo&) const {}

ReadResult DeviceBackedSource::readValidated(std::uint64_t offset, std::span<std::byte> buffer) {
    const std::uint32_t alignment = geometry_.requiredAlignment;
    const bool aligned = alignment == 1 ||
                         (isAligned(offset, alignment) && isAligned(buffer.size(), alignment) &&
                          isAligned(reinterpret_cast<std::uintptr_t>(buffer.data()), alignment));
    try {
        return aligned ? readDirect(offset, buffer) : readViaBounceBuffer(offset, buffer);
    } catch (const std::bad_alloc&) {
        ReadResult result;
        result.status = ReadStatus::InternalError;
        return result;
    }
}

ReadResult DeviceBackedSource::readDirect(std::uint64_t offset, std::span<std::byte> buffer) {
    ReadResult result;
    std::size_t done = 0;
    while (done < buffer.size()) {
        const std::size_t chunk = std::min(buffer.size() - done, kMaxDeviceChunk);
        const DeviceReadResult r = device_->readAt(offset + done, buffer.subspan(done, chunk));
        if (r.bytesRead > chunk) {
            result.status = ReadStatus::InternalError;
            result.bytesRead = done;
            return result;
        }
        done += r.bytesRead;
        if (!r.success) {
            result.status = ReadStatus::IoError;
            result.bytesRead = done;
            result.systemErrorCode = r.systemErrorCode;
            return result;
        }
        if (r.bytesRead < chunk) {
            // Short read without an error (e.g. the file shrank). Stop rather
            // than loop: the caller decides what to do with the partial data.
            result.status = ReadStatus::PartialRead;
            result.bytesRead = done;
            return result;
        }
    }
    result.status = ReadStatus::Success;
    result.bytesRead = done;
    return result;
}

ReadResult DeviceBackedSource::readViaBounceBuffer(std::uint64_t offset, std::span<std::byte> buffer) {
    const std::uint64_t alignment = geometry_.requiredAlignment;
    // The request lies within size(), and size() is a multiple of the
    // alignment, so rounding the end up never leaves the device.
    const std::uint64_t end = offset + buffer.size();
    const std::uint64_t alignedEnd = (end + alignment - 1) / alignment * alignment;
    const std::uint64_t alignedStart = offset / alignment * alignment;
    const std::size_t bounceSize =
        static_cast<std::size_t>(std::min<std::uint64_t>(kBounceChunk, alignedEnd - alignedStart));
    const AlignedBuffer bounce = allocateAligned(bounceSize, static_cast<std::size_t>(alignment));

    ReadResult result;
    std::size_t copied = 0;
    while (copied < buffer.size()) {
        const std::uint64_t position = offset + copied;
        const std::uint64_t chunkStart = position / alignment * alignment;
        const std::uint64_t chunkEnd = std::min<std::uint64_t>(chunkStart + bounceSize, alignedEnd);
        const auto chunkSize = static_cast<std::size_t>(chunkEnd - chunkStart);
        const auto skip = static_cast<std::size_t>(position - chunkStart);

        const ReadResult chunk = readDirect(chunkStart, std::span<std::byte>(bounce.get(), chunkSize));

        // Copy whatever valid bytes cover the requested range, even on failure.
        if (chunk.bytesRead > skip) {
            const std::size_t available = std::min(chunk.bytesRead - skip, buffer.size() - copied);
            std::memcpy(buffer.data() + copied, bounce.get() + skip, available);
            copied += available;
        }
        // A failure confined to the alignment padding beyond the request
        // does not affect the caller's data.
        if (!chunk.ok() && copied < buffer.size()) {
            result.status = chunk.status;
            result.systemErrorCode = chunk.systemErrorCode;
            result.bytesRead = copied;
            return result;
        }
    }
    result.status = ReadStatus::Success;
    result.bytesRead = copied;
    return result;
}

}  // namespace recovery::storage
