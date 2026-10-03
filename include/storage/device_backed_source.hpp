#pragma once

#include "storage/device_io.hpp"
#include "storage/storage_source.hpp"

#include <filesystem>
#include <memory>

namespace recovery::storage {

// Shared implementation of IStorageSource on top of an IDeviceIo.
//
// Handles geometry validation, splitting of large reads and, for devices that
// require aligned I/O (raw disks), transparently serves unaligned requests
// through an aligned bounce buffer. Because physical disks and images share
// this class, the same request produces the same result on both.
//
// Thread safety: after open() succeeds, read functions may be called
// concurrently (positional reads, per-call buffers). open()/close() must not
// race with anything.
class DeviceBackedSource : public IStorageSource {
public:
    ~DeviceBackedSource() override;

    [[nodiscard]] Status open() final;
    void close() noexcept final;
    [[nodiscard]] bool isOpen() const noexcept final;
    [[nodiscard]] std::uint64_t size() const noexcept final;
    [[nodiscard]] std::uint32_t sectorSize() const noexcept final;
    [[nodiscard]] SourceInfo getInfo() const final;

protected:
    DeviceBackedSource(SourceType type, std::filesystem::path path, DeviceKind kind,
                       std::shared_ptr<IDeviceOpener> opener);

    // Checks constructor arguments before anything is opened.
    [[nodiscard]] virtual Status validateConfiguration() const = 0;
    // Turns the geometry reported by the device into the source geometry
    // (e.g. applies the configured sector size of an image), rejecting
    // values that make no sense for this kind of source.
    [[nodiscard]] virtual Result<DeviceGeometry> finalizeGeometry(const DeviceGeometry& reported) const = 0;
    virtual void decorateInfo(SourceInfo& info) const;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] ReadResult readValidated(std::uint64_t offset, std::span<std::byte> buffer) final;

private:
    [[nodiscard]] ReadResult readDirect(std::uint64_t offset, std::span<std::byte> buffer);
    [[nodiscard]] ReadResult readViaBounceBuffer(std::uint64_t offset, std::span<std::byte> buffer);

    SourceType type_;
    std::filesystem::path path_;
    DeviceKind kind_;
    std::shared_ptr<IDeviceOpener> opener_;
    std::unique_ptr<IDeviceIo> device_;
    DeviceGeometry geometry_;
    DeviceDescription description_;
};

}  // namespace recovery::storage
