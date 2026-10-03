#pragma once

#include "recovery/config.hpp"
#include "recovery/result.hpp"
#include "recovery/strong_types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace recovery::storage {

enum class SourceType : std::uint8_t {
    PhysicalDisk,
    DiskImage,
    // In-memory or simulated sources used by tests and tools; never user media.
    Synthetic,
};

[[nodiscard]] std::string_view toString(SourceType type) noexcept;
[[nodiscard]] std::optional<SourceType> parseSourceType(std::string_view text) noexcept;

struct SourceInfo {
    SourceType type = SourceType::Synthetic;
    std::filesystem::path path;
    std::uint64_t sizeBytes = 0;
    std::uint32_t logicalSectorSize = 0;
    std::uint32_t physicalSectorSize = 0;
    // Always true. Stated explicitly so reports and UIs can show it.
    bool readOnly = true;
    // Physical disks only.
    std::optional<std::uint32_t> diskNumber;
    std::string vendor;
    std::string product;
    std::optional<bool> removable;
};

enum class ReadStatus : std::uint8_t {
    Success,
    // Fewer bytes than requested were returned; the first `bytesRead` bytes are valid.
    PartialRead,
    // The requested range is not inside the source.
    OutOfRange,
    // Malformed request: oversized buffer, overflowing offset/sector arithmetic.
    InvalidArgument,
    NotOpen,
    // The device or operating system reported an error (see systemErrorCode).
    IoError,
    // An implementation invariant was violated (e.g. allocation failure).
    InternalError,
};

[[nodiscard]] std::string_view toString(ReadStatus status) noexcept;

struct ReadResult {
    ReadStatus status = ReadStatus::InternalError;
    std::size_t requestedBytes = 0;
    std::size_t bytesRead = 0;
    std::uint64_t offset = 0;
    // Win32 error code when status == IoError (and sometimes PartialRead); otherwise 0.
    std::uint32_t systemErrorCode = 0;

    [[nodiscard]] bool ok() const noexcept { return status == ReadStatus::Success; }
};

// Converts a failed read into an engine Error.
[[nodiscard]] Error toError(const ReadResult& result);

// Upper bound for a single read request. Larger transfers must be split by
// the caller; this bounds memory use and keeps a single failure small.
inline constexpr std::size_t kMaxReadSize = 64 * kMiB;

// Offsets are passed to Windows as signed 64-bit values (LARGE_INTEGER), so
// nothing at or beyond 2^63 is addressable.
inline constexpr std::uint64_t kMaxAddressableOffset =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

inline constexpr std::uint32_t kMinSectorSize = 512;
inline constexpr std::uint32_t kMaxSectorSize = 64 * 1024;

// True for powers of two within [kMinSectorSize, kMaxSectorSize].
[[nodiscard]] bool isValidSectorSize(std::uint64_t sectorSize) noexcept;

// True for Win32 device-namespace paths (\\.\X, \\?\Volume{...}, \\?\GLOBALROOT,
// \??\...), which address devices rather than regular files.
[[nodiscard]] bool isDeviceNamespacePath(const std::filesystem::path& path);

// Read-only view of a storage device or image.
//
// There is deliberately no write operation anywhere in this interface.
//
// Every read is validated here, in non-virtual functions, before an
// implementation is invoked, so all sources enforce identical bounds and
// overflow rules.
//
// Thread safety: open() and close() must not run concurrently with any other
// call. Once open, read functions may be called concurrently if the
// implementation documents it (all engine-provided sources do).
class IStorageSource {
public:
    virtual ~IStorageSource() = default;

    IStorageSource(const IStorageSource&) = delete;
    IStorageSource& operator=(const IStorageSource&) = delete;
    IStorageSource(IStorageSource&&) = delete;
    IStorageSource& operator=(IStorageSource&&) = delete;

    [[nodiscard]] virtual Status open() = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool isOpen() const noexcept = 0;

    // Size in bytes; 0 while closed.
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    // Logical sector size in bytes; 0 while closed.
    [[nodiscard]] virtual std::uint32_t sectorSize() const noexcept = 0;
    [[nodiscard]] virtual SourceInfo getInfo() const = 0;

    // Reads buffer.size() bytes at `offset`. The whole range must lie inside
    // the source; a range crossing the end is rejected (OutOfRange) rather
    // than silently shortened. A zero-length read at any offset <= size()
    // succeeds. The status must be checked: on PartialRead only the first
    // bytesRead bytes of `buffer` are valid.
    [[nodiscard]] ReadResult read(ByteOffset offset, std::span<std::byte> buffer);

    // Like read(), but anything short of a complete read is an error.
    [[nodiscard]] Status readExact(ByteOffset offset, std::span<std::byte> buffer);

    // Reads `count` whole logical sectors starting at `first` into the start
    // of `buffer`, which must hold at least count * sectorSize() bytes.
    [[nodiscard]] ReadResult readSectors(SectorNumber first, SectorCount count, std::span<std::byte> buffer);

protected:
    IStorageSource() = default;

    // Invoked only when the source is open, buffer is non-empty and at most
    // kMaxReadSize bytes, and [offset, offset + buffer.size()) lies within size().
    [[nodiscard]] virtual ReadResult readValidated(std::uint64_t offset, std::span<std::byte> buffer) = 0;
};

}  // namespace recovery::storage
