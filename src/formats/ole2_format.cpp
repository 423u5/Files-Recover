#include "formats/ole2_format.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"
#include "structure_walk.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recovery::formats {

namespace {

using carving::EndDetection;
using carving::HeaderCheck;
using carving::IContentReader;
using carving::ValidationResult;
using carving::ValidationStatus;
using detail::Depth;
using detail::Walk;
using detail::WalkStatus;

constexpr std::array<std::uint8_t, 8> kSignature = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

// Sector numbers and markers ([MS-CFB] 2.1).
constexpr std::uint32_t kMaxRegular = 0xFFFFFFFA;
constexpr std::uint32_t kDifatSector = 0xFFFFFFFC;
constexpr std::uint32_t kFatSector = 0xFFFFFFFD;
constexpr std::uint32_t kEndOfChain = 0xFFFFFFFE;
constexpr std::uint32_t kFree = 0xFFFFFFFF;
constexpr std::uint32_t kNoStream = 0xFFFFFFFF;

constexpr std::size_t kHeaderBytes = 512;
// FAT sector numbers in the header ([MS-CFB] 2.2).
constexpr std::size_t kHeaderDifat = 109;
constexpr std::size_t kEntryBytes = 128;
constexpr std::uint64_t kMiniSector = 64;
// Streams shorter than this are in the mini stream.
constexpr std::uint64_t kCutoff = 4096;

// Directory entry types ([MS-CFB] 2.6.1).
constexpr std::uint8_t kUnallocated = 0;
constexpr std::uint8_t kStorage = 1;
constexpr std::uint8_t kStream = 2;
constexpr std::uint8_t kRoot = 5;

// The class of a Windows Installer database, patch and transform:
// {000C1084-0000-0000-C000-000000000046} and the others, as stored.
constexpr std::array<std::uint8_t, 16> installerClass(std::uint8_t kind) {
    return {kind, 0x10, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
}

std::string count(std::uint64_t n, std::string_view one, std::string_view many) {
    return std::to_string(n) + " " + std::string(n == 1 ? one : many);
}

struct Header {
    std::uint16_t major = 0;
    std::uint32_t sectorSize = 0;
    std::uint32_t directorySectors = 0;
    std::uint32_t fatSectors = 0;
    std::uint32_t firstDirectory = 0;
    std::uint32_t firstMiniFat = 0;
    std::uint32_t miniFatSectors = 0;
    std::uint32_t firstDifat = 0;
    std::uint32_t difatSectors = 0;
    std::array<std::uint32_t, kHeaderDifat> difat{};
};

// Reads the header from its first 512 bytes; the reason when it is none.
std::optional<std::string> readHeader(std::span<const std::byte> bytes, Header& out) {
    if (bytes.size() < kHeaderBytes) {
        return "the data ends inside the header";
    }
    for (std::size_t i = 0; i < kSignature.size(); ++i) {
        if (bytes[i] != static_cast<std::byte>(kSignature[i])) {
            return "no compound file signature";
        }
    }
    if (loadLe16(bytes, 0x1C) != 0xFFFE) {
        return "a byte order other than little-endian";
    }
    out.major = loadLe16(bytes, 0x1A);
    const std::uint16_t shift = loadLe16(bytes, 0x1E);
    if (!((out.major == 3 && shift == 9) || (out.major == 4 && shift == 12))) {
        return "version " + std::to_string(out.major) + " with sectors of 2^" + std::to_string(shift) + " bytes";
    }
    if (loadLe16(bytes, 0x20) != 6) {
        return "mini sectors other than 64 bytes";
    }
    if (loadLe32(bytes, 0x38) != kCutoff) {
        return "a mini stream cutoff other than 4096";
    }
    out.sectorSize = std::uint32_t{1} << shift;
    out.directorySectors = loadLe32(bytes, 0x28);
    if (out.major == 3 && out.directorySectors != 0) {
        return "a version 3 file that counts its directory sectors";
    }
    out.fatSectors = loadLe32(bytes, 0x2C);
    out.firstDirectory = loadLe32(bytes, 0x30);
    out.firstMiniFat = loadLe32(bytes, 0x3C);
    out.miniFatSectors = loadLe32(bytes, 0x40);
    out.firstDifat = loadLe32(bytes, 0x44);
    out.difatSectors = loadLe32(bytes, 0x48);
    if (out.fatSectors == 0) {
        return "no FAT sector";
    }
    for (std::size_t i = 0; i < kHeaderDifat; ++i) {
        out.difat[i] = loadLe32(bytes, 0x4C + 4 * i);
    }
    return std::nullopt;
}

struct Entry {
    std::uint8_t type = kUnallocated;
    std::uint32_t left = kNoStream;
    std::uint32_t right = kNoStream;
    std::uint32_t child = kNoStream;
    std::uint32_t start = 0;
    std::uint64_t size = 0;
    // The name's length field (bytes, with the terminating NUL).
    std::uint16_t nameLength = 0;
    // The name as ASCII (other characters as '?'), and the class (root only).
    std::string name;
    std::array<std::byte, 16> clsid{};
};

Entry readEntry(std::span<const std::byte> bytes, std::uint16_t major) {
    Entry entry;
    entry.nameLength = loadLe16(bytes, 0x40);
    const std::size_t characters = std::min<std::size_t>(entry.nameLength / 2, 32);
    for (std::size_t i = 0; i < characters; ++i) {
        const std::uint16_t c = loadLe16(bytes, 2 * i);
        if (c == 0) {
            break;
        }
        entry.name += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?';
    }
    entry.type = loadU8(bytes, 0x42);
    entry.left = loadLe32(bytes, 0x44);
    entry.right = loadLe32(bytes, 0x48);
    entry.child = loadLe32(bytes, 0x4C);
    std::copy_n(bytes.begin() + 0x50, entry.clsid.size(), entry.clsid.begin());
    entry.start = loadLe32(bytes, 0x74);
    // Version 3 files keep a stream's size in the low 32 bits; older writers
    // left the high ones uninitialized ([MS-CFB] 2.6.3).
    entry.size = major == 3 ? loadLe32(bytes, 0x78) : loadLe64(bytes, 0x78);
    return entry;
}

// Walks a compound file: its header, DIFAT and FAT, which give its end, and
// its directory. See structure_walk.hpp for what a walk reports.
// Depth::Full also checks every directory entry and the chain of every
// stream, the mini stream and the mini FAT.
class Ole2Walker {
public:
    Ole2Walker(IContentReader& content, Depth depth, const Ole2Limits& limits) noexcept
        : content_(content), depth_(depth), limits_(limits) {}

    Result<Walk> run();

    [[nodiscard]] const Walk& walk() const noexcept { return walk_; }
    // After a complete walk of a file whose length is known (validation):
    // the sectors from the last one in use to the end of the content are the
    // file's when its FAT covers them, as free sectors (writers keep the
    // sectors a stream gave up, and grow a file ahead of its streams).
    void takeFreeSectors();
    // What the root's entries say the file is (empty when they say nothing).
    [[nodiscard]] std::string extension() const;
    // Depth::Full: directory entries not checked (beyond the limit).
    [[nodiscard]] bool entriesSkipped() const noexcept { return entriesSkipped_; }

private:
    [[nodiscard]] std::uint64_t offsetOf(std::uint32_t sector) const noexcept {
        return (std::uint64_t{sector} + 1) * header_.sectorSize;
    }
    Result<bool> readFatList();
    Result<bool> readFat();
    Result<bool> readDirectory();
    // The sectors of the chain that starts at `start`, following `table`
    // (the FAT, or the mini FAT for mini sectors), at most `most` of them;
    // false (and a problem noted) when it does not end in ENDOFCHAIN.
    bool chain(const std::vector<std::uint32_t>& table, std::uint32_t start, std::uint64_t most,
               std::vector<std::uint32_t>& out, const std::string& what);
    // Depth::Full.
    Result<bool> checkStreams();
    // False (and a problem noted) when an entry or the tree is wrong;
    // reachable_ then holds the entries the root's tree reaches.
    bool checkEntries();
    void collectRootNames();
    // Marks the sectors of a chain as used; false (and a problem noted) when
    // one is used already. The check stops there: the first problem is the
    // verdict, and chains that share sectors could make it go on and on.
    bool claim(std::vector<std::uint8_t>& used, const std::vector<std::uint32_t>& sectors, std::uint64_t unit,
               std::uint64_t base, const std::string& what);

    IContentReader& content_;
    Depth depth_;
    const Ole2Limits& limits_;
    Walk walk_;
    Header header_;
    std::vector<std::uint32_t> fatList_;
    std::vector<std::uint32_t> difatList_;
    std::vector<std::uint32_t> fat_;
    std::uint32_t last_ = 0;
    std::vector<std::uint32_t> directoryChain_;
    std::vector<Entry> entries_;
    std::vector<std::string> rootNames_;
    std::vector<bool> reachable_;
    bool entriesSkipped_ = false;
    std::uint64_t streams_ = 0;
};

Result<Walk> Ole2Walker::run() {
    Result<std::optional<std::span<const std::byte>>> head = detail::readIfAvailable(content_, 0, kHeaderBytes);
    if (!head.ok()) {
        return head.error();
    }
    if (!head->has_value()) {
        return walk_.finish(WalkStatus::Truncated, 0, "the data ends inside the header");
    }
    if (const std::optional<std::string> wrong = readHeader(**head, header_); wrong.has_value()) {
        return walk_.finish(WalkStatus::Broken, 0, "not a compound file header: " + *wrong);
    }
    Result<bool> listed = readFatList();
    if (!listed.ok()) {
        return listed.error();
    }
    if (!*listed) {
        return walk_;
    }
    Result<bool> read = readFat();
    if (!read.ok()) {
        return read.error();
    }
    if (!*read) {
        return walk_;
    }
    const std::uint64_t end = offsetOf(last_) + header_.sectorSize;
    if (end > content_.size()) {
        return walk_.finish(WalkStatus::Truncated, content_.size(),
                            "the data ends before the file's last sector (" + std::to_string(end) + " bytes)");
    }
    Result<bool> directory = readDirectory();
    if (!directory.ok()) {
        return directory.error();
    }
    if (*directory) {
        collectRootNames();
        // A directory cut at the limit cannot be checked: its links reach beyond what was read.
        if (depth_ == Depth::Full && !entriesSkipped_ && checkEntries()) {
            Result<bool> streams = checkStreams();
            if (!streams.ok()) {
                return streams.error();
            }
        }
    }
    std::string summary = count(std::uint64_t{last_} + 1, "sector", "sectors") + " of " +
                          std::to_string(header_.sectorSize) + " bytes, " +
                          count(entries_.size(), "directory entry", "directory entries");
    if (depth_ == Depth::Full && *directory) {
        summary += ", " + count(streams_, "stream", "streams") + ", every chain consistent";
    }
    return walk_.finish(WalkStatus::Complete, end, summary);
}

void Ole2Walker::takeFreeSectors() {
    const std::uint64_t size = content_.size();
    if (walk_.status != WalkStatus::Complete || walk_.end >= size) {
        return;
    }
    // The last sector the content reaches into (it may be cut short). Every
    // sector after last_ that the FAT covers is free (last_ is the last one in use).
    const std::uint64_t reached = (size - 1) / header_.sectorSize - 1;
    if (reached >= fat_.size()) {
        return;  // bytes no sector of the file holds
    }
    walk_.detail += ", " + count(reached - last_, "free sector", "free sectors") + " after the last one in use";
    walk_.end = size;
}

Result<bool> Ole2Walker::readFatList() {
    const std::uint32_t perSector = header_.sectorSize / 4;
    const std::uint64_t entries = std::uint64_t{header_.fatSectors} * perSector;
    if (entries > limits_.maxSectors) {
        walk_.finish(WalkStatus::Broken, kHeaderBytes,
                     "the header counts " + count(header_.fatSectors, "FAT sector", "FAT sectors") +
                         ", more than the limit of " + std::to_string(limits_.maxSectors) + " sectors");
        return false;
    }
    for (std::size_t i = 0; i < kHeaderDifat && fatList_.size() < header_.fatSectors; ++i) {
        const std::uint32_t sector = header_.difat[i];
        if (sector == kFree) {
            break;
        }
        if (sector > kMaxRegular) {
            walk_.finish(WalkStatus::Broken, kHeaderBytes, "the header's DIFAT holds a marker for a FAT sector");
            return false;
        }
        fatList_.push_back(sector);
    }
    std::uint32_t next = header_.firstDifat;
    for (std::uint32_t k = 0; k < header_.difatSectors && fatList_.size() < header_.fatSectors; ++k) {
        if (next > kMaxRegular || std::uint64_t{next} >= entries) {
            walk_.finish(WalkStatus::Broken, kHeaderBytes,
                         "DIFAT sector " + std::to_string(k + 1) + " is at no sector the FAT covers");
            return false;
        }
        Result<std::optional<std::span<const std::byte>>> read =
            detail::readIfAvailable(content_, offsetOf(next), header_.sectorSize);
        if (!read.ok()) {
            return read.error();
        }
        if (!read->has_value()) {
            walk_.finish(WalkStatus::Truncated, content_.size(), "the data ends before a DIFAT sector");
            return false;
        }
        difatList_.push_back(next);
        for (std::uint32_t i = 0; i + 1 < perSector && fatList_.size() < header_.fatSectors; ++i) {
            const std::uint32_t sector = loadLe32(**read, 4 * std::size_t{i});
            if (sector == kFree) {
                break;
            }
            if (sector > kMaxRegular) {
                walk_.finish(WalkStatus::Broken, offsetOf(next), "a DIFAT sector holds a marker for a FAT sector");
                return false;
            }
            fatList_.push_back(sector);
        }
        next = loadLe32(**read, 4 * std::size_t{perSector - 1});
    }
    if (fatList_.size() != header_.fatSectors) {
        walk_.finish(WalkStatus::Broken, kHeaderBytes,
                     "the DIFAT lists " + count(fatList_.size(), "FAT sector", "FAT sectors") + ", the header " +
                         std::to_string(header_.fatSectors));
        return false;
    }
    return true;
}

Result<bool> Ole2Walker::readFat() {
    const std::uint32_t perSector = header_.sectorSize / 4;
    const std::uint64_t entries = std::uint64_t{header_.fatSectors} * perSector;
    fat_.assign(static_cast<std::size_t>(entries), kFree);
    for (std::size_t k = 0; k < fatList_.size(); ++k) {
        const std::uint32_t sector = fatList_[k];
        if (std::uint64_t{sector} >= entries) {
            walk_.finish(WalkStatus::Broken, kHeaderBytes,
                         "FAT sector " + std::to_string(k + 1) + " is at no sector the FAT covers");
            return false;
        }
        Result<std::optional<std::span<const std::byte>>> read =
            detail::readIfAvailable(content_, offsetOf(sector), header_.sectorSize);
        if (!read.ok()) {
            return read.error();
        }
        if (!read->has_value()) {
            walk_.finish(WalkStatus::Truncated, content_.size(), "the data ends before a FAT sector");
            return false;
        }
        for (std::uint32_t i = 0; i < perSector; ++i) {
            const std::uint32_t value = loadLe32(**read, 4 * std::size_t{i});
            if (value != kFree && value != kEndOfChain && value != kFatSector && value != kDifatSector &&
                std::uint64_t{value} >= entries) {
                walk_.finish(WalkStatus::Broken, offsetOf(sector),
                             "the FAT entry of sector " + std::to_string(k * perSector + i) + " holds " +
                                 std::to_string(value) + ", which is no sector the FAT covers");
                return false;
            }
            fat_[k * perSector + i] = value;
        }
    }
    for (const std::uint32_t sector : fatList_) {
        if (fat_[sector] != kFatSector) {
            walk_.noteProblem(offsetOf(sector), "FAT sector " + std::to_string(sector) + " is not marked as one");
            break;
        }
    }
    for (const std::uint32_t sector : difatList_) {
        if (fat_[sector] != kDifatSector) {
            walk_.noteProblem(offsetOf(sector), "DIFAT sector " + std::to_string(sector) + " is not marked as one");
            break;
        }
    }
    const auto used = std::find_if(fat_.rbegin(), fat_.rend(), [](std::uint32_t v) { return v != kFree; });
    if (used == fat_.rend()) {
        walk_.finish(WalkStatus::Broken, kHeaderBytes, "the FAT has no sector in use");
        return false;
    }
    last_ = static_cast<std::uint32_t>(fat_.rend() - used - 1);
    return true;
}

bool Ole2Walker::chain(const std::vector<std::uint32_t>& table, std::uint32_t start, std::uint64_t most,
                       std::vector<std::uint32_t>& out, const std::string& what) {
    out.clear();
    std::uint32_t sector = start;
    while (sector != kEndOfChain) {
        if (std::uint64_t{sector} >= table.size()) {
            walk_.noteProblem(kHeaderBytes, what + " reaches sector " + std::to_string(sector) +
                                                ", which its table does not cover");
            return false;
        }
        if (out.size() >= most) {
            walk_.noteProblem(kHeaderBytes, what + " is longer than " + count(most, "sector", "sectors"));
            return false;
        }
        // More sectors than the table has: the chain runs in a loop (a size
        // can ask for any length: version 4 sizes have 64 bits).
        if (out.size() >= table.size()) {
            walk_.noteProblem(kHeaderBytes, what + " runs in a loop");
            return false;
        }
        out.push_back(sector);
        const std::uint32_t next = table[sector];
        if (next == kFree || next == kFatSector || next == kDifatSector) {
            walk_.noteProblem(kHeaderBytes, what + " runs into a sector that is not in a chain");
            return false;
        }
        sector = next;
    }
    return true;
}

Result<bool> Ole2Walker::readDirectory() {
    if (!chain(fat_, header_.firstDirectory, std::uint64_t{last_} + 1, directoryChain_, "the directory's chain")) {
        return false;
    }
    if (directoryChain_.empty()) {
        walk_.noteProblem(kHeaderBytes, "the directory has no sector");
        return false;
    }
    if (header_.major == 4 && header_.directorySectors != directoryChain_.size()) {
        walk_.noteProblem(kHeaderBytes, "the header counts " + std::to_string(header_.directorySectors) +
                                            " directory sectors, the chain holds " +
                                            std::to_string(directoryChain_.size()));
    }
    const std::size_t perSector = header_.sectorSize / kEntryBytes;
    for (const std::uint32_t sector : directoryChain_) {
        Result<std::span<const std::byte>> read = content_.read(offsetOf(sector), header_.sectorSize);
        if (!read.ok()) {
            return read.error();
        }
        for (std::size_t i = 0; i < perSector; ++i) {
            if (entries_.size() >= limits_.maxEntries) {
                entriesSkipped_ = true;
                break;
            }
            entries_.push_back(readEntry(read->subspan(i * kEntryBytes, kEntryBytes), header_.major));
        }
    }
    if (entries_.empty() || entries_.front().type != kRoot) {
        walk_.noteProblem(offsetOf(directoryChain_.front()), "the directory's first entry is not the root");
        return false;
    }
    return true;
}

void Ole2Walker::collectRootNames() {
    // The root's children: the tree under its child (left and right siblings).
    std::vector<std::uint32_t> pending{entries_.front().child};
    std::vector<bool> seen(entries_.size(), false);
    while (!pending.empty() && rootNames_.size() < 4096) {
        const std::uint32_t id = pending.back();
        pending.pop_back();
        if (id == kNoStream || id >= entries_.size() || seen[id]) {
            continue;
        }
        seen[id] = true;
        rootNames_.push_back(entries_[id].name);
        pending.push_back(entries_[id].left);
        pending.push_back(entries_[id].right);
    }
}

std::string Ole2Walker::extension() const {
    if (entries_.empty() || entries_.front().type != kRoot) {
        return {};
    }
    const std::array<std::byte, 16>& clsid = entries_.front().clsid;
    const auto isClass = [&](const std::array<std::uint8_t, 16>& wanted) {
        return std::equal(clsid.begin(), clsid.end(), wanted.begin(),
                          [](std::byte a, std::uint8_t b) { return a == static_cast<std::byte>(b); });
    };
    if (isClass(installerClass(0x84))) {
        return "msi";
    }
    if (isClass(installerClass(0x86))) {
        return "msp";
    }
    if (isClass(installerClass(0x82))) {
        return "mst";
    }
    const auto has = [&](std::string_view name) {
        return std::find(rootNames_.begin(), rootNames_.end(), name) != rootNames_.end();
    };
    if (has("WordDocument")) {
        return "doc";
    }
    if (has("Workbook") || has("Book")) {
        return "xls";
    }
    if (has("PowerPoint Document")) {
        return "ppt";
    }
    if (has("__properties_version1.0") ||
        std::any_of(rootNames_.begin(), rootNames_.end(),
                    [](const std::string& name) { return name.starts_with("__substg1.0_"); })) {
        return "msg";
    }
    if (has("VisioDocument")) {
        return "vsd";
    }
    if (has("Quill")) {
        return "pub";
    }
    if (has("Catalog")) {
        return "db";
    }
    return {};
}

bool Ole2Walker::checkEntries() {
    const std::uint64_t total = entries_.size();
    const auto inRange = [&](std::uint32_t id) { return id == kNoStream || id < total; };
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& entry = entries_[i];
        const std::string which = "directory entry " + std::to_string(i);
        if (entry.type != kUnallocated && entry.type != kStorage && entry.type != kStream && entry.type != kRoot) {
            walk_.noteProblem(offsetOf(directoryChain_.front()), which + " has type " + std::to_string(entry.type));
            return false;
        }
        if (entry.type == kRoot && i != 0) {
            walk_.noteProblem(offsetOf(directoryChain_.front()), which + " is a second root");
            return false;
        }
        if (entry.type != kUnallocated &&
            (entry.nameLength > 64 || entry.nameLength % 2 != 0 || entry.nameLength < 2 || !inRange(entry.left) ||
             !inRange(entry.right) || !inRange(entry.child))) {
            walk_.noteProblem(offsetOf(directoryChain_.front()),
                              which + " has a name length or a link no compound file has");
            return false;
        }
    }
    // Every entry the root's tree reaches, once.
    std::vector<bool>& seen = reachable_;
    seen.assign(entries_.size(), false);
    std::vector<std::uint32_t> pending{0};
    while (!pending.empty()) {
        const std::uint32_t id = pending.back();
        pending.pop_back();
        if (id == kNoStream) {
            continue;
        }
        if (seen[id]) {
            walk_.noteProblem(offsetOf(directoryChain_.front()),
                              "the directory's tree reaches entry " + std::to_string(id) + " twice");
            return false;
        }
        seen[id] = true;
        const Entry& entry = entries_[id];
        if (entry.type == kUnallocated) {
            walk_.noteProblem(offsetOf(directoryChain_.front()),
                              "the directory's tree reaches the unallocated entry " + std::to_string(id));
            return false;
        }
        if (id != 0) {
            pending.push_back(entry.left);
            pending.push_back(entry.right);
        }
        if (entry.type == kRoot || entry.type == kStorage) {
            pending.push_back(entry.child);
        }
    }
    return true;
}

bool Ole2Walker::claim(std::vector<std::uint8_t>& used, const std::vector<std::uint32_t>& sectors,
                       std::uint64_t unit, std::uint64_t base, const std::string& what) {
    for (const std::uint32_t sector : sectors) {
        if (sector >= used.size()) {
            continue;
        }
        if (used[sector] != 0) {
            walk_.noteProblem(base + std::uint64_t{sector} * unit,
                              what + " shares sector " + std::to_string(sector) + " with another chain");
            return false;
        }
        used[sector] = 1;
    }
    return true;
}

Result<bool> Ole2Walker::checkStreams() {
    const std::uint64_t sectors = std::uint64_t{last_} + 1;
    const auto lengthOf = [](std::uint64_t size, std::uint64_t unit) { return (size + unit - 1) / unit; };
    std::vector<std::uint8_t> used(static_cast<std::size_t>(sectors), 0);
    if (!claim(used, fatList_, header_.sectorSize, header_.sectorSize, "the FAT") ||
        !claim(used, difatList_, header_.sectorSize, header_.sectorSize, "the DIFAT") ||
        !claim(used, directoryChain_, header_.sectorSize, header_.sectorSize, "the directory")) {
        return false;
    }

    // The mini stream (the root's data) and the mini FAT.
    std::vector<std::uint32_t> sectorsOf;
    const Entry& root = entries_.front();
    std::vector<std::uint32_t> miniStream;
    if (root.size > 0) {
        if (!chain(fat_, root.start, lengthOf(root.size, header_.sectorSize), miniStream, "the mini stream's chain")) {
            return false;
        }
        if (miniStream.size() != lengthOf(root.size, header_.sectorSize)) {
            walk_.noteProblem(header_.sectorSize, "the mini stream's chain holds " +
                                                      count(miniStream.size(), "sector", "sectors") + ", not " +
                                                      std::to_string(lengthOf(root.size, header_.sectorSize)));
            return false;
        }
        if (!claim(used, miniStream, header_.sectorSize, header_.sectorSize, "the mini stream")) {
            return false;
        }
    }
    std::vector<std::uint32_t> miniFat;
    if (header_.miniFatSectors > 0) {
        std::vector<std::uint32_t> miniFatChain;
        if (!chain(fat_, header_.firstMiniFat, header_.miniFatSectors, miniFatChain, "the mini FAT's chain")) {
            return false;
        }
        if (miniFatChain.size() != header_.miniFatSectors) {
            walk_.noteProblem(kHeaderBytes, "the mini FAT's chain holds " +
                                                count(miniFatChain.size(), "sector", "sectors") + ", the header " +
                                                std::to_string(header_.miniFatSectors));
            return false;
        }
        if (!claim(used, miniFatChain, header_.sectorSize, header_.sectorSize, "the mini FAT")) {
            return false;
        }
        for (const std::uint32_t sector : miniFatChain) {
            Result<std::span<const std::byte>> read = content_.read(offsetOf(sector), header_.sectorSize);
            if (!read.ok()) {
                return read.error();
            }
            for (std::size_t i = 0; i < header_.sectorSize / 4; ++i) {
                miniFat.push_back(loadLe32(*read, 4 * i));
            }
        }
    }
    const std::uint64_t miniSectors = root.size / kMiniSector;
    std::vector<std::uint8_t> miniUsed(static_cast<std::size_t>(std::min<std::uint64_t>(miniSectors, miniFat.size())),
                                       0);
    // Mini sectors beyond the mini stream's data cannot be in a chain.
    std::vector<std::uint32_t> table = miniFat;
    if (table.size() > miniSectors) {
        table.resize(static_cast<std::size_t>(miniSectors));
    }

    for (std::size_t i = 1; i < entries_.size(); ++i) {
        const Entry& entry = entries_[i];
        // Entries no tree reaches are left over: nothing reads them.
        if (entry.type != kStream || !reachable_[i]) {
            continue;
        }
        ++streams_;
        if (entry.size == 0) {
            continue;
        }
        const std::string which = "the chain of stream " + std::to_string(i) + " (" + entry.name + ")";
        if (entry.size < kCutoff) {
            const std::uint64_t wanted = lengthOf(entry.size, kMiniSector);
            if (!chain(table, entry.start, wanted, sectorsOf, which)) {
                return false;
            }
            if (sectorsOf.size() != wanted) {
                walk_.noteProblem(kHeaderBytes, which + " holds " + count(sectorsOf.size(), "mini sector",
                                                                          "mini sectors") +
                                                    ", not " + std::to_string(wanted));
                return false;
            }
            if (!claim(miniUsed, sectorsOf, kMiniSector, 0, which)) {
                return false;
            }
        } else {
            const std::uint64_t wanted = lengthOf(entry.size, header_.sectorSize);
            if (!chain(fat_, entry.start, wanted, sectorsOf, which)) {
                return false;
            }
            if (sectorsOf.size() != wanted) {
                walk_.noteProblem(kHeaderBytes, which + " holds " + count(sectorsOf.size(), "sector", "sectors") +
                                                    ", not " + std::to_string(wanted));
                return false;
            }
            if (!claim(used, sectorsOf, header_.sectorSize, header_.sectorSize, which)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

Result<std::shared_ptr<const Ole2Format>> Ole2Format::make(Limits limits) {
    if (limits.maxSectors == 0 || limits.maxEntries == 0) {
        return makeError(ErrorCode::InvalidInput, "the OLE2 format's limits must not be 0");
    }
    return std::shared_ptr<const Ole2Format>(new Ole2Format(limits));
}

Ole2Format::Ole2Format(Limits limits) : limits_(limits) {
    descriptor_.id = "ole2";
    descriptor_.name = "OLE2 compound file";
    descriptor_.extension = "ole";
    descriptor_.signatures = {
        carving::byteSignature("OLE2 compound file signature", {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1})};
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::StructureWalk;
    // Nothing checks the data of a compound file's streams: a document
    // whose sectors another file took since still validates, and that
    // file's hits must still be carved.
    descriptor_.trustValidFiles = false;
}

HeaderCheck Ole2Format::checkHeader(std::span<const std::byte> header) const {
    Header parsed;
    if (const std::optional<std::string> wrong = readHeader(header, parsed); wrong.has_value()) {
        return HeaderCheck::reject(*wrong);
    }
    return HeaderCheck::accept();
}

Result<EndDetection> Ole2Format::findEnd(IContentReader& content) const {
    Ole2Walker walker(content, Depth::Layout, limits_);
    Result<Walk> walk = walker.run();
    if (!walk.ok()) {
        return walk.error();
    }
    EndDetection end = detail::endOf(*walk, content.size());
    end.extension = walker.extension();
    return end;
}

Result<ValidationResult> Ole2Format::validate(IContentReader& content) const {
    Ole2Walker walker(content, Depth::Full, limits_);
    Result<Walk> walk = walker.run();
    if (!walk.ok()) {
        return walk.error();
    }
    // A named file keeps its free sectors; a carved one ends at its last sector in use.
    walker.takeFreeSectors();
    ValidationResult verdict = detail::verdictOf(walker.walk(), content.size());
    if (verdict.status == ValidationStatus::Valid && walker.entriesSkipped()) {
        verdict.status = ValidationStatus::NotValidated;
        verdict.detail += "; the directory has more entries than the limit, and those were not checked";
    }
    return verdict;
}

}  // namespace recovery::formats
