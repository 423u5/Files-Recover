#include "support/exfat_builder.hpp"

#include "recovery/byte_order.hpp"
#include "support/fat32_builder.hpp"  // utf8ToUtf16

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace recovery::test {

namespace {

constexpr std::uint32_t kEndOfChain = 0xFFFFFFFF;
constexpr std::size_t kEntrySize = 32;
constexpr std::size_t kNameChars = 15;
constexpr std::uint16_t kDate = (44 << 9) | (5 << 5) | 17;    // 2024-05-17
constexpr std::uint16_t kTime = (13 << 11) | (45 << 5) | 15;  // 13:45:30
constexpr std::uint32_t kTimestamp = (static_cast<std::uint32_t>(kDate) << 16) | kTime;
constexpr std::uint8_t kCreate10ms = 50;         // +0.5 s
constexpr std::uint8_t kUtcPlus2 = 0x80 | 8;     // valid, +8 quarter hours
constexpr std::uint16_t kAttrDirectory = 0x10;
constexpr std::uint16_t kAttrArchive = 0x20;

// The rotating checksums of the exFAT specification, written out here
// independently of the engine.
std::uint16_t setChecksum(std::span<const std::byte> set) {
    std::uint16_t sum = 0;
    for (std::size_t i = 0; i < set.size(); ++i) {
        if (i != 2 && i != 3) {
            sum = static_cast<std::uint16_t>(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + static_cast<std::uint8_t>(set[i]));
        }
    }
    return sum;
}

std::uint32_t rotatingSum32(std::span<const std::byte> bytes, bool skipBootFields) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (skipBootFields && (i == 106 || i == 107 || i == 112)) {
            continue;
        }
        sum = ((sum & 1) ? 0x80000000U : 0U) + (sum >> 1) + static_cast<std::uint8_t>(bytes[i]);
    }
    return sum;
}

}  // namespace

ExFatImageBuilder::ExFatImageBuilder(ExFatBuilderOptions options)
    : options_(std::move(options)),
      bytesPerSector_(1U << options_.bytesPerSectorShift),
      clusterSize_(bytesPerSector_ << options_.sectorsPerClusterShift) {
    const std::uint32_t sectorsPerCluster = 1U << options_.sectorsPerClusterShift;
    fatLengthSectors_ = static_cast<std::uint32_t>(
        ((static_cast<std::uint64_t>(options_.clusterCount) + 2) * 4 + bytesPerSector_ - 1) / bytesPerSector_);
    const std::uint32_t fatEnd = options_.fatOffsetSectors + fatLengthSectors_ * options_.fatCount;
    heapOffsetSectors_ = (fatEnd + sectorsPerCluster - 1) / sectorsPerCluster * sectorsPerCluster;
    volumeSectors_ = heapOffsetSectors_ + static_cast<std::uint64_t>(options_.clusterCount) * sectorsPerCluster;
    image_.assign(static_cast<std::size_t>(volumeSectors_ * bytesPerSector_), std::byte{0});

    setFat(0, 0xFFFFFFF8);
    setFat(1, kEndOfChain);

    // Up-case table: ASCII, Latin-1, Greek and Cyrillic letters; everything else maps to itself.
    for (std::size_t i = 0; i < upcaseMap_.size(); ++i) {
        upcaseMap_[i] = static_cast<char16_t>(i);
    }
    const auto mapRange = [&](char16_t from, char16_t to, int delta) {
        for (char16_t c = from; c <= to; ++c) {
            upcaseMap_[c] = static_cast<char16_t>(c + delta);
        }
    };
    mapRange(u'a', u'z', -0x20);
    mapRange(0xE0, 0xF6, -0x20);
    mapRange(0xF8, 0xFE, -0x20);
    upcaseMap_[0xFF] = 0x178;
    mapRange(0x3B1, 0x3C1, -0x20);
    upcaseMap_[0x3C2] = 0x3A3;  // final sigma
    mapRange(0x3C3, 0x3C9, -0x20);
    mapRange(0x430, 0x44F, -0x20);
    mapRange(0x450, 0x45F, -0x50);
    // Compressed form: identity runs become 0xFFFF followed by the run length.
    std::vector<char16_t> units;
    for (std::size_t i = 0; i < upcaseMap_.size();) {
        std::size_t run = 0;
        while (i + run < upcaseMap_.size() && static_cast<std::size_t>(upcaseMap_[i + run]) == i + run &&
               run < 0xFFFF) {
            ++run;
        }
        if (run >= 3) {
            units.push_back(0xFFFF);
            units.push_back(static_cast<char16_t>(run));
            i += run;
        } else {
            units.push_back(upcaseMap_[i]);
            ++i;
        }
    }
    upcase_.resize(units.size() * 2);
    for (std::size_t i = 0; i < units.size(); ++i) {
        storeLe16(upcase_, i * 2, units[i]);
    }
    upcaseChecksum_ = rotatingSum32(upcase_, false);

    // Allocation bitmap, up-case table and root directory, in that order, from cluster 2.
    const std::size_t bitmapBytes = (options_.clusterCount + 7) / 8;
    const std::vector<std::uint32_t> bitmap = allocateRun((bitmapBytes + clusterSize_ - 1) / clusterSize_);
    chain(bitmap);
    bitmapCluster_ = bitmap.front();
    const std::vector<std::uint32_t> table = allocateRun((upcase_.size() + clusterSize_ - 1) / clusterSize_);
    chain(table);
    upcaseCluster_ = table.front();
    writeData(table, upcase_);
    rootCluster_ = allocate();
    setFat(rootCluster_, kEndOfChain);
    Directory& rootDirectory = directories_[rootCluster_];
    rootDirectory.clusters = {rootCluster_};
    rootDirectory.contiguous = false;

    // Root entries in Windows' order: label, bitmap, up-case table, volume GUID.
    const std::u16string label = utf8ToUtf16(options_.label);
    if (label.size() > 11) {
        throw std::runtime_error("volume label too long");
    }
    std::span<std::byte> e = directorySlot(rootDirectory, rootDirectory.nextSlot++);
    e[0] = std::byte{label.empty() ? std::uint8_t{0x03} : std::uint8_t{0x83}};
    e[1] = static_cast<std::byte>(label.size());
    for (std::size_t i = 0; i < label.size(); ++i) {
        storeLe16(e, 2 + 2 * i, label[i]);
    }
    e = directorySlot(rootDirectory, rootDirectory.nextSlot++);
    e[0] = std::byte{0x81};
    storeLe32(e, 20, bitmapCluster_);
    storeLe64(e, 24, bitmapBytes);
    e = directorySlot(rootDirectory, rootDirectory.nextSlot++);
    e[0] = std::byte{0x82};
    storeLe32(e, 4, upcaseChecksum_);
    storeLe32(e, 20, upcaseCluster_);
    storeLe64(e, 24, upcase_.size());
    if (options_.volumeGuidEntry) {
        e = directorySlot(rootDirectory, rootDirectory.nextSlot++);
        e[0] = std::byte{0xA0};
        const std::array<std::byte, 16> guid = {std::byte{0x10}, std::byte{0x32}, std::byte{0x54}, std::byte{0x76},
                                                std::byte{0x98}, std::byte{0xBA}, std::byte{0xDC}, std::byte{0xFE},
                                                std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
                                                std::byte{0x89}, std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}};
        std::copy(guid.begin(), guid.end(), e.begin() + 6);
        storeLe16(e, 2, setChecksum(e));
    }
}

std::uint64_t ExFatImageBuilder::fatOffset() const {
    return static_cast<std::uint64_t>(options_.fatOffsetSectors) * bytesPerSector_;
}

std::uint64_t ExFatImageBuilder::clusterOffset(std::uint32_t cluster) const {
    return static_cast<std::uint64_t>(heapOffsetSectors_) * bytesPerSector_ +
           static_cast<std::uint64_t>(cluster - 2) * clusterSize_;
}

void ExFatImageBuilder::setFat(std::uint32_t cluster, std::uint32_t value) {
    for (std::uint32_t copy = 0; copy < options_.fatCount; ++copy) {
        const std::uint64_t offset =
            fatOffset() + static_cast<std::uint64_t>(copy) * fatLengthSectors_ * bytesPerSector_ + cluster * 4ULL;
        storeLe32(image_, static_cast<std::size_t>(offset), value);
    }
}

std::uint32_t ExFatImageBuilder::fat(std::uint32_t cluster) const {
    return loadLe32(image_, static_cast<std::size_t>(fatOffset() + cluster * 4ULL));
}

void ExFatImageBuilder::setAllocated(std::uint32_t cluster, bool allocated) {
    const std::uint32_t bit = cluster - 2;
    const auto offset = static_cast<std::size_t>(clusterOffset(bitmapCluster_) + bit / 8);
    const auto mask = static_cast<std::byte>(1U << (bit % 8));
    image_[offset] = allocated ? (image_[offset] | mask) : (image_[offset] & ~mask);
}

bool ExFatImageBuilder::isAllocated(std::uint32_t cluster) const {
    const std::uint32_t bit = cluster - 2;
    const auto offset = static_cast<std::size_t>(clusterOffset(bitmapCluster_) + bit / 8);
    return ((static_cast<std::uint8_t>(image_[offset]) >> (bit % 8)) & 1U) != 0;
}

std::uint32_t ExFatImageBuilder::allocatedClusters() const {
    std::uint32_t count = 0;
    for (std::uint32_t c = 2; c <= lastCluster(); ++c) {
        count += isAllocated(c) ? 1U : 0U;
    }
    return count;
}

void ExFatImageBuilder::skipClusters(std::uint32_t count) {
    cursor_ += count;
}

std::uint32_t ExFatImageBuilder::allocate() {
    return allocateRun(1).front();
}

std::vector<std::uint32_t> ExFatImageBuilder::allocateRun(std::size_t count) {
    for (std::uint32_t start = cursor_; start + count - 1 <= lastCluster(); ++start) {
        bool free = true;
        for (std::size_t i = 0; i < count && free; ++i) {
            free = !isAllocated(static_cast<std::uint32_t>(start + i));
        }
        if (free) {
            std::vector<std::uint32_t> run;
            for (std::size_t i = 0; i < count; ++i) {
                run.push_back(static_cast<std::uint32_t>(start + i));
                setAllocated(run.back(), true);
            }
            cursor_ = static_cast<std::uint32_t>(start + count);
            return run;
        }
    }
    throw std::runtime_error("test volume is full");
}

void ExFatImageBuilder::chain(const std::vector<std::uint32_t>& clusters) {
    for (std::size_t i = 0; i < clusters.size(); ++i) {
        setFat(clusters[i], i + 1 < clusters.size() ? clusters[i + 1] : kEndOfChain);
    }
}

void ExFatImageBuilder::writeData(const std::vector<std::uint32_t>& clusters, std::span<const std::byte> data) {
    for (std::size_t i = 0; i < clusters.size(); ++i) {
        const std::size_t begin = i * clusterSize_;
        const std::size_t count = std::min<std::size_t>(clusterSize_, data.size() - begin);
        std::memcpy(image_.data() + clusterOffset(clusters[i]), data.data() + begin, count);
    }
}

void ExFatImageBuilder::grow(Directory& directory) {
    const std::uint32_t last = directory.clusters.back();
    std::uint32_t added = 0;
    if (directory.contiguous && last + 1 <= lastCluster() && !isAllocated(last + 1)) {
        added = last + 1;
        setAllocated(added, true);
    } else {
        added = allocate();
        if (directory.contiguous) {
            // No longer one run: switch to a FAT chain.
            chain(directory.clusters);
            directory.contiguous = false;
        }
    }
    const auto offset = static_cast<std::ptrdiff_t>(clusterOffset(added));
    std::fill(image_.begin() + offset, image_.begin() + offset + clusterSize_, std::byte{0});
    directory.clusters.push_back(added);
    if (!directory.contiguous) {
        chain(directory.clusters);
    }
    updateDirectorySize(directory);
}

void ExFatImageBuilder::updateDirectorySize(const Directory& directory) {
    if (!directory.self) {
        return;  // the root has no Stream Extension
    }
    const std::span<std::byte> stream = slot(*directory.self, 1);
    const std::uint64_t length = static_cast<std::uint64_t>(directory.clusters.size()) * clusterSize_;
    stream[1] = static_cast<std::byte>(0x01 | (directory.contiguous ? 0x02 : 0x00));
    storeLe64(stream, 8, length);
    storeLe64(stream, 24, length);
    rechecksum(*directory.self);
}

std::span<std::byte> ExFatImageBuilder::directorySlot(Directory& directory, std::size_t index) {
    const std::size_t perCluster = clusterSize_ / kEntrySize;
    while (index / perCluster >= directory.clusters.size()) {
        grow(directory);
    }
    const std::uint64_t offset =
        clusterOffset(directory.clusters[index / perCluster]) + (index % perCluster) * kEntrySize;
    return std::span<std::byte>(image_).subspan(static_cast<std::size_t>(offset), kEntrySize);
}

std::span<std::byte> ExFatImageBuilder::directorySlot(std::uint32_t directory, std::size_t index) {
    return directorySlot(directories_.at(directory), index);
}

std::size_t ExFatImageBuilder::nextFreeSlot(std::uint32_t directory) const {
    return directories_.at(directory).nextSlot;
}

std::span<std::byte> ExFatImageBuilder::slot(const Entry& entry, std::size_t index) {
    return directorySlot(entry.directory, entry.firstSlot + index);
}

std::uint64_t ExFatImageBuilder::slotOffset(const Entry& entry, std::size_t index) const {
    const Directory& directory = directories_.at(entry.directory);
    const std::size_t perCluster = clusterSize_ / kEntrySize;
    const std::size_t at = entry.firstSlot + index;
    return clusterOffset(directory.clusters.at(at / perCluster)) + (at % perCluster) * kEntrySize;
}

void ExFatImageBuilder::rechecksum(const Entry& entry) {
    std::vector<std::byte> set;
    for (std::size_t i = 0; i < entry.slotCount; ++i) {
        const std::span<std::byte> e = slot(entry, i);
        set.insert(set.end(), e.begin(), e.end());
        set[i * kEntrySize] |= std::byte{0x80};  // the checksum covers the in-use entry types
    }
    storeLe16(slot(entry, 0), 2, setChecksum(set));
}

std::uint16_t ExFatImageBuilder::hashOf(std::u16string_view name) const {
    std::uint16_t hash = 0;
    for (const char16_t unit : name) {
        const char16_t upper = upcaseMap_[unit];
        for (const auto byte : {static_cast<std::uint8_t>(upper & 0xFF), static_cast<std::uint8_t>(upper >> 8)}) {
            hash = static_cast<std::uint16_t>(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + byte);
        }
    }
    return hash;
}

ExFatImageBuilder::Entry ExFatImageBuilder::writeEntrySet(std::uint32_t parent, std::string_view name,
                                                          std::uint16_t attributes, std::uint32_t firstCluster,
                                                          std::uint64_t dataLength, bool noFatChain,
                                                          std::size_t vendorExtensions) {
    const std::u16string units = utf8ToUtf16(name);
    if (units.empty() || units.size() > 255) {
        throw std::runtime_error("invalid exFAT name length");
    }
    const std::size_t nameEntries = (units.size() + kNameChars - 1) / kNameChars;
    Directory& directory = directories_.at(parent);

    Entry entry;
    entry.directory = parent;
    entry.firstSlot = directory.nextSlot;
    entry.slotCount = 2 + nameEntries + vendorExtensions;
    directory.nextSlot += entry.slotCount;

    const std::span<std::byte> file = directorySlot(directory, entry.firstSlot);
    file[0] = std::byte{0x85};
    file[1] = static_cast<std::byte>(entry.slotCount - 1);
    storeLe16(file, 4, attributes);
    storeLe32(file, 8, kTimestamp);
    storeLe32(file, 12, kTimestamp);
    storeLe32(file, 16, kTimestamp);
    file[20] = std::byte{kCreate10ms};
    file[22] = std::byte{kUtcPlus2};
    file[23] = std::byte{kUtcPlus2};
    file[24] = std::byte{kUtcPlus2};

    const std::span<std::byte> stream = directorySlot(directory, entry.firstSlot + 1);
    stream[0] = std::byte{0xC0};
    stream[1] = static_cast<std::byte>(0x01 | (noFatChain ? 0x02 : 0x00));
    stream[3] = static_cast<std::byte>(units.size());
    storeLe16(stream, 4, hashOf(units));
    storeLe64(stream, 8, dataLength);
    storeLe32(stream, 20, firstCluster);
    storeLe64(stream, 24, dataLength);

    for (std::size_t n = 0; n < nameEntries; ++n) {
        const std::span<std::byte> part = directorySlot(directory, entry.firstSlot + 2 + n);
        part[0] = std::byte{0xC1};
        for (std::size_t i = 0; i < kNameChars; ++i) {
            const std::size_t index = n * kNameChars + i;
            storeLe16(part, 2 + 2 * i, index < units.size() ? units[index] : char16_t{0});
        }
    }
    for (std::size_t v = 0; v < vendorExtensions; ++v) {
        const std::span<std::byte> vendor = directorySlot(directory, entry.firstSlot + 2 + nameEntries + v);
        vendor[0] = std::byte{0xE0};
        std::fill(vendor.begin() + 2, vendor.end(), std::byte{0x5A});
    }
    rechecksum(entry);
    return entry;
}

ExFatImageBuilder::Entry ExFatImageBuilder::addDirectory(std::uint32_t parent, std::string_view name) {
    const std::uint32_t cluster = allocate();
    const auto offset = static_cast<std::ptrdiff_t>(clusterOffset(cluster));
    std::fill(image_.begin() + offset, image_.begin() + offset + clusterSize_, std::byte{0});
    Entry entry = writeEntrySet(parent, name, kAttrDirectory, cluster, clusterSize_, true, 0);
    entry.clusters = {cluster};
    entry.isDirectory = true;
    entry.contiguous = true;
    Directory& directory = directories_[cluster];
    directory.clusters = {cluster};
    directory.self = entry;
    return entry;
}

ExFatImageBuilder::Entry ExFatImageBuilder::addFile(std::uint32_t parent, std::string_view name,
                                                    std::span<const std::byte> data, std::size_t vendorExtensions) {
    const std::size_t needed = data.empty() ? 0 : (data.size() - 1) / clusterSize_ + 1;
    std::vector<std::uint32_t> clusters = needed == 0 ? std::vector<std::uint32_t>{} : allocateRun(needed);
    writeData(clusters, data);
    Entry entry = writeEntrySet(parent, name, kAttrArchive, clusters.empty() ? 0 : clusters.front(), data.size(),
                                !clusters.empty(), vendorExtensions);
    entry.clusters = std::move(clusters);
    entry.contiguous = !entry.clusters.empty();
    return entry;
}

ExFatImageBuilder::Entry ExFatImageBuilder::addFileInClusters(std::uint32_t parent, std::string_view name,
                                                              std::span<const std::byte> data,
                                                              const std::vector<std::uint32_t>& clusters) {
    const std::size_t needed = data.empty() ? 0 : (data.size() - 1) / clusterSize_ + 1;
    if (clusters.size() != needed) {
        throw std::runtime_error("cluster list does not match the data size");
    }
    for (const std::uint32_t c : clusters) {
        if (c < 2 || c > lastCluster() || isAllocated(c)) {
            throw std::runtime_error("cluster is not free");
        }
        setAllocated(c, true);
    }
    chain(clusters);
    writeData(clusters, data);
    Entry entry = writeEntrySet(parent, name, kAttrArchive, clusters.empty() ? 0 : clusters.front(), data.size(),
                                false, 0);
    entry.clusters = clusters;
    return entry;
}

void ExFatImageBuilder::deleteEntry(const Entry& entry, bool freeClusters) {
    for (std::size_t i = 0; i < entry.slotCount; ++i) {
        slot(entry, i)[0] &= std::byte{0x7F};
    }
    if (!freeClusters) {
        return;
    }
    std::vector<std::uint32_t> clusters = entry.clusters;
    bool contiguous = entry.contiguous;
    if (entry.isDirectory) {
        const Directory& directory = directories_.at(entry.clusters.front());  // it may have grown
        clusters = directory.clusters;
        contiguous = directory.contiguous;
    }
    for (const std::uint32_t c : clusters) {
        setAllocated(c, false);
        if (options_.clearFatOnDelete && !contiguous) {
            setFat(c, 0);
        }
    }
}

void ExFatImageBuilder::writeBootRegion(std::uint32_t firstSector) {
    const std::span<std::byte> region =
        std::span<std::byte>(image_).subspan(static_cast<std::size_t>(firstSector) * bytesPerSector_,
                                             static_cast<std::size_t>(bytesPerSector_) * 12);
    std::fill(region.begin(), region.end(), std::byte{0});
    const std::span<std::byte> s = region.first(bytesPerSector_);
    s[0] = std::byte{0xEB};
    s[1] = std::byte{0x76};
    s[2] = std::byte{0x90};
    std::memcpy(s.data() + 3, "EXFAT   ", 8);
    storeLe64(s, 72, volumeSectors_);
    storeLe32(s, 80, options_.fatOffsetSectors);
    storeLe32(s, 84, fatLengthSectors_);
    storeLe32(s, 88, heapOffsetSectors_);
    storeLe32(s, 92, options_.clusterCount);
    storeLe32(s, 96, rootCluster_);
    storeLe32(s, 100, options_.volumeSerial);
    storeLe16(s, 104, 0x0100);  // revision 1.00
    s[108] = static_cast<std::byte>(options_.bytesPerSectorShift);
    s[109] = static_cast<std::byte>(options_.sectorsPerClusterShift);
    s[110] = static_cast<std::byte>(options_.fatCount);
    s[111] = std::byte{0x80};
    s[112] = static_cast<std::byte>(allocatedClusters() * 100ULL / options_.clusterCount);
    s[510] = std::byte{0x55};
    s[511] = std::byte{0xAA};
    for (std::uint32_t sector = 1; sector <= 8; ++sector) {
        storeLe32(region, (sector + 1) * bytesPerSector_ - 4, 0xAA550000);
    }
    rewriteBootChecksum(image_, bytesPerSector_, firstSector);
}

void ExFatImageBuilder::rewriteBootChecksum(std::span<std::byte> image, std::uint32_t bytesPerSector,
                                            std::uint32_t firstSector) {
    const std::span<std::byte> region = image.subspan(static_cast<std::size_t>(firstSector) * bytesPerSector,
                                                      static_cast<std::size_t>(bytesPerSector) * 12);
    const std::uint32_t sum = rotatingSum32(region.first(static_cast<std::size_t>(bytesPerSector) * 11), true);
    for (std::uint32_t offset = 0; offset < bytesPerSector; offset += 4) {
        storeLe32(region, 11 * bytesPerSector + offset, sum);
    }
}

std::vector<std::byte> ExFatImageBuilder::build() {
    writeBootRegion(0);
    writeBootRegion(12);
    return image_;
}

}  // namespace recovery::test
