// Maps a path to the physical disks backing its volume.
//
// Windows assumptions:
//  * GetVolumePathNameW / GetVolumeNameForVolumeMountPointW resolve local
//    volumes (drive letters and mounted folders). Network shares, SUBST
//    drives and some virtual file systems cannot be resolved; the caller
//    treats that as "unsafe".
//  * The volume is opened with desired access 0 (attribute query only), which
//    cannot modify it and does not require administrator rights.

#include "storage/destination_guard.hpp"

#include "recovery/text.hpp"
#include "win32.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace recovery::storage {

namespace {

using windows::UniqueHandle;

constexpr DWORD kMaxExtents = 1024;

Result<std::vector<std::uint32_t>> resolveDisks(const std::filesystem::path& existingPath) {
    const std::wstring& path = existingPath.native();

    std::vector<wchar_t> volumePath(32768, L'\0');
    if (!::GetVolumePathNameW(path.c_str(), volumePath.data(), static_cast<DWORD>(volumePath.size()))) {
        const DWORD error = ::GetLastError();
        return makeError(ErrorCode::IoError, "cannot determine the volume of '" + toUtf8(existingPath) + "'", error);
    }

    wchar_t volumeName[MAX_PATH + 1] = {};
    if (!::GetVolumeNameForVolumeMountPointW(volumePath.data(), volumeName, MAX_PATH + 1)) {
        const DWORD error = ::GetLastError();
        return makeError(ErrorCode::IoError, "'" + toUtf8(existingPath) + "' is not on a local volume", error);
    }
    std::wstring volumeDevice(volumeName);
    if (!volumeDevice.empty() && volumeDevice.back() == L'\\') {
        volumeDevice.pop_back();  // "\\?\Volume{GUID}" opens the volume, not its root directory
    }

    UniqueHandle volume(::CreateFileW(volumeDevice.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, 0, nullptr));
    if (!volume.valid()) {
        const DWORD error = ::GetLastError();
        return makeError(ErrorCode::IoError, "cannot query the destination volume", error);
    }

    // Aligned storage large enough for the header and one extent; grown on ERROR_MORE_DATA.
    std::vector<std::uint64_t> storage((sizeof(VOLUME_DISK_EXTENTS) + 7) / 8 + 8);
    DWORD returned = 0;
    for (int attempt = 0;; ++attempt) {
        const auto capacity = static_cast<DWORD>(storage.size() * sizeof(std::uint64_t));
        if (::DeviceIoControl(volume.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, storage.data(),
                              capacity, &returned, nullptr)) {
            break;
        }
        const DWORD error = ::GetLastError();
        if (error != ERROR_MORE_DATA || attempt > 0) {
            return makeError(ErrorCode::IoError, "IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS failed", error);
        }
        DWORD needed = 0;
        std::memcpy(&needed, storage.data(), sizeof(needed));
        if (needed == 0 || needed > kMaxExtents) {
            return makeError(ErrorCode::IoError, "volume reported an implausible number of disk extents");
        }
        const std::size_t bytes = offsetof(VOLUME_DISK_EXTENTS, Extents) + needed * sizeof(DISK_EXTENT);
        storage.assign((bytes + 7) / 8, 0);
    }

    const auto* raw = reinterpret_cast<const std::byte*>(storage.data());
    if (returned < offsetof(VOLUME_DISK_EXTENTS, Extents)) {
        return makeError(ErrorCode::IoError, "truncated disk extent information");
    }
    DWORD count = 0;
    std::memcpy(&count, raw + offsetof(VOLUME_DISK_EXTENTS, NumberOfDiskExtents), sizeof(count));
    if (count > kMaxExtents ||
        offsetof(VOLUME_DISK_EXTENTS, Extents) + static_cast<std::size_t>(count) * sizeof(DISK_EXTENT) > returned) {
        return makeError(ErrorCode::IoError, "inconsistent disk extent information");
    }

    std::vector<std::uint32_t> disks;
    for (DWORD i = 0; i < count; ++i) {
        DISK_EXTENT extent{};
        std::memcpy(&extent, raw + offsetof(VOLUME_DISK_EXTENTS, Extents) + i * sizeof(DISK_EXTENT), sizeof(extent));
        disks.push_back(extent.DiskNumber);
    }
    return disks;
}

}  // namespace

DiskResolver makePlatformDiskResolver() {
    return &resolveDisks;
}

}  // namespace recovery::storage
