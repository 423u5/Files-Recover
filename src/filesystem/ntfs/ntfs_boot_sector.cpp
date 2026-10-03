#include "filesystem/ntfs/ntfs_boot_sector.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"

#include <bit>
#include <cstring>
#include <format>

namespace recovery::filesystem::ntfs {

namespace {

constexpr std::size_t kBootSectorSize = 512;

Error corrupted(std::string message) {
    return makeError(ErrorCode::CorruptedFilesystem, "NTFS boot sector: " + std::move(message));
}

}  // namespace

std::uint32_t decodeRecordSize(std::uint8_t raw, std::uint32_t clusterSize) noexcept {
    std::uint64_t bytes = 0;
    if (raw == 0) {
        return 0;
    }
    if (raw < 0x80) {
        bytes = static_cast<std::uint64_t>(raw) * clusterSize;
    } else {
        const unsigned shift = 256U - raw;  // -raw as a signed byte: 1 .. 128
        if (shift > 31) {
            return 0;
        }
        bytes = 1ULL << shift;
    }
    if (bytes < kMinRecordSize || bytes > kMaxRecordSize || !std::has_single_bit(bytes)) {
        return 0;
    }
    return static_cast<std::uint32_t>(bytes);
}

Result<BootSector> parseBootSector(std::span<const std::byte> s, std::uint64_t sourceSize) {
    if (s.size() < kBootSectorSize) {
        return makeError(ErrorCode::InvalidInput, "boot sector buffer is smaller than 512 bytes");
    }
    if (std::memcmp(s.data() + 3, "NTFS    ", 8) != 0) {
        return makeError(ErrorCode::UnsupportedFilesystem, "not an NTFS boot sector");
    }
    if (loadU8(s, 510) != 0x55 || loadU8(s, 511) != 0xAA) {
        return corrupted("boot signature is missing");
    }

    BootSector b;
    b.bytesPerSector = loadLe16(s, 11);
    b.sectorsPerClusterRaw = loadU8(s, 13);
    b.totalSectors = loadLe64(s, 40);
    b.mftCluster = loadLe64(s, 48);
    b.mftMirrorCluster = loadLe64(s, 56);
    b.recordSizeRaw = loadU8(s, 64);
    b.indexRecordSizeRaw = loadU8(s, 68);
    b.volumeSerial = loadLe64(s, 72);

    if (loadU8(s, 0) != 0xEB || loadU8(s, 1) != 0x52 || loadU8(s, 2) != 0x90) {
        b.warnings.push_back("boot sector has a non-standard jump instruction");
    }
    // The FAT fields of the BIOS parameter block must be zero on NTFS.
    if (loadLe16(s, 14) != 0 || loadU8(s, 16) != 0 || loadLe16(s, 17) != 0 || loadLe16(s, 19) != 0 ||
        loadLe16(s, 22) != 0 || loadLe32(s, 32) != 0) {
        b.warnings.push_back("unused FAT fields of the boot sector are not zero");
    }

    if (b.bytesPerSector != 512 && b.bytesPerSector != 1024 && b.bytesPerSector != 2048 &&
        b.bytesPerSector != 4096) {
        return corrupted("invalid bytes per sector " + std::to_string(b.bytesPerSector));
    }
    if (b.sectorsPerClusterRaw == 0) {
        return corrupted("sectors per cluster is zero");
    }
    if (b.sectorsPerClusterRaw <= 0x80) {
        if (!std::has_single_bit(b.sectorsPerClusterRaw)) {
            return corrupted("invalid sectors per cluster " + std::to_string(b.sectorsPerClusterRaw));
        }
        b.sectorsPerCluster = b.sectorsPerClusterRaw;
    } else {
        // Clusters above 64 KiB: the byte is -log2(sectors per cluster).
        const unsigned shift = 256U - b.sectorsPerClusterRaw;
        if (shift > 21) {
            return corrupted("invalid sectors per cluster value " + std::to_string(b.sectorsPerClusterRaw));
        }
        b.sectorsPerCluster = 1U << shift;
    }
    const std::uint64_t clusterSize = static_cast<std::uint64_t>(b.bytesPerSector) * b.sectorsPerCluster;
    if (clusterSize > kMaxClusterSize) {
        return corrupted("cluster size " + std::to_string(clusterSize) + " exceeds 2 MiB");
    }
    b.clusterSize = static_cast<std::uint32_t>(clusterSize);

    if (b.totalSectors == 0) {
        return corrupted("volume length is zero");
    }
    const std::optional<std::uint64_t> volumeBytes = checkedMul<std::uint64_t>(b.totalSectors, b.bytesPerSector);
    if (!volumeBytes) {
        return corrupted("volume length overflows");
    }
    b.volumeBytes = *volumeBytes;
    b.clusterCount = b.totalSectors / b.sectorsPerCluster;
    if (b.clusterCount == 0) {
        return corrupted("volume is smaller than one cluster");
    }

    b.recordSize = decodeRecordSize(b.recordSizeRaw, b.clusterSize);
    if (b.recordSize == 0) {
        return corrupted(std::format("invalid MFT record size value 0x{:02X}", b.recordSizeRaw));
    }
    b.indexRecordSize = decodeRecordSize(b.indexRecordSizeRaw, b.clusterSize);
    if (b.indexRecordSize == 0) {
        b.warnings.push_back(std::format("invalid index record size value 0x{:02X}", b.indexRecordSizeRaw));
    }

    // The MFT's first record must lie inside the volume. The mirror is only a
    // fallback, so a bad mirror location is not fatal.
    if (!b.isValidCluster(b.mftCluster) ||
        !rangeWithin<std::uint64_t>(b.clusterOffset(b.mftCluster), b.recordSize, b.volumeBytes)) {
        return corrupted("MFT cluster " + std::to_string(b.mftCluster) + " is outside the volume");
    }
    if (!b.isValidCluster(b.mftMirrorCluster) ||
        !rangeWithin<std::uint64_t>(b.clusterOffset(b.mftMirrorCluster), b.recordSize, b.volumeBytes)) {
        b.warnings.push_back("MFT mirror cluster " + std::to_string(b.mftMirrorCluster) + " is outside the volume");
    }

    if (b.volumeBytes > sourceSize) {
        b.warnings.push_back("volume is larger than its source (" + std::to_string(b.volumeBytes) + " > " +
                             std::to_string(sourceSize) + " bytes); the image may be truncated");
    }
    return b;
}

}  // namespace recovery::filesystem::ntfs
