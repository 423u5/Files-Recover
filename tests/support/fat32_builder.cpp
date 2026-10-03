#include "support/fat32_builder.hpp"

#include "filesystem/fat32/fat32_names.hpp"
#include "recovery/byte_order.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace recovery::test {

namespace {

constexpr std::uint32_t kEndOfChain = 0x0FFFFFFF;
constexpr std::uint16_t kDate = (44 << 9) | (5 << 5) | 17;    // 2024-05-17
constexpr std::uint16_t kTime = (13 << 11) | (45 << 5) | 15;  // 13:45:30
constexpr std::uint8_t kCreateHundredths = 50;                // +0.5 s

bool fitsShortName(std::string_view name) {
    const std::size_t dot = name.find('.');
    const std::string_view base = name.substr(0, dot);
    const std::string_view ext = dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3 || ext.find('.') != std::string_view::npos) {
        return false;
    }
    const auto valid = [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x80 && !(c >= 'a' && c <= 'z') && c != ' ' && filesystem::fat32::isValidShortNameByte(u);
    };
    return std::all_of(base.begin(), base.end(), valid) && std::all_of(ext.begin(), ext.end(), valid);
}

// Windows basis-name character mapping (simplified: non-ASCII becomes '_').
std::string basisPart(std::u16string_view part) {
    std::string out;
    for (const char16_t unit : part) {
        if (unit == u' ' || unit == u'.') {
            continue;
        }
        char c = '_';
        if (unit < 0x80) {
            c = static_cast<char>(unit);
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
            if (!filesystem::fat32::isValidShortNameByte(static_cast<std::uint8_t>(c))) {
                c = '_';
            }
        }
        out += c;
    }
    return out;
}

}  // namespace

std::u16string utf8ToUtf16(std::string_view text) {
    std::u16string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        char32_t cp = 0;
        std::size_t length = 1;
        if (b < 0x80) {
            cp = b;
        } else if ((b >> 5) == 0x6) {
            cp = b & 0x1F;
            length = 2;
        } else if ((b >> 4) == 0xE) {
            cp = b & 0x0F;
            length = 3;
        } else {
            cp = b & 0x07;
            length = 4;
        }
        for (std::size_t k = 1; k < length; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        i += length;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (cp >> 10));
            out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<char16_t>(cp);
        }
    }
    return out;
}

Fat32ImageBuilder::Fat32ImageBuilder(Fat32BuilderOptions options)
    : options_(std::move(options)),
      clusterSize_(static_cast<std::uint32_t>(options_.bytesPerSector) * options_.sectorsPerCluster),
      fatSectors_(static_cast<std::uint32_t>(
          ((static_cast<std::uint64_t>(options_.clusterCount) + 2) * 4 + options_.bytesPerSector - 1) /
          options_.bytesPerSector)) {
    const std::uint64_t totalSectors = options_.reservedSectors +
                                       static_cast<std::uint64_t>(options_.fatCount) * fatSectors_ +
                                       static_cast<std::uint64_t>(options_.clusterCount) * options_.sectorsPerCluster;
    image_.assign(static_cast<std::size_t>(totalSectors * options_.bytesPerSector), std::byte{0});

    writeBootSector(0);
    writeFsInfo(1);
    if (options_.reservedSectors > 7) {
        writeBootSector(6);
        writeFsInfo(7);
    }
    setFat(0, 0x0FFFFF00 | 0xF8);
    setFat(1, kEndOfChain);
    setFat(root(), kEndOfChain);

    Directory& rootDirectory = directories_[root()];
    rootDirectory.clusters = {root()};
    if (!options_.label.empty()) {
        const std::span<std::byte> label = slot(rootDirectory, rootDirectory.nextSlot++);
        std::string padded = options_.label;
        padded.resize(11, ' ');
        std::memcpy(label.data(), padded.data(), 11);
        label[11] = std::byte{0x08};
        storeLe16(label, 22, kTime);
        storeLe16(label, 24, kDate);
    }
}

void Fat32ImageBuilder::writeBootSector(std::uint32_t sector) {
    const std::span<std::byte> s =
        std::span<std::byte>(image_).subspan(static_cast<std::size_t>(sector) * options_.bytesPerSector, 512);
    s[0] = std::byte{0xEB};
    s[1] = std::byte{0x58};
    s[2] = std::byte{0x90};
    std::memcpy(s.data() + 3, "MSWIN4.1", 8);
    storeLe16(s, 11, options_.bytesPerSector);
    s[13] = static_cast<std::byte>(options_.sectorsPerCluster);
    storeLe16(s, 14, options_.reservedSectors);
    s[16] = static_cast<std::byte>(options_.fatCount);
    s[21] = std::byte{0xF8};
    storeLe16(s, 24, 63);
    storeLe16(s, 26, 255);
    storeLe32(s, 32, static_cast<std::uint32_t>(image_.size() / options_.bytesPerSector));
    storeLe32(s, 36, fatSectors_);
    storeLe32(s, 44, root());
    storeLe16(s, 48, 1);
    storeLe16(s, 50, options_.reservedSectors > 7 ? 6 : 0);
    s[64] = std::byte{0x80};
    s[66] = std::byte{0x29};
    storeLe32(s, 67, options_.volumeId);
    std::string label = options_.label.empty() ? "NO NAME" : options_.label;
    label.resize(11, ' ');
    std::memcpy(s.data() + 71, label.data(), 11);
    std::memcpy(s.data() + 82, "FAT32   ", 8);
    s[510] = std::byte{0x55};
    s[511] = std::byte{0xAA};
}

void Fat32ImageBuilder::writeFsInfo(std::uint32_t sector) {
    const std::span<std::byte> s =
        std::span<std::byte>(image_).subspan(static_cast<std::size_t>(sector) * options_.bytesPerSector, 512);
    storeLe32(s, 0, 0x41615252);
    storeLe32(s, 484, 0x61417272);
    storeLe32(s, 488, options_.clusterCount - usedClusters());
    storeLe32(s, 492, cursor_);
    storeLe32(s, 508, 0xAA550000);
}

std::uint64_t Fat32ImageBuilder::fatOffset(std::uint32_t copy) const {
    return (static_cast<std::uint64_t>(options_.reservedSectors) + static_cast<std::uint64_t>(copy) * fatSectors_) *
           options_.bytesPerSector;
}

std::uint64_t Fat32ImageBuilder::clusterOffset(std::uint32_t cluster) const {
    return fatOffset(options_.fatCount) + static_cast<std::uint64_t>(cluster - 2) * clusterSize_;
}

void Fat32ImageBuilder::setFatInCopy(std::uint32_t copy, std::uint32_t cluster, std::uint32_t value) {
    storeLe32(image_, static_cast<std::size_t>(fatOffset(copy) + static_cast<std::uint64_t>(cluster) * 4), value);
}

void Fat32ImageBuilder::setFat(std::uint32_t cluster, std::uint32_t value) {
    for (std::uint32_t copy = 0; copy < options_.fatCount; ++copy) {
        setFatInCopy(copy, cluster, value);
    }
}

std::uint32_t Fat32ImageBuilder::fat(std::uint32_t cluster, std::uint32_t copy) const {
    return loadLe32(image_, static_cast<std::size_t>(fatOffset(copy) + static_cast<std::uint64_t>(cluster) * 4));
}

std::uint32_t Fat32ImageBuilder::usedClusters() const {
    std::uint32_t used = 0;
    for (std::uint32_t c = 2; c <= lastCluster(); ++c) {
        used += fat(c) != 0 ? 1U : 0U;
    }
    return used;
}

void Fat32ImageBuilder::skipClusters(std::uint32_t count) {
    cursor_ += count;
}

std::uint32_t Fat32ImageBuilder::allocate() {
    for (std::uint32_t c = std::max<std::uint32_t>(cursor_, 3); c <= lastCluster(); ++c) {
        if (fat(c) == 0) {
            setFat(c, kEndOfChain);
            cursor_ = c + 1;
            return c;
        }
    }
    throw std::runtime_error("test volume is full");
}

std::span<std::byte> Fat32ImageBuilder::slot(Directory& directory, std::size_t index) {
    const std::size_t perCluster = clusterSize_ / 32;
    while (index / perCluster >= directory.clusters.size()) {
        const std::uint32_t added = allocate();
        setFat(directory.clusters.back(), added);
        const auto offset = static_cast<std::ptrdiff_t>(clusterOffset(added));
        std::fill(image_.begin() + offset, image_.begin() + offset + clusterSize_, std::byte{0});
        directory.clusters.push_back(added);
    }
    const std::uint64_t offset = clusterOffset(directory.clusters[index / perCluster]) + (index % perCluster) * 32;
    return std::span<std::byte>(image_).subspan(static_cast<std::size_t>(offset), 32);
}

std::array<std::byte, 11> Fat32ImageBuilder::shortNameFor(Directory& directory, std::string_view name,
                                                          bool& needsLongName) {
    std::array<std::byte, 11> result{};
    std::fill(result.begin(), result.end(), std::byte{' '});
    std::string base;
    std::string ext;
    if (fitsShortName(name)) {
        needsLongName = false;
        const std::size_t dot = name.find('.');
        base = std::string(name.substr(0, dot));
        ext = dot == std::string_view::npos ? std::string{} : std::string(name.substr(dot + 1));
    } else {
        needsLongName = true;
        const std::u16string wide = utf8ToUtf16(name);
        const std::size_t dot = wide.rfind(u'.');
        const bool hasExt = dot != std::u16string::npos && dot > 0;
        std::string basis = basisPart(hasExt ? std::u16string_view(wide).substr(0, dot) : std::u16string_view(wide));
        ext = hasExt ? basisPart(std::u16string_view(wide).substr(dot + 1)).substr(0, 3) : std::string{};
        if (basis.empty()) {
            basis = "_";
        }
        for (int n = 1;; ++n) {
            const std::string tail = "~" + std::to_string(n);
            std::string candidate = basis.substr(0, 8 - tail.size()) + tail;
            if (!directory.shortNames.contains(candidate + "." + ext)) {
                base = candidate;
                break;
            }
        }
    }
    if (!directory.shortNames.insert(base + "." + ext).second) {
        throw std::runtime_error("duplicate 8.3 name in test directory");
    }
    std::memcpy(result.data(), base.data(), base.size());
    std::memcpy(result.data() + 8, ext.data(), ext.size());
    return result;
}

Fat32ImageBuilder::Entry Fat32ImageBuilder::writeEntrySet(std::uint32_t parent, std::string_view name,
                                                          std::uint8_t attributes, std::uint32_t cluster,
                                                          std::uint32_t size) {
    Directory& directory = directories_.at(parent);
    bool needsLongName = false;
    const std::array<std::byte, 11> shortName = shortNameFor(directory, name, needsLongName);

    Entry entry;
    entry.directory = parent;
    entry.firstSlot = directory.nextSlot;
    if (needsLongName) {
        const std::u16string wide = utf8ToUtf16(name);
        if (wide.size() > 255) {
            throw std::runtime_error("long name too long");
        }
        const std::size_t parts = (wide.size() + 12) / 13;
        const std::uint8_t checksum = filesystem::fat32::longNameChecksum(shortName);
        static constexpr std::array<std::size_t, 13> kOffsets = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (std::size_t part = parts; part >= 1; --part) {
            const std::span<std::byte> s = slot(directory, directory.nextSlot++);
            s[0] = static_cast<std::byte>(part | (part == parts ? 0x40 : 0));
            s[11] = std::byte{0x0F};
            s[13] = static_cast<std::byte>(checksum);
            for (std::size_t i = 0; i < 13; ++i) {
                const std::size_t index = (part - 1) * 13 + i;
                std::uint16_t unit = 0xFFFF;
                if (index < wide.size()) {
                    unit = wide[index];
                } else if (index == wide.size()) {
                    unit = 0x0000;
                }
                storeLe16(s, kOffsets[i], unit);
            }
        }
    }
    entry.shortSlot = directory.nextSlot;
    const std::span<std::byte> s = slot(directory, directory.nextSlot++);
    std::memcpy(s.data(), shortName.data(), 11);
    s[11] = static_cast<std::byte>(attributes);
    s[13] = static_cast<std::byte>(kCreateHundredths);
    storeLe16(s, 14, kTime);
    storeLe16(s, 16, kDate);
    storeLe16(s, 18, kDate);
    storeLe16(s, 20, static_cast<std::uint16_t>(cluster >> 16));
    storeLe16(s, 22, kTime);
    storeLe16(s, 24, kDate);
    storeLe16(s, 26, static_cast<std::uint16_t>(cluster & 0xFFFF));
    storeLe32(s, 28, size);
    return entry;
}

Fat32ImageBuilder::Entry Fat32ImageBuilder::addDirectory(std::uint32_t parent, std::string_view name) {
    const std::uint32_t cluster = allocate();
    const auto offset = static_cast<std::ptrdiff_t>(clusterOffset(cluster));
    std::fill(image_.begin() + offset, image_.begin() + offset + clusterSize_, std::byte{0});
    Directory& directory = directories_[cluster];
    directory.clusters = {cluster};

    const auto dotEntry = [&](std::size_t index, const char* dotName, std::uint32_t target) {
        const std::span<std::byte> s = slot(directory, index);
        std::memcpy(s.data(), dotName, 11);
        s[11] = std::byte{0x10};
        storeLe16(s, 20, static_cast<std::uint16_t>(target >> 16));
        storeLe16(s, 26, static_cast<std::uint16_t>(target & 0xFFFF));
        storeLe16(s, 22, kTime);
        storeLe16(s, 24, kDate);
    };
    dotEntry(0, ".          ", cluster);
    dotEntry(1, "..         ", parent == root() ? 0 : parent);
    directory.nextSlot = 2;

    Entry entry = writeEntrySet(parent, name, 0x10, cluster, 0);
    entry.clusters = {cluster};
    entry.isDirectory = true;
    return entry;
}

void Fat32ImageBuilder::writeData(const std::vector<std::uint32_t>& clusters, std::span<const std::byte> data) {
    for (std::size_t i = 0; i < clusters.size(); ++i) {
        const std::size_t begin = i * clusterSize_;
        const std::size_t count = std::min<std::size_t>(clusterSize_, data.size() - begin);
        std::memcpy(image_.data() + clusterOffset(clusters[i]), data.data() + begin, count);
    }
}

Fat32ImageBuilder::Entry Fat32ImageBuilder::addFile(std::uint32_t parent, std::string_view name,
                                                    std::span<const std::byte> data) {
    std::vector<std::uint32_t> clusters;
    const std::size_t needed = data.empty() ? 0 : (data.size() - 1) / clusterSize_ + 1;
    for (std::size_t i = 0; i < needed; ++i) {
        clusters.push_back(allocate());
    }
    for (std::size_t i = 0; i + 1 < clusters.size(); ++i) {
        setFat(clusters[i], clusters[i + 1]);
    }
    writeData(clusters, data);
    Entry entry = writeEntrySet(parent, name, 0x20, clusters.empty() ? 0 : clusters.front(),
                                static_cast<std::uint32_t>(data.size()));
    entry.clusters = std::move(clusters);
    return entry;
}

Fat32ImageBuilder::Entry Fat32ImageBuilder::addFileInClusters(std::uint32_t parent, std::string_view name,
                                                              std::span<const std::byte> data,
                                                              const std::vector<std::uint32_t>& clusters) {
    const std::size_t needed = data.empty() ? 0 : (data.size() - 1) / clusterSize_ + 1;
    if (clusters.size() != needed) {
        throw std::runtime_error("cluster list does not match the data size");
    }
    for (const std::uint32_t c : clusters) {
        if (c < 3 || c > lastCluster() || fat(c) != 0) {
            throw std::runtime_error("cluster is not free");
        }
        setFat(c, kEndOfChain);
    }
    for (std::size_t i = 0; i + 1 < clusters.size(); ++i) {
        setFat(clusters[i], clusters[i + 1]);
    }
    writeData(clusters, data);
    Entry entry = writeEntrySet(parent, name, 0x20, clusters.empty() ? 0 : clusters.front(),
                                static_cast<std::uint32_t>(data.size()));
    entry.clusters = clusters;
    return entry;
}

void Fat32ImageBuilder::deleteEntry(const Entry& entry, bool freeClusters) {
    Directory& directory = directories_.at(entry.directory);
    for (std::size_t index = entry.firstSlot; index <= entry.shortSlot; ++index) {
        slot(directory, index)[0] = std::byte{0xE5};
    }
    if (freeClusters) {
        for (const std::uint32_t c : entry.clusters) {
            setFat(c, 0);
        }
    }
}

std::span<std::byte> Fat32ImageBuilder::shortEntry(const Entry& entry) {
    return slot(directories_.at(entry.directory), entry.shortSlot);
}

std::uint64_t Fat32ImageBuilder::shortEntryOffset(const Entry& entry) const {
    const Directory& directory = directories_.at(entry.directory);
    const std::size_t perCluster = clusterSize_ / 32;
    return clusterOffset(directory.clusters.at(entry.shortSlot / perCluster)) + (entry.shortSlot % perCluster) * 32;
}

std::vector<std::byte> Fat32ImageBuilder::build() {
    writeFsInfo(1);
    if (options_.reservedSectors > 7) {
        writeFsInfo(7);
    }
    return image_;
}

std::vector<std::byte> readExtents(storage::IStorageSource& volume, const std::vector<filesystem::Extent>& extents) {
    std::vector<std::byte> out;
    for (const filesystem::Extent& extent : extents) {
        std::vector<std::byte> piece(static_cast<std::size_t>(extent.length));
        if (!volume.readExact(ByteOffset{extent.offset}, piece).ok()) {
            throw std::runtime_error("extent unreadable");
        }
        out.insert(out.end(), piece.begin(), piece.end());
    }
    return out;
}

}  // namespace recovery::test
