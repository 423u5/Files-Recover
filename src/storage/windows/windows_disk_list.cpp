// Lists the physical disks and the drive letters of their volumes.
//
// Windows assumptions:
//  * The disks are the present device interfaces of GUID_DEVINTERFACE_DISK
//    (SetupAPI). Each is opened with desired access 0: attributes only, so
//    no administrator rights are needed and nothing can be read or changed
//    through the handle. Its number comes from IOCTL_STORAGE_GET_DEVICE_NUMBER,
//    its vendor, product, removability and bus from IOCTL_STORAGE_QUERY_PROPERTY,
//    its size and sector size from IOCTL_DISK_GET_DRIVE_GEOMETRY_EX (which
//    fails for a reader without a medium: size 0). All three are
//    FILE_ANY_ACCESS requests.
//  * Drive letters: GetLogicalDrives(); each fixed or removable drive's
//    volume is resolved to its disks as destination checks resolve paths
//    (IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS). Network, optical and SUBST
//    drives and drives that cannot be resolved (no medium) are left out.
//    Critical-error dialogs ("insert a disk") are turned off for the calling
//    thread while the drives are asked.

#include "storage/destination_guard.hpp"
#include "storage/disk_list.hpp"

#include "win32.hpp"

#include <setupapi.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace recovery::storage {

namespace {

using windows::UniqueHandle;

// GUID_DEVINTERFACE_DISK {53F56307-B6BF-11D0-94F2-00A0C91EFB8B} (ntddstor.h),
// spelled out so that no translation unit has to define it with INITGUID.
constexpr GUID kDiskInterface = {0x53F56307, 0xB6BF, 0x11D0, {0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B}};

// Bounds on what the system hands back.
constexpr DWORD kMaxInterfaces = 4096;
constexpr DWORD kMaxDetailBytes = 64 * 1024;

class DeviceInfoSet {
public:
    explicit DeviceInfoSet(HDEVINFO set) noexcept : set_(set) {}
    ~DeviceInfoSet() {
        if (valid()) {
            ::SetupDiDestroyDeviceInfoList(set_);
        }
    }
    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;
    DeviceInfoSet(DeviceInfoSet&&) = delete;
    DeviceInfoSet& operator=(DeviceInfoSet&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return set_ != INVALID_HANDLE_VALUE && set_ != nullptr; }
    [[nodiscard]] HDEVINFO get() const noexcept { return set_; }

private:
    HDEVINFO set_;
};

// Turns off the critical-error dialogs of the calling thread while it lives.
class QuietErrors {
public:
    QuietErrors() noexcept { changed_ = ::SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previous_) != FALSE; }
    ~QuietErrors() {
        if (changed_) {
            ::SetThreadErrorMode(previous_, nullptr);
        }
    }
    QuietErrors(const QuietErrors&) = delete;
    QuietErrors& operator=(const QuietErrors&) = delete;
    QuietErrors(QuietErrors&&) = delete;
    QuietErrors& operator=(QuietErrors&&) = delete;

private:
    DWORD previous_ = 0;
    bool changed_ = false;
};

std::string trimmed(std::string text) {
    const auto notSpace = [](char c) { return c != ' ' && c != '\0'; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

// A NUL-terminated ASCII string at `offset` within the first `size` bytes of
// `data`, never read past `size`; other characters become '?'.
std::string boundedString(const std::byte* data, std::size_t size, DWORD offset) {
    if (offset == 0 || offset >= size) {
        return {};
    }
    std::string text;
    for (std::size_t i = offset; i < size && text.size() < 256; ++i) {
        const auto c = static_cast<unsigned char>(data[i]);
        if (c == 0) {
            break;
        }
        text += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?';
    }
    return trimmed(std::move(text));
}

std::string busName(STORAGE_BUS_TYPE bus) {
    switch (bus) {
    case BusTypeScsi:
        return "SCSI";
    case BusTypeAtapi:
        return "ATAPI";
    case BusTypeAta:
        return "ATA";
    case BusType1394:
        return "1394";
    case BusTypeSsa:
        return "SSA";
    case BusTypeFibre:
        return "Fibre Channel";
    case BusTypeUsb:
        return "USB";
    case BusTypeRAID:
        return "RAID";
    case BusTypeiScsi:
        return "iSCSI";
    case BusTypeSas:
        return "SAS";
    case BusTypeSata:
        return "SATA";
    case BusTypeSd:
        return "SD";
    case BusTypeMmc:
        return "MMC";
    case BusTypeVirtual:
        return "Virtual";
    case BusTypeFileBackedVirtual:
        return "File-backed virtual";
    case BusTypeSpaces:
        return "Storage Spaces";
    case BusTypeNvme:
        return "NVMe";
    case BusTypeSCM:
        return "SCM";
    case BusTypeUfs:
        return "UFS";
    default:
        return {};
    }
}

void describe(HANDLE handle, AttachedDisk& disk) {
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    // Aligned storage for STORAGE_DEVICE_DESCRIPTOR plus its trailing strings.
    std::vector<std::uint64_t> storage(512);
    const auto capacity = static_cast<DWORD>(storage.size() * sizeof(std::uint64_t));
    DWORD returned = 0;
    if (::DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), storage.data(), capacity,
                          &returned, nullptr) &&
        returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const auto* bytes = reinterpret_cast<const std::byte*>(storage.data());
        STORAGE_DEVICE_DESCRIPTOR header{};
        std::memcpy(&header, bytes, sizeof(header));
        const std::size_t valid = std::min<std::size_t>({returned, header.Size, capacity});
        disk.vendor = boundedString(bytes, valid, header.VendorIdOffset);
        disk.product = boundedString(bytes, valid, header.ProductIdOffset);
        disk.removable = header.RemovableMedia != FALSE;
        disk.bus = busName(header.BusType);
    }

    DISK_GEOMETRY_EX geometry{};
    returned = 0;
    if (::DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry, sizeof(geometry),
                          &returned, nullptr) &&
        returned >= offsetof(DISK_GEOMETRY_EX, Data) && geometry.DiskSize.QuadPart > 0) {
        disk.sizeBytes = static_cast<std::uint64_t>(geometry.DiskSize.QuadPart);
        disk.logicalSectorSize = geometry.Geometry.BytesPerSector;
    }
}

// The device path of the interface `data`, or empty when it cannot be had.
std::wstring interfacePath(HDEVINFO set, SP_DEVICE_INTERFACE_DATA& data) {
    DWORD required = 0;
    ::SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &required, nullptr);
    if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || required > kMaxDetailBytes) {
        return {};
    }
    // Aligned storage for the variable-length structure.
    std::vector<std::uint64_t> storage((required + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!::SetupDiGetDeviceInterfaceDetailW(set, &data, detail, required, nullptr, nullptr)) {
        return {};
    }
    const std::size_t maxChars = (required - offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath)) / sizeof(wchar_t);
    const wchar_t* path = detail->DevicePath;
    return std::wstring(path, std::find(path, path + maxChars, L'\0'));
}

Result<std::vector<AttachedDisk>> listDisks() {
    DeviceInfoSet set(::SetupDiGetClassDevsW(&kDiskInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (!set.valid()) {
        const DWORD error = ::GetLastError();
        return makeError(ErrorCode::IoError, "cannot list the disks (SetupDiGetClassDevsW)", error);
    }
    std::map<std::uint32_t, AttachedDisk> disks;
    for (DWORD index = 0; index < kMaxInterfaces; ++index) {
        SP_DEVICE_INTERFACE_DATA data{};
        data.cbSize = sizeof(data);
        if (!::SetupDiEnumDeviceInterfaces(set.get(), nullptr, &kDiskInterface, index, &data)) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }
        const std::wstring path = interfacePath(set.get(), data);
        if (path.empty()) {
            continue;
        }
        UniqueHandle handle(::CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                          0, nullptr));
        if (!handle.valid()) {
            continue;
        }
        STORAGE_DEVICE_NUMBER number{};
        DWORD returned = 0;
        if (!::DeviceIoControl(handle.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number, sizeof(number),
                               &returned, nullptr) ||
            returned < sizeof(number) || number.DeviceType != FILE_DEVICE_DISK) {
            continue;
        }
        AttachedDisk disk;
        disk.number = number.DeviceNumber;
        describe(handle.get(), disk);
        disks.insert_or_assign(disk.number, std::move(disk));
    }

    const QuietErrors quiet;
    const DiskResolver resolve = makePlatformDiskResolver();
    const DWORD letters = ::GetLogicalDrives();
    for (int letter = 0; letter < 26; ++letter) {
        if ((letters & (1u << letter)) == 0) {
            continue;
        }
        const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + letter)) + L":\\";
        const UINT type = ::GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
            continue;
        }
        const Result<std::vector<std::uint32_t>> on = resolve(std::filesystem::path(root));
        if (!on.ok()) {
            continue;
        }
        for (const std::uint32_t number : *on) {
            if (const auto found = disks.find(number); found != disks.end()) {
                found->second.volumes.emplace_back(root);
            }
        }
    }

    std::vector<AttachedDisk> listed;
    listed.reserve(disks.size());
    for (auto& [number, disk] : disks) {
        listed.push_back(std::move(disk));
    }
    return listed;
}

}  // namespace

DiskLister makePlatformDiskLister() {
    return &listDisks;
}

}  // namespace recovery::storage
