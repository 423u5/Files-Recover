#include "support/ole2_builders.hpp"

#include "support/test_files.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace recovery::test {

namespace {

constexpr std::uint32_t kFree = 0xFFFFFFFF;
constexpr std::uint32_t kEndOfChain = 0xFFFFFFFE;
constexpr std::uint32_t kFatSector = 0xFFFFFFFD;
constexpr std::uint32_t kDifatSector = 0xFFFFFFFC;
constexpr std::uint32_t kNoStream = 0xFFFFFFFF;
constexpr std::size_t kMini = 64;
constexpr std::size_t kCutoff = 4096;

void put16(std::vector<std::byte>& out, std::size_t at, std::uint32_t value) {
    out[at] = static_cast<std::byte>(value & 0xFF);
    out[at + 1] = static_cast<std::byte>((value >> 8) & 0xFF);
}

void put32(std::vector<std::byte>& out, std::size_t at, std::uint32_t value) {
    put16(out, at, value & 0xFFFF);
    put16(out, at + 2, value >> 16);
}

struct Node {
    std::string name;
    std::uint8_t type = 2;
    std::uint32_t left = kNoStream;
    std::uint32_t right = kNoStream;
    std::uint32_t child = kNoStream;
    std::uint8_t color = 1;  // black
    std::uint32_t start = kEndOfChain;
    std::uint64_t size = 0;
    std::array<std::uint8_t, 16> clsid{};
};

// [MS-CFB] 2.6.4: shorter names first, then by their upper-case letters.
bool lessName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return a.size() < b.size();
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const int x = std::toupper(static_cast<unsigned char>(a[i]));
        const int y = std::toupper(static_cast<unsigned char>(b[i]));
        if (x != y) {
            return x < y;
        }
    }
    return false;
}

// A balanced tree of `sorted` (ids into `nodes`), its deepest level red: a
// red-black tree whose every path holds as many black nodes.
std::uint32_t tree(std::vector<Node>& nodes, const std::vector<std::uint32_t>& sorted, std::size_t lo, std::size_t hi,
                   int depth, int deepest) {
    if (lo >= hi) {
        return kNoStream;
    }
    const std::size_t mid = (lo + hi) / 2;
    const std::uint32_t id = sorted[mid];
    nodes[id].left = tree(nodes, sorted, lo, mid, depth + 1, deepest);
    nodes[id].right = tree(nodes, sorted, mid + 1, hi, depth + 1, deepest);
    nodes[id].color = depth == deepest && deepest > 0 ? 0 : 1;
    return id;
}

std::uint32_t treeOf(std::vector<Node>& nodes, std::vector<std::uint32_t> ids) {
    std::sort(ids.begin(), ids.end(), [&](std::uint32_t a, std::uint32_t b) { return lessName(nodes[a].name, nodes[b].name); });
    int deepest = 0;
    while ((std::size_t{2} << deepest) - 1 < ids.size()) {
        ++deepest;
    }
    return tree(nodes, ids, 0, ids.size(), 0, deepest);
}

std::size_t sectorsFor(std::size_t bytes, std::size_t sector) {
    return (bytes + sector - 1) / sector;
}

}  // namespace

std::vector<std::byte> compoundFile(const std::vector<Ole2Stream>& streams, const Ole2Options& options) {
    const std::size_t S = options.version == 4 ? 4096 : 512;
    const std::size_t perFat = S / 4;

    // The directory: the root, the storages, the streams.
    std::vector<Node> nodes(1);
    nodes[0].name = "Root Entry";
    nodes[0].type = 5;
    nodes[0].clsid = options.rootClass;
    std::map<std::string, std::uint32_t> storages;
    std::map<std::uint32_t, std::vector<std::uint32_t>> children;
    std::vector<std::uint32_t> streamIds;
    for (const Ole2Stream& stream : streams) {
        std::uint32_t parent = 0;
        if (!stream.storage.empty()) {
            auto found = storages.find(stream.storage);
            if (found == storages.end()) {
                Node storage;
                storage.name = stream.storage;
                storage.type = 1;
                storage.start = 0;
                nodes.push_back(storage);
                const auto id = static_cast<std::uint32_t>(nodes.size() - 1);
                children[0].push_back(id);
                found = storages.emplace(stream.storage, id).first;
            }
            parent = found->second;
        }
        Node node;
        node.name = stream.name;
        node.size = stream.data.size();
        nodes.push_back(node);
        const auto id = static_cast<std::uint32_t>(nodes.size() - 1);
        children[parent].push_back(id);
        streamIds.push_back(id);
    }
    for (auto& [parent, ids] : children) {
        nodes[parent].child = treeOf(nodes, ids);
    }

    // Sectors: large streams, the mini stream, the mini FAT, the directory.
    std::vector<std::vector<std::byte>> data;  // per sector
    std::vector<std::uint32_t> fat;
    const auto chainOf = [&](std::size_t first, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            fat[first + i] = i + 1 == count ? kEndOfChain : static_cast<std::uint32_t>(first + i + 1);
        }
    };
    std::vector<std::byte> mini;
    std::vector<std::uint32_t> miniFat;
    std::vector<std::pair<std::size_t, std::size_t>> large;  // (stream index, first sector)
    std::size_t next = 0;
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const std::vector<std::byte>& bytes = streams[i].data;
        Node& node = nodes[streamIds[i]];
        if (bytes.empty()) {
            continue;
        }
        if (bytes.size() >= kCutoff) {
            node.start = static_cast<std::uint32_t>(next);
            large.emplace_back(i, next);
            next += sectorsFor(bytes.size(), S);
        } else {
            const std::size_t first = mini.size() / kMini;
            const std::size_t count = sectorsFor(bytes.size(), kMini);
            node.start = static_cast<std::uint32_t>(first);
            mini.insert(mini.end(), bytes.begin(), bytes.end());
            mini.resize((first + count) * kMini, std::byte{0});
            for (std::size_t k = 0; k < count; ++k) {
                miniFat.push_back(k + 1 == count ? kEndOfChain : static_cast<std::uint32_t>(first + k + 1));
            }
        }
    }
    const std::size_t miniStreamFirst = next;
    const std::size_t miniStreamSectors = sectorsFor(mini.size(), S);
    next += miniStreamSectors;
    if (!mini.empty()) {
        nodes[0].start = static_cast<std::uint32_t>(miniStreamFirst);
        nodes[0].size = mini.size();
    }
    const std::size_t miniFatFirst = next;
    const std::size_t miniFatSectors = sectorsFor(miniFat.size() * 4, S);
    next += miniFatSectors;
    const std::size_t directoryFirst = next;
    const std::size_t directorySectors = sectorsFor(nodes.size() * 128, S);
    next += directorySectors;
    // The FAT and the DIFAT cover every sector, their own too.
    std::size_t fatSectors = 1;
    std::size_t difatSectors = 0;
    for (;;) {
        const std::size_t total = next + fatSectors + difatSectors + options.trailingFreeSectors;
        const std::size_t neededFat = sectorsFor(total, perFat);
        const std::size_t neededDifat = neededFat > 109 ? sectorsFor(neededFat - 109, perFat - 1) : 0;
        if (neededFat == fatSectors && neededDifat == difatSectors) {
            break;
        }
        fatSectors = neededFat;
        difatSectors = neededDifat;
    }
    const std::size_t fatFirst = next;
    const std::size_t difatFirst = fatFirst + fatSectors;
    const std::size_t used = difatFirst + difatSectors;
    fat.assign(fatSectors * perFat, kFree);
    for (const auto& [i, first] : large) {
        chainOf(first, sectorsFor(streams[i].data.size(), S));
    }
    if (miniStreamSectors > 0) {
        chainOf(miniStreamFirst, miniStreamSectors);
    }
    if (miniFatSectors > 0) {
        chainOf(miniFatFirst, miniFatSectors);
    }
    chainOf(directoryFirst, directorySectors);
    for (std::size_t k = 0; k < fatSectors; ++k) {
        fat[fatFirst + k] = kFatSector;
    }
    for (std::size_t k = 0; k < difatSectors; ++k) {
        fat[difatFirst + k] = kDifatSector;
    }

    const std::size_t sectors = used + options.trailingFreeSectors;
    std::vector<std::byte> out((sectors + 1) * S, std::byte{0});
    const auto at = [&](std::size_t sector) { return (sector + 1) * S; };
    for (const auto& [i, first] : large) {
        std::copy(streams[i].data.begin(), streams[i].data.end(), out.begin() + static_cast<std::ptrdiff_t>(at(first)));
    }
    std::copy(mini.begin(), mini.end(), out.begin() + static_cast<std::ptrdiff_t>(at(miniStreamFirst)));
    for (std::size_t k = 0; k < miniFat.size(); ++k) {
        put32(out, at(miniFatFirst) + 4 * k, miniFat[k]);
    }
    for (std::size_t k = miniFat.size(); k < miniFatSectors * perFat; ++k) {
        put32(out, at(miniFatFirst) + 4 * k, kFree);
    }
    for (std::size_t id = 0; id < directorySectors * (S / 128); ++id) {
        const std::size_t base = at(directoryFirst) + 128 * id;
        if (id >= nodes.size()) {
            put32(out, base + 0x44, kNoStream);
            put32(out, base + 0x48, kNoStream);
            put32(out, base + 0x4C, kNoStream);
            continue;
        }
        const Node& node = nodes[id];
        const std::size_t chars = std::min<std::size_t>(node.name.size(), 31);
        for (std::size_t c = 0; c < chars; ++c) {
            put16(out, base + 2 * c, static_cast<std::uint8_t>(node.name[c]));
        }
        put16(out, base + 0x40, static_cast<std::uint32_t>((chars + 1) * 2));
        out[base + 0x42] = static_cast<std::byte>(node.type);
        out[base + 0x43] = static_cast<std::byte>(node.color);
        put32(out, base + 0x44, node.left);
        put32(out, base + 0x48, node.right);
        put32(out, base + 0x4C, node.child);
        for (std::size_t b = 0; b < 16; ++b) {
            out[base + 0x50 + b] = static_cast<std::byte>(node.clsid[b]);
        }
        put32(out, base + 0x74, node.start);
        put32(out, base + 0x78, static_cast<std::uint32_t>(node.size & 0xFFFFFFFF));
        put32(out, base + 0x7C, options.version == 4 ? static_cast<std::uint32_t>(node.size >> 32) : 0);
    }
    for (std::size_t k = 0; k < fat.size(); ++k) {
        put32(out, at(fatFirst) + 4 * k, fat[k]);
    }
    // The DIFAT: the header's 109 FAT sectors, then DIFAT sectors.
    for (std::size_t d = 0; d < difatSectors; ++d) {
        const std::size_t base = at(difatFirst + d);
        for (std::size_t k = 0; k + 1 < perFat; ++k) {
            const std::size_t fatIndex = 109 + d * (perFat - 1) + k;
            put32(out, base + 4 * k, fatIndex < fatSectors ? static_cast<std::uint32_t>(fatFirst + fatIndex) : kFree);
        }
        put32(out, base + 4 * (perFat - 1),
              d + 1 == difatSectors ? kEndOfChain : static_cast<std::uint32_t>(difatFirst + d + 1));
    }

    // The header.
    static constexpr std::array<std::uint8_t, 8> kSignature = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    for (std::size_t i = 0; i < kSignature.size(); ++i) {
        out[i] = static_cast<std::byte>(kSignature[i]);
    }
    put16(out, 0x18, 0x003E);
    put16(out, 0x1A, static_cast<std::uint32_t>(options.version));
    put16(out, 0x1C, 0xFFFE);
    put16(out, 0x1E, options.version == 4 ? 12 : 9);
    put16(out, 0x20, 6);
    put32(out, 0x28, options.version == 4 ? static_cast<std::uint32_t>(directorySectors) : 0);
    put32(out, 0x2C, static_cast<std::uint32_t>(fatSectors));
    put32(out, 0x30, static_cast<std::uint32_t>(directoryFirst));
    put32(out, 0x38, 4096);
    put32(out, 0x3C, miniFatSectors > 0 ? static_cast<std::uint32_t>(miniFatFirst) : kEndOfChain);
    put32(out, 0x40, static_cast<std::uint32_t>(miniFatSectors));
    put32(out, 0x44, difatSectors > 0 ? static_cast<std::uint32_t>(difatFirst) : kEndOfChain);
    put32(out, 0x48, static_cast<std::uint32_t>(difatSectors));
    for (std::size_t k = 0; k < 109; ++k) {
        put32(out, 0x4C + 4 * k, k < fatSectors ? static_cast<std::uint32_t>(fatFirst + k) : kFree);
    }
    return out;
}

std::vector<std::byte> wordDocument(std::size_t textBytes, std::uint64_t seed) {
    return compoundFile({
        {"", "WordDocument", makePattern(textBytes, seed)},
        {"", "1Table", makePattern(5000, seed + 100)},
        {"", "\x05SummaryInformation", makePattern(400, seed + 200)},
        {"", "\x05" "DocumentSummaryInformation", makePattern(300, seed + 300)},
        {"", "\x01" "CompObj", makePattern(100, seed + 400)},
    });
}

std::vector<std::byte> excelWorkbook(std::size_t bytes, std::uint64_t seed) {
    return compoundFile({
        {"", "Workbook", makePattern(bytes, seed)},
        {"", "\x05SummaryInformation", makePattern(400, seed + 200)},
        {"", "\x05" "DocumentSummaryInformation", makePattern(300, seed + 300)},
    });
}

std::vector<std::byte> powerPointPresentation(std::size_t bytes, std::uint64_t seed) {
    return compoundFile({
        {"", "PowerPoint Document", makePattern(bytes, seed)},
        {"", "Current User", makePattern(60, seed + 100)},
        {"", "Pictures", makePattern(6000, seed + 200)},
        {"", "\x05SummaryInformation", makePattern(400, seed + 300)},
    });
}

std::vector<std::byte> outlookMessage(std::uint64_t seed) {
    return compoundFile({
        {"", "__properties_version1.0", makePattern(200, seed)},
        {"", "__substg1.0_0037001F", makePattern(64, seed + 100)},
        {"", "__substg1.0_1000001F", makePattern(5000, seed + 200)},
        {"__nameid_version1.0", "__substg1.0_00020102", makePattern(48, seed + 300)},
    });
}

}  // namespace recovery::test
