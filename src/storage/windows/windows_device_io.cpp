// Win32 implementation of IDeviceIo / IDeviceOpener.
//
// Windows assumptions (see also docs/architecture/storage.md):
//  * Every handle is opened with GENERIC_READ only. No write, lock, dismount
//    or FSCTL/IOCTL that modifies state is ever issued.
//  * Reads use ReadFile with an explicit OVERLAPPED offset on a synchronous
//    handle, so no shared file position is used and concurrent reads are safe.
//  * Physical drives: size from IOCTL_DISK_GET_LENGTH_INFO, logical sector
//    size from IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, physical sector size from
//    StorageAccessAlignmentProperty (optional; older drivers do not report it).

#include "storage/device_io.hpp"

#include "recovery/text.hpp"
#include "win32.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace recovery::storage {

namespace {

using windows::UniqueHandle;

std::string trimmed(std::string text) {
    const auto notSpace = [](char c) { return c != ' ' && c != '\0'; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

// Reads a NUL-terminated ASCII string at `offset` within the first `size`
// bytes of `data`, never reading past `size`.
std::string boundedString(const std::byte* data, std::size_t size, DWORD offset) {
    if (offset == 0 || offset >= size) {
        return {};
    }
    std::string text;
    for (std::size_t i = offset; i < size && text.size() < 256; ++i) {
        const auto c = static_cast<char>(data[i]);
        if (c == '\0') {
            break;
        }
        text += (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F) ? c : '?';
    }
    return trimmed(std::move(text));
}

class WindowsDeviceIo final : public IDeviceIo {
public:
    WindowsDeviceIo(UniqueHandle handle, DeviceKind kind) noexcept : handle_(std::move(handle)), kind_(kind) {}

    Result<DeviceGeometry> queryGeometry() override {
        return kind_ == DeviceKind::RegularFile ? fileGeometry() : diskGeometry();
    }

    DeviceDescription describe() override {
        DeviceDescription description;
        if (kind_ != DeviceKind::PhysicalDisk) {
            return description;
        }
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        // Aligned storage for STORAGE_DEVICE_DESCRIPTOR plus its trailing strings.
        std::vector<std::uint64_t> storage(512);
        const DWORD capacity = static_cast<DWORD>(storage.size() * sizeof(std::uint64_t));
        DWORD returned = 0;
        if (!::DeviceIoControl(handle_.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), storage.data(),
                               capacity, &returned, nullptr) ||
            returned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            return description;
        }
        const auto* bytes = reinterpret_cast<const std::byte*>(storage.data());
        STORAGE_DEVICE_DESCRIPTOR header{};
        std::memcpy(&header, bytes, sizeof(header));
        const std::size_t valid = std::min<std::size_t>({returned, header.Size, capacity});
        description.vendor = boundedString(bytes, valid, header.VendorIdOffset);
        description.product = boundedString(bytes, valid, header.ProductIdOffset);
        description.removable = header.RemovableMedia != FALSE;
        return description;
    }

    DeviceReadResult readAt(std::uint64_t offset, std::span<std::byte> buffer) override {
        DeviceReadResult result;
        if (buffer.size() > std::numeric_limits<DWORD>::max() ||
            offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
            result.systemErrorCode = ERROR_INVALID_PARAMETER;
            return result;
        }
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD bytesRead = 0;
        const BOOL ok = ::ReadFile(handle_.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead,
                                   &overlapped);
        result.bytesRead = bytesRead;
        if (!ok) {
            const DWORD error = ::GetLastError();
            // End of file is a short read, not a failure.
            result.success = error == ERROR_HANDLE_EOF;
            result.systemErrorCode = result.success ? 0 : error;
            return result;
        }
        result.success = true;
        return result;
    }

private:
    Result<DeviceGeometry> fileGeometry() const {
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(handle_.get(), &size)) {
            const DWORD error = ::GetLastError();
            return makeError(ErrorCode::IoError, "cannot query image file size", error);
        }
        if (size.QuadPart < 0) {
            return makeError(ErrorCode::IoError, "image file reported a negative size");
        }
        DeviceGeometry geometry;
        geometry.sizeBytes = static_cast<std::uint64_t>(size.QuadPart);
        geometry.requiredAlignment = 1;
        return geometry;
    }

    Result<DeviceGeometry> diskGeometry() const {
        DISK_GEOMETRY_EX diskGeometry{};
        DWORD returned = 0;
        if (!::DeviceIoControl(handle_.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &diskGeometry,
                               sizeof(diskGeometry), &returned, nullptr)) {
            const DWORD error = ::GetLastError();
            return makeError(ErrorCode::IoError, "IOCTL_DISK_GET_DRIVE_GEOMETRY_EX failed", error);
        }
        if (returned < offsetof(DISK_GEOMETRY_EX, Data)) {
            return makeError(ErrorCode::IoError, "IOCTL_DISK_GET_DRIVE_GEOMETRY_EX returned a truncated structure");
        }

        GET_LENGTH_INFORMATION length{};
        if (!::DeviceIoControl(handle_.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length, sizeof(length),
                               &returned, nullptr) ||
            returned < sizeof(length)) {
            const DWORD error = ::GetLastError();
            return makeError(ErrorCode::IoError, "IOCTL_DISK_GET_LENGTH_INFO failed", error);
        }
        if (length.Length.QuadPart <= 0) {
            return makeError(ErrorCode::IoError, "disk reported a non-positive length");
        }

        DeviceGeometry geometry;
        geometry.sizeBytes = static_cast<std::uint64_t>(length.Length.QuadPart);
        geometry.logicalSectorSize = diskGeometry.Geometry.BytesPerSector;
        geometry.requiredAlignment = diskGeometry.Geometry.BytesPerSector;

        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageAccessAlignmentProperty;
        query.QueryType = PropertyStandardQuery;
        STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment{};
        if (::DeviceIoControl(handle_.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &alignment,
                              sizeof(alignment), &returned, nullptr) &&
            returned >= sizeof(alignment)) {
            geometry.physicalSectorSize = alignment.BytesPerPhysicalSector;
        }
        return geometry;
    }

    UniqueHandle handle_;
    DeviceKind kind_;
};

class WindowsDeviceOpener final : public IDeviceOpener {
public:
    Result<std::unique_ptr<IDeviceIo>> openReadOnly(const std::filesystem::path& path, DeviceKind kind) override {
        if (path.empty()) {
            return makeError(ErrorCode::InvalidInput, "empty device path");
        }
        // Mounted volumes on a physical disk hold their own handles, so the
        // disk can only be opened when sharing write access with them. Image
        // files are opened exclusively for reading so they cannot change
        // underneath the engine.
        const DWORD share =
            kind == DeviceKind::PhysicalDisk ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : FILE_SHARE_READ;
        UniqueHandle handle(::CreateFileW(path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!handle.valid()) {
            const DWORD error = ::GetLastError();
            return makeError(ErrorCode::IoError, "cannot open '" + toUtf8(path) + "' for reading", error);
        }

        if (kind == DeviceKind::RegularFile) {
            if (::GetFileType(handle.get()) != FILE_TYPE_DISK) {
                return makeError(ErrorCode::InvalidInput, "'" + toUtf8(path) + "' is not a regular file");
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if (!::GetFileInformationByHandle(handle.get(), &info)) {
                const DWORD error = ::GetLastError();
                return makeError(ErrorCode::IoError, "cannot query '" + toUtf8(path) + "'", error);
            }
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                return makeError(ErrorCode::InvalidInput, "'" + toUtf8(path) + "' is a directory");
            }
        }
        return std::unique_ptr<IDeviceIo>(std::make_unique<WindowsDeviceIo>(std::move(handle), kind));
    }
};

}  // namespace

std::shared_ptr<IDeviceOpener> makePlatformDeviceOpener() {
    return std::make_shared<WindowsDeviceOpener>();
}

}  // namespace recovery::storage
