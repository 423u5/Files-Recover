#include "support/partition_builder.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/crc32.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>

namespace recovery::test {

namespace {

std::span<std::byte> sectorAt(std::vector<std::byte>& disk, std::uint32_t sectorSize, std::uint64_t lba) {
    const std::uint64_t offset = lba * sectorSize;
    if (offset + sectorSize > disk.size()) {
        throw std::out_of_range("sector outside test disk");
    }
    return std::span<std::byte>(disk).subspan(static_cast<std::size_t>(offset), sectorSize);
}

void writeText(std::span<std::byte> out, std::size_t offset, std::string_view text) {
    std::memcpy(out.data() + offset, text.data(), text.size());
}

void writeGuid(std::span<std::byte> out, std::size_t offset, const partition::Guid& guid) {
    std::copy(guid.bytes().begin(), guid.bytes().end(), out.begin() + static_cast<std::ptrdiff_t>(offset));
}

}  // namespace

void writeMbrSector(std::vector<std::byte>& disk, std::uint32_t sectorSize, std::uint64_t lba,
                    const std::vector<MbrEntrySpec>& entries) {
    const std::span<std::byte> sector = sectorAt(disk, sectorSize, lba);
    for (std::size_t i = 0; i < entries.size() && i < 4; ++i) {
        const std::size_t base = 446 + i * 16;
        sector[base] = static_cast<std::byte>(entries[i].status);
        sector[base + 4] = static_cast<std::byte>(entries[i].type);
        storeLe32(sector, base + 8, entries[i].startLba);
        storeLe32(sector, base + 12, entries[i].sectorCount);
    }
    sector[510] = std::byte{0x55};
    sector[511] = std::byte{0xAA};
}

GptLayout writeGpt(std::vector<std::byte>& disk, const GptSpec& spec) {
    const std::uint32_t ss = spec.sectorSize;
    const std::uint64_t sectors = disk.size() / ss;
    const std::uint64_t arrayBytes = static_cast<std::uint64_t>(spec.entryCount) * spec.entrySize;
    const std::uint64_t arraySectors = (arrayBytes + ss - 1) / ss;

    GptLayout layout;
    layout.primaryEntriesLba = 2;
    layout.firstUsableLba = 2 + arraySectors;
    layout.backupHeaderLba = sectors - 1;
    layout.backupEntriesLba = sectors - 1 - arraySectors;
    layout.lastUsableLba = layout.backupEntriesLba - 1;

    std::vector<std::byte> array(static_cast<std::size_t>(arraySectors * ss));
    for (std::size_t i = 0; i < spec.partitions.size(); ++i) {
        const GptPartitionSpec& p = spec.partitions[i];
        const std::span<std::byte> e = std::span<std::byte>(array).subspan(i * spec.entrySize, spec.entrySize);
        writeGuid(e, 0, p.type);
        writeGuid(e, 16, p.unique);
        storeLe64(e, 32, p.firstLba);
        storeLe64(e, 40, p.lastLba);
        storeLe64(e, 48, p.attributes);
        for (std::size_t c = 0; c < p.name.size() && c < 36; ++c) {
            storeLe16(e, 56 + 2 * c, static_cast<std::uint16_t>(p.name[c]));
        }
    }
    const std::uint32_t arrayCrc = crc32(std::span<const std::byte>(array).first(static_cast<std::size_t>(arrayBytes)));

    const auto writeHeader = [&](std::uint64_t myLba, std::uint64_t alternateLba, std::uint64_t entriesLba) {
        const std::span<std::byte> h = sectorAt(disk, ss, myLba);
        std::fill(h.begin(), h.end(), std::byte{0});
        writeText(h, 0, "EFI PART");
        storeLe32(h, 8, 0x00010000);
        storeLe32(h, 12, 92);
        storeLe64(h, 24, myLba);
        storeLe64(h, 32, alternateLba);
        storeLe64(h, 40, layout.firstUsableLba);
        storeLe64(h, 48, layout.lastUsableLba);
        writeGuid(h, 56, spec.diskGuid);
        storeLe64(h, 72, entriesLba);
        storeLe32(h, 80, spec.entryCount);
        storeLe32(h, 84, spec.entrySize);
        storeLe32(h, 88, arrayCrc);
        storeLe32(h, 16, crc32(h.first(92)));
        std::copy(array.begin(), array.end(),
                  disk.begin() + static_cast<std::ptrdiff_t>(entriesLba * ss));
    };
    writeHeader(1, layout.backupHeaderLba, layout.primaryEntriesLba);
    writeHeader(layout.backupHeaderLba, 1, layout.backupEntriesLba);

    if (spec.protectiveMbr) {
        const auto size = static_cast<std::uint32_t>(std::min<std::uint64_t>(sectors - 1, 0xFFFFFFFFULL));
        writeMbrSector(disk, ss, 0, {{0x00, 0xEE, 1, size}});
    }
    return layout;
}

void resealGptHeader(std::vector<std::byte>& disk, std::uint32_t sectorSize, std::uint64_t headerLba) {
    const std::span<std::byte> h = sectorAt(disk, sectorSize, headerLba);
    const std::uint32_t headerSize = std::min<std::uint32_t>(loadLe32(h, 12), sectorSize);
    storeLe32(h, 16, 0);
    storeLe32(h, 16, crc32(h.first(std::max<std::uint32_t>(headerSize, 92))));
}

void resealGptEntries(std::vector<std::byte>& disk, std::uint32_t sectorSize, std::uint64_t headerLba) {
    const std::span<std::byte> h = sectorAt(disk, sectorSize, headerLba);
    const std::uint64_t entriesLba = loadLe64(h, 72);
    const std::uint64_t bytes = static_cast<std::uint64_t>(loadLe32(h, 80)) * loadLe32(h, 84);
    // Fuzz tests call this on corrupted headers: leave nonsense alone.
    if (entriesLba > disk.size() / sectorSize || bytes > disk.size() - entriesLba * sectorSize) {
        return;
    }
    const std::span<const std::byte> array =
        std::span<const std::byte>(disk).subspan(static_cast<std::size_t>(entriesLba * sectorSize),
                                                 static_cast<std::size_t>(bytes));
    storeLe32(h, 88, crc32(array));
    resealGptHeader(disk, sectorSize, headerLba);
}

std::vector<std::byte> makeFat32BootRecordStub(std::uint32_t sectorSize) {
    std::vector<std::byte> s(sectorSize);
    s[0] = std::byte{0xEB};
    s[1] = std::byte{0x58};
    s[2] = std::byte{0x90};
    writeText(s, 3, "MSWIN4.1");
    storeLe16(s, 11, static_cast<std::uint16_t>(sectorSize));
    s[13] = std::byte{8};
    storeLe16(s, 14, 32);
    s[16] = std::byte{2};
    writeText(s, 82, "FAT32   ");
    s[510] = std::byte{0x55};
    s[511] = std::byte{0xAA};
    return s;
}

std::vector<std::byte> makeOemBootRecordStub(std::string_view oem, std::uint32_t sectorSize) {
    std::vector<std::byte> s(sectorSize);
    s[0] = std::byte{0xEB};
    s[1] = std::byte{0x76};
    s[2] = std::byte{0x90};
    writeText(s, 3, oem);
    s[510] = std::byte{0x55};
    s[511] = std::byte{0xAA};
    return s;
}

}  // namespace recovery::test
