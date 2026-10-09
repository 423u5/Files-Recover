#pragma once

// The physical disks attached to the computer (P19): what a user interface
// lists for the user to choose a source from. Nothing is read from a disk:
// the operating system is asked what it knows about each one, through
// handles that cannot read or change anything.

#include "recovery/result.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace recovery::storage {

struct AttachedDisk {
    // Its number: the N of \\.\PhysicalDriveN.
    std::uint32_t number = 0;
    // Bytes, and the logical sector size; 0 when the system does not know
    // them (a card reader without a card).
    std::uint64_t sizeBytes = 0;
    std::uint32_t logicalSectorSize = 0;
    std::string vendor;
    std::string product;
    std::optional<bool> removable;
    // How it is attached: "USB", "SATA", "NVMe", "SD", "MMC", "SAS", "SCSI",
    // "ATA", "ATAPI", "1394", "Fibre Channel", "iSCSI", "RAID", "Virtual",
    // "File-backed virtual", "Storage Spaces", "UFS", "SCM"; empty when not
    // known.
    std::string bus;
    // The roots of its volumes that have a drive letter ("E:\"), by letter.
    std::vector<std::filesystem::path> volumes;
};

// Lists the disks, by number. Fails only when the system cannot be asked;
// a disk that cannot be queried is listed with what is known of it.
using DiskLister = std::function<Result<std::vector<AttachedDisk>>()>;

// The operating system's lister (src/storage/windows/windows_disk_list.cpp).
[[nodiscard]] DiskLister makePlatformDiskLister();

}  // namespace recovery::storage
