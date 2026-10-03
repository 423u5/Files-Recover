#include "filesystem/exfat/exfat_boot_sector.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace recovery::filesystem::exfat {

namespace {

constexpr std::size_t kBootSectorSize = 512;
constexpr std::uint32_t kMinFatOffsetSectors = 24;  // after the main and backup boot regions

Error unsupported(std::string message) {
    return makeError(ErrorCode::UnsupportedFilesystem, std::move(message));
}

Error corrupted(std::string message) {
    return makeError(ErrorCode::CorruptedFilesystem, "exFAT boot sector: " + std::move(message));
}

}  // namespace

Result<BootSector> parseBootSector(std::span<const std::byte> s, std::uint64_t sourceSize) {
    if (s.size() < kBootSectorSize) {
        return makeError(ErrorCode::InvalidInput, "boot sector buffer is smaller than 512 bytes");
    }
    if (std::memcmp(s.data() + 3, "EXFAT   ", 8) != 0) {
        return unsupported("not an exFAT boot sector");
    }
    if (loadU8(s, 510) != 0x55 || loadU8(s, 511) != 0xAA) {
        return corrupted("boot signature is missing");
    }

    BootSector b;
    b.partitionOffset = loadLe64(s, 64);
    b.volumeLengthSectors = loadLe64(s, 72);
    b.fatOffsetSectors = loadLe32(s, 80);
    b.fatLengthSectors = loadLe32(s, 84);
    b.clusterHeapOffsetSectors = loadLe32(s, 88);
    b.recordedClusterCount = loadLe32(s, 92);
    b.rootCluster = loadLe32(s, 96);
    b.volumeSerial = loadLe32(s, 100);
    b.revisionMinor = loadU8(s, 104);
    b.revisionMajor = loadU8(s, 105);
    b.volumeFlags = loadLe16(s, 106);
    b.bytesPerSectorShift = loadU8(s, 108);
    b.sectorsPerClusterShift = loadU8(s, 109);
    b.fatCount = loadU8(s, 110);
    b.percentInUse = loadU8(s, 112);

    if (b.revisionMajor != 1) {
        return unsupported(std::format("exFAT revision {}.{:02} is not supported", b.revisionMajor, b.revisionMinor));
    }
    if (loadU8(s, 0) != 0xEB || loadU8(s, 1) != 0x76 || loadU8(s, 2) != 0x90) {
        b.warnings.push_back("boot sector has a non-standard jump instruction");
    }
    // Bytes 11-63 overlay the FAT BIOS parameter block and must be zero so
    // that FAT implementations do not mount the volume.
    if (std::any_of(s.begin() + 11, s.begin() + 64, [](std::byte v) { return v != std::byte{0}; })) {
        b.warnings.push_back("bytes 11-63 of the boot sector are not zero");
    }

    if (b.bytesPerSectorShift < 9 || b.bytesPerSectorShift > 12) {
        return corrupted("invalid bytes-per-sector shift " + std::to_string(b.bytesPerSectorShift));
    }
    if (b.sectorsPerClusterShift > 25 - b.bytesPerSectorShift) {
        return corrupted("invalid sectors-per-cluster shift " + std::to_string(b.sectorsPerClusterShift));
    }
    b.bytesPerSector = 1U << b.bytesPerSectorShift;
    b.clusterSize = b.bytesPerSector << b.sectorsPerClusterShift;
    if (b.fatCount != 1 && b.fatCount != 2) {
        return corrupted("invalid FAT count " + std::to_string(b.fatCount));
    }
    if (b.volumeLengthSectors == 0) {
        return corrupted("volume length is zero");
    }
    const std::optional<std::uint64_t> volumeBytes = checkedMul<std::uint64_t>(b.volumeLengthSectors, b.bytesPerSector);
    if (!volumeBytes) {
        return corrupted("volume length overflows");
    }
    b.volumeBytes = *volumeBytes;
    if (b.volumeBytes < kMinVolumeBytes) {
        b.warnings.push_back("volume is smaller than the 1 MiB exFAT minimum");
    }

    // Layout. Every field is untrusted; the sums below cannot overflow 64 bits
    // (32-bit sector counts times at most 2).
    if (b.fatOffsetSectors < kMinFatOffsetSectors) {
        return corrupted("the FAT overlaps the boot regions (offset " + std::to_string(b.fatOffsetSectors) + ")");
    }
    if (b.fatLengthSectors == 0) {
        return corrupted("FAT length is zero");
    }
    const std::uint64_t fatEnd =
        b.fatOffsetSectors + static_cast<std::uint64_t>(b.fatLengthSectors) * b.fatCount;
    if (fatEnd > b.clusterHeapOffsetSectors) {
        return corrupted("the FAT region overlaps the cluster heap");
    }
    if (b.clusterHeapOffsetSectors >= b.volumeLengthSectors) {
        return corrupted("the cluster heap starts beyond the end of the volume");
    }
    if (b.recordedClusterCount == 0) {
        return corrupted("cluster count is zero");
    }
    b.fatOffset = static_cast<std::uint64_t>(b.fatOffsetSectors) << b.bytesPerSectorShift;
    b.fatBytes = static_cast<std::uint64_t>(b.fatLengthSectors) << b.bytesPerSectorShift;
    b.dataOffset = static_cast<std::uint64_t>(b.clusterHeapOffsetSectors) << b.bytesPerSectorShift;

    std::uint64_t clusters = b.recordedClusterCount;
    const std::uint64_t heapClusters =
        (b.volumeLengthSectors - b.clusterHeapOffsetSectors) >> b.sectorsPerClusterShift;
    if (clusters > heapClusters) {
        b.warnings.push_back("cluster count " + std::to_string(clusters) + " exceeds the " +
                             std::to_string(heapClusters) + " clusters that fit in the volume; the rest are ignored");
        clusters = heapClusters;
    }
    const std::uint64_t fatCapacity = b.fatBytes / 4;  // entries, including the two reserved ones
    if (fatCapacity < 3) {
        return corrupted("FAT is too small");
    }
    if (clusters > fatCapacity - 2) {
        b.warnings.push_back("FAT covers only " + std::to_string(fatCapacity - 2) + " of " +
                             std::to_string(clusters) + " clusters; the rest are ignored");
        clusters = fatCapacity - 2;
    }
    if (clusters > kMaxClusters) {
        b.warnings.push_back("cluster count exceeds the exFAT maximum; extra clusters ignored");
        clusters = kMaxClusters;
    }
    if (clusters == 0) {
        return corrupted("volume has no usable clusters");
    }
    b.clusterCount = static_cast<std::uint32_t>(clusters);

    if (!b.isValidCluster(b.rootCluster)) {
        return corrupted("root directory cluster " + std::to_string(b.rootCluster) + " is out of range");
    }

    b.activeFat = b.volumeFlags & 0x01U;
    if (b.activeFat >= b.fatCount) {
        b.warnings.push_back("active FAT flag selects a FAT that does not exist; using FAT 0");
        b.activeFat = 0;
    }
    b.dirty = (b.volumeFlags & 0x02U) != 0;
    b.mediaFailure = (b.volumeFlags & 0x04U) != 0;
    if (b.dirty) {
        b.warnings.push_back("volume is marked dirty (it was not cleanly unmounted)");
    }
    if (b.mediaFailure) {
        b.warnings.push_back("volume records media failures");
    }
    if (b.percentInUse > 100 && b.percentInUse != 0xFF) {
        b.warnings.push_back("invalid percent-in-use value " + std::to_string(b.percentInUse));
    }
    if (b.volumeBytes > sourceSize) {
        b.warnings.push_back("volume is larger than its source (" + std::to_string(b.volumeBytes) + " > " +
                             std::to_string(sourceSize) + " bytes); the image may be truncated");
    }
    return b;
}

std::uint32_t bootChecksum(std::span<const std::byte> region, std::uint32_t bytesPerSector) noexcept {
    const std::size_t length = std::min<std::size_t>(region.size(), static_cast<std::size_t>(bytesPerSector) * 11);
    std::uint32_t checksum = 0;
    for (std::size_t i = 0; i < length; ++i) {
        if (i == 106 || i == 107 || i == 112) {
            continue;  // VolumeFlags and PercentInUse change without a checksum update
        }
        checksum = ((checksum & 1U) != 0 ? 0x80000000U : 0U) + (checksum >> 1) + static_cast<std::uint8_t>(region[i]);
    }
    return checksum;
}

bool bootChecksumMatches(std::span<const std::byte> region, std::uint32_t bytesPerSector) noexcept {
    const std::size_t sector = bytesPerSector;
    if (sector < 512 || region.size() < sector * kBootRegionSectors) {
        return false;
    }
    const std::uint32_t expected = bootChecksum(region, bytesPerSector);
    const std::span<const std::byte> stored = region.subspan(sector * 11, sector);
    for (std::size_t offset = 0; offset < sector; offset += 4) {
        if (loadLe32(stored, offset) != expected) {
            return false;
        }
    }
    return true;
}

}  // namespace recovery::filesystem::exfat
