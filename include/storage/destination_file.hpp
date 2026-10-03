#pragma once

#include "recovery/result.hpp"

#include <cstdint>
#include <filesystem>
#include <span>

namespace recovery::storage {

// A regular file on the recovery destination, the only thing the engine
// ever writes to.
//
// Safety properties:
//  * Device-namespace paths (\\.\PhysicalDrive0, \\?\Volume{...}) are
//    refused before any system call, and a handle that turns out not to be a
//    regular disk file (CON, NUL, pipes) is closed immediately.
//  * CreateNew never overwrites: it fails if the file already exists
//    (atomic check, no race window).
//  * Every write is checked for a complete transfer.
//
// Thread safety: none; one owner at a time.
class DestinationFile {
public:
    enum class OpenMode : std::uint8_t {
        CreateNew,
        OpenExisting,
    };

    DestinationFile() noexcept = default;
    ~DestinationFile();
    DestinationFile(const DestinationFile&) = delete;
    DestinationFile& operator=(const DestinationFile&) = delete;
    DestinationFile(DestinationFile&& other) noexcept;
    DestinationFile& operator=(DestinationFile&& other) noexcept;

    [[nodiscard]] static Result<DestinationFile> open(const std::filesystem::path& path, OpenMode mode);

    [[nodiscard]] bool isOpen() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] Status writeAt(std::uint64_t offset, std::span<const std::byte> data);
    [[nodiscard]] Status truncate(std::uint64_t newSize);
    // Flushes OS buffers to the device (FlushFileBuffers).
    [[nodiscard]] Status flush();
    [[nodiscard]] Result<std::uint64_t> size() const;
    void close() noexcept;

private:
    void* handle_ = nullptr;  // Win32 HANDLE
};

// Atomically replaces `target` with `replacement` (both destination files).
[[nodiscard]] Status replaceFile(const std::filesystem::path& replacement, const std::filesystem::path& target);

}  // namespace recovery::storage
