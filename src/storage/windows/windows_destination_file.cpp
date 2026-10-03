// Win32 implementation of DestinationFile.
//
// Windows assumptions:
//  * CREATE_NEW provides the atomic "never overwrite" guarantee.
//  * FlushFileBuffers makes written data durable before metadata that refers
//    to it is committed.
//  * MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) replaces a
//    file atomically on NTFS/ReFS; on FAT/exFAT destinations the replace is
//    not guaranteed to be atomic across power loss.

#include "storage/destination_file.hpp"

#include "recovery/checked_math.hpp"
#include "recovery/text.hpp"
#include "storage/storage_source.hpp"
#include "win32.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace recovery::storage {

namespace {

using windows::UniqueHandle;

constexpr std::size_t kMaxWriteChunk = 16 * kMiB;

HANDLE asHandle(void* handle) noexcept {
    return static_cast<HANDLE>(handle);
}

Error destinationError(std::string message, DWORD systemError = 0) {
    return makeError(ErrorCode::DestinationError, std::move(message), systemError);
}

}  // namespace

DestinationFile::~DestinationFile() {
    close();
}

DestinationFile::DestinationFile(DestinationFile&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

DestinationFile& DestinationFile::operator=(DestinationFile&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

Result<DestinationFile> DestinationFile::open(const std::filesystem::path& path, OpenMode mode) {
    if (path.empty()) {
        return makeError(ErrorCode::InvalidInput, "destination path is empty");
    }
    if (isDeviceNamespacePath(path)) {
        return destinationError("refusing to write to device path '" + toUtf8(path) + "'");
    }

    const DWORD disposition = mode == OpenMode::CreateNew ? CREATE_NEW : OPEN_EXISTING;
    UniqueHandle handle(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                      disposition, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            return destinationError("'" + toUtf8(path) + "' already exists; refusing to overwrite", error);
        }
        return destinationError("cannot open destination '" + toUtf8(path) + "'", error);
    }
    if (::GetFileType(handle.get()) != FILE_TYPE_DISK) {
        return destinationError("destination '" + toUtf8(path) + "' is not a regular file");
    }

    DestinationFile file;
    file.handle_ = handle.release();
    return file;
}

Status DestinationFile::writeAt(std::uint64_t offset, std::span<const std::byte> data) {
    if (!isOpen()) {
        return makeError(ErrorCode::InvalidInput, "destination file is not open");
    }
    const std::optional<std::uint64_t> end = checkedAdd<std::uint64_t>(offset, data.size());
    if (!end.has_value() || *end > kMaxAddressableOffset) {
        return makeError(ErrorCode::InvalidInput, "destination write range overflows");
    }

    std::size_t written = 0;
    while (written < data.size()) {
        const auto chunk = static_cast<DWORD>(std::min(data.size() - written, kMaxWriteChunk));
        const std::uint64_t position = offset + written;
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFULL);
        overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD transferred = 0;
        if (!::WriteFile(asHandle(handle_), data.data() + written, chunk, &transferred, &overlapped)) {
            const DWORD error = ::GetLastError();
            return destinationError("write to destination failed at offset " + std::to_string(position), error);
        }
        if (transferred != chunk) {
            return destinationError("short write to destination at offset " + std::to_string(position));
        }
        written += chunk;
    }
    return success();
}

Status DestinationFile::truncate(std::uint64_t newSize) {
    if (!isOpen()) {
        return makeError(ErrorCode::InvalidInput, "destination file is not open");
    }
    if (newSize > kMaxAddressableOffset) {
        return makeError(ErrorCode::InvalidInput, "destination size out of range");
    }
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(newSize);
    if (!::SetFileInformationByHandle(asHandle(handle_), FileEndOfFileInfo, &info, sizeof(info))) {
        const DWORD error = ::GetLastError();
        return destinationError("cannot set destination file size", error);
    }
    return success();
}

Status DestinationFile::flush() {
    if (!isOpen()) {
        return makeError(ErrorCode::InvalidInput, "destination file is not open");
    }
    if (!::FlushFileBuffers(asHandle(handle_))) {
        const DWORD error = ::GetLastError();
        return destinationError("flushing destination failed", error);
    }
    return success();
}

Result<std::uint64_t> DestinationFile::size() const {
    if (!isOpen()) {
        return makeError(ErrorCode::InvalidInput, "destination file is not open");
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(asHandle(handle_), &size) || size.QuadPart < 0) {
        const DWORD error = ::GetLastError();
        return destinationError("cannot query destination size", error);
    }
    return static_cast<std::uint64_t>(size.QuadPart);
}

void DestinationFile::close() noexcept {
    if (handle_ != nullptr) {
        ::CloseHandle(asHandle(handle_));
        handle_ = nullptr;
    }
}

Status replaceFile(const std::filesystem::path& replacement, const std::filesystem::path& target) {
    if (isDeviceNamespacePath(replacement) || isDeviceNamespacePath(target)) {
        return destinationError("refusing to rename device paths");
    }
    if (!::MoveFileExW(replacement.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = ::GetLastError();
        return destinationError("cannot replace '" + toUtf8(target) + "'", error);
    }
    return success();
}

}  // namespace recovery::storage
