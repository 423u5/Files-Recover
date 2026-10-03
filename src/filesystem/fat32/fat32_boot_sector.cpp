#include "filesystem/fat32/fat32_boot_sector.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

namespace recovery::filesystem::fat32 {

namespace {

constexpr std::size_t kBootSectorSize = 512;

std::string trimmedAscii(std::span<const std::byte> bytes) {
    std::string text;
    for (const std::byte b : bytes) {
        const auto c = static_cast<unsigned char>(b);
        text += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

bool matches(std::span<const std::byte> data, std::size_t offset, std::string_view text) {
    return std::memcmp(data.data() + offset, text.data(), text.size()) == 0;
}

Error unsupported(std::string message) {
    return makeError(ErrorCode::UnsupportedFilesystem, std::move(message));
}

Error corrupted(std::string message) {
    return makeError(ErrorCode::CorruptedFilesystem, "FAT32 boot sector: " + std::move(message));
}

}  // namespace

Result<BootSector> parseBootSector(std::span<const std::byte> s, std::uint64_t sourceSize) {
    if (s.size() < kBootSectorSize) {
        return makeError(ErrorCode::InvalidInput, "boot sector buffer is smaller than 512 bytes");
    }
    if (loadU8(s, 510) != 0x55 || loadU8(s, 511) != 0xAA) {
        return unsupported("no boot sector signature");
    }
    if (matches(s, 3, "EXFAT   ") || matches(s, 3, "NTFS    ")) {
        return unsupported("not a FAT volume");
    }

    BootSector b;
    b.oemName = trimmedAscii(s.subspan(3, 8));
    b.bytesPerSector = loadLe16(s, 11);
    b.sectorsPerCluster = loadU8(s, 13);
    b.reservedSectors = loadLe16(s, 14);
    b.fatCount = loadU8(s, 16);
    const std::uint16_t rootEntries = loadLe16(s, 17);
    const std::uint16_t totalSectors16 = loadLe16(s, 19);
    b.media = loadU8(s, 21);
    const std::uint16_t fatSectors16 = loadLe16(s, 22);
    b.totalSectors = loadLe32(s, 32);
    b.fatSectors = loadLe32(s, 36);
    b.extFlags = loadLe16(s, 40);
    const std::uint16_t fsVersion = loadLe16(s, 42);
    b.rootCluster = loadLe32(s, 44);
    b.fsInfoSector = loadLe16(s, 48);
    b.backupBootSector = loadLe16(s, 50);
    const std::uint8_t extendedSignature = loadU8(s, 66);
    if (extendedSignature == 0x29 || extendedSignature == 0x28) {
        b.volumeId = loadLe32(s, 67);
    }
    if (extendedSignature == 0x29) {
        b.volumeLabel = trimmedAscii(s.subspan(71, 11));
        if (b.volumeLabel == "NO NAME") {
            b.volumeLabel.clear();
        }
    }

    // FAT32 identity: FAT12/16 use the 16-bit FAT size and a fixed root directory.
    if (fatSectors16 != 0 || rootEntries != 0) {
        return unsupported("FAT12/FAT16 volume (only FAT32 is supported)");
    }
    if (b.fatSectors == 0) {
        return unsupported("not a FAT32 boot sector");
    }

    const std::uint8_t jump = loadU8(s, 0);
    if (!((jump == 0xEB && loadU8(s, 2) == 0x90) || jump == 0xE9)) {
        b.warnings.push_back("boot sector has no x86 jump instruction");
    }
    if (b.bytesPerSector != 512 && b.bytesPerSector != 1024 && b.bytesPerSector != 2048 &&
        b.bytesPerSector != 4096) {
        return corrupted("invalid bytes per sector " + std::to_string(b.bytesPerSector));
    }
    if (b.sectorsPerCluster == 0 || !std::has_single_bit(b.sectorsPerCluster)) {
        return corrupted("invalid sectors per cluster " + std::to_string(b.sectorsPerCluster));
    }
    b.clusterSize = static_cast<std::uint32_t>(b.bytesPerSector) * b.sectorsPerCluster;
    if (b.clusterSize > kMaxClusterSize) {
        return corrupted("cluster size " + std::to_string(b.clusterSize) + " is too large");
    }
    if (b.clusterSize > 32 * 1024) {
        b.warnings.push_back("cluster size above 32 KiB is non-standard");
    }
    if (b.reservedSectors == 0) {
        return corrupted("no reserved sectors");
    }
    if (b.fatCount == 0 || b.fatCount > 4) {
        return corrupted("invalid FAT count " + std::to_string(b.fatCount));
    }
    if (b.fatCount != 2) {
        b.warnings.push_back("FAT count " + std::to_string(b.fatCount) + " is unusual");
    }
    if (totalSectors16 != 0) {
        b.warnings.push_back("16-bit total sector field is set on a FAT32 volume");
    }
    if (b.totalSectors == 0) {
        return corrupted("total sector count is zero");
    }
    if (b.media != 0xF0 && b.media < 0xF8) {
        b.warnings.push_back(std::format("unusual media descriptor 0x{:02X}", b.media));
    }
    if (fsVersion != 0) {
        b.warnings.push_back("unknown FAT32 version " + std::to_string(fsVersion));
    }

    // Layout, with every product and sum checked (all fields are untrusted).
    const std::uint64_t bps = b.bytesPerSector;
    b.fatOffset = b.reservedSectors * bps;
    b.fatBytes = static_cast<std::uint64_t>(b.fatSectors) * bps;
    const std::uint64_t dataSector =
        b.reservedSectors + static_cast<std::uint64_t>(b.fatCount) * b.fatSectors;  // < 2^35
    if (dataSector >= b.totalSectors) {
        return corrupted("the FAT region extends past the end of the volume");
    }
    b.dataOffset = dataSector * bps;
    b.volumeBytes = static_cast<std::uint64_t>(b.totalSectors) * bps;

    std::uint64_t clusters = (b.totalSectors - dataSector) / b.sectorsPerCluster;
    if (clusters == 0) {
        return corrupted("volume has no data clusters");
    }
    const std::uint64_t fatCapacity = b.fatBytes / 4;  // entries, including the two reserved ones
    if (fatCapacity < 3) {
        return corrupted("FAT is too small");
    }
    if (clusters > fatCapacity - 2) {
        b.warnings.push_back("FAT covers only " + std::to_string(fatCapacity - 2) + " of " +
                             std::to_string(clusters) + " clusters; the rest is unreachable");
        clusters = fatCapacity - 2;
    }
    if (clusters > kMaxClusters) {
        b.warnings.push_back("cluster count exceeds the FAT32 maximum; extra clusters ignored");
        clusters = kMaxClusters;
    }
    b.clusterCount = static_cast<std::uint32_t>(clusters);
    if (b.clusterCount < kMinStandardClusters) {
        b.warnings.push_back("only " + std::to_string(b.clusterCount) +
                             " clusters: below the FAT32 minimum of 65525 (non-standard volume)");
    }

    if (!b.isValidCluster(b.rootCluster)) {
        return corrupted("root directory cluster " + std::to_string(b.rootCluster) + " is out of range");
    }

    b.mirrored = (b.extFlags & 0x80) == 0;
    b.activeFat = b.mirrored ? 0 : (b.extFlags & 0x0F);
    if (b.activeFat >= b.fatCount) {
        b.warnings.push_back("active FAT index is out of range; using FAT 0");
        b.activeFat = 0;
    }
    if (b.fsInfoSector == 0 || b.fsInfoSector >= b.reservedSectors) {
        b.warnings.push_back("FSInfo sector lies outside the reserved region");
    }
    if (b.volumeBytes > sourceSize) {
        b.warnings.push_back("volume is larger than its source (" + std::to_string(b.volumeBytes) + " > " +
                             std::to_string(sourceSize) + " bytes); the image may be truncated");
    }
    return b;
}

std::optional<FsInfo> parseFsInfo(std::span<const std::byte> s, const BootSector& boot) {
    if (s.size() < 512 || loadLe32(s, 0) != 0x41615252 || loadLe32(s, 484) != 0x61417272 ||
        loadLe32(s, 508) != 0xAA550000) {
        return std::nullopt;
    }
    FsInfo info;
    const std::uint32_t freeCount = loadLe32(s, 488);
    if (freeCount != 0xFFFFFFFF && freeCount <= boot.clusterCount) {
        info.freeClusters = freeCount;
    }
    const std::uint32_t nextFree = loadLe32(s, 492);
    if (boot.isValidCluster(nextFree)) {
        info.nextFree = nextFree;
    }
    return info;
}

}  // namespace recovery::filesystem::fat32
