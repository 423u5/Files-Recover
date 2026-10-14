#pragma once

// Shared by the fuzz targets (P20): a source over the fuzzer's input, the
// registries of every carving format and media decoder, and the input as
// bytes. Every target treats its input as the untrusted data the engine reads
// from a disk: a volume, a file's content, a journal record.

#include "carving/format_registry.hpp"
#include "recovery/result.hpp"
#include "storage/storage_source.hpp"
#include "validation/media_validator.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace recovery::fuzz {

// The input as a read-only source: its bytes, padded with zeros to a whole
// number of sectors (a source's size is a multiple of its sector size).
class FuzzSource final : public storage::IStorageSource {
public:
    explicit FuzzSource(std::span<const std::byte> data, std::uint32_t sectorSize = 512);

    Status open() override;
    void close() noexcept override;
    bool isOpen() const noexcept override;
    std::uint64_t size() const noexcept override;
    std::uint32_t sectorSize() const noexcept override;
    storage::SourceInfo getInfo() const override;

protected:
    storage::ReadResult readValidated(std::uint64_t offset, std::span<std::byte> buffer) override;

private:
    std::vector<std::byte> data_;
    std::uint32_t sectorSize_;
    bool open_ = false;
};

// Every image, audio and video format, registered once.
[[nodiscard]] const carving::FormatRegistry& allFormats();
// Every media decoder, registered once.
[[nodiscard]] const validation::MediaValidatorRegistry& allMedia();

[[nodiscard]] inline std::span<const std::byte> bytesOf(const std::uint8_t* data, std::size_t size) noexcept {
    return std::as_bytes(std::span<const std::uint8_t>(data, size));
}

// Stops the fuzzer when a call that must succeed on any input fails (a
// harness error, never a finding about the input).
void require(bool condition, const char* what);

}  // namespace recovery::fuzz
