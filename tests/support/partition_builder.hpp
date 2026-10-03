#pragma once

#include "partition/guid.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::test {

// Builders for partition tables inside an in-memory disk. They write exactly
// what the UEFI/MBR specifications describe (including CRCs), so tests can
// then corrupt specific fields.

struct MbrEntrySpec {
    std::uint8_t status = 0x00;
    std::uint8_t type = 0x00;
    std::uint32_t startLba = 0;
    std::uint32_t sectorCount = 0;
};

// Writes an MBR or EBR (up to four entries plus the 0x55AA signature) at `lba`.
void writeMbrSector(std::vector<std::byte>& disk, std::uint32_t sectorSize, std::uint64_t lba,
                    const std::vector<MbrEntrySpec>& entries);

struct GptPartitionSpec {
    partition::Guid type;
    partition::Guid unique;
    std::uint64_t firstLba = 0;
    std::uint64_t lastLba = 0;
    std::uint64_t attributes = 0;
    std::u16string name;
};

struct GptSpec {
    std::uint32_t sectorSize = 512;
    partition::Guid diskGuid = *partition::Guid::parse("11111111-2222-3333-4444-555555555555");
    std::uint32_t entryCount = 128;
    std::uint32_t entrySize = 128;
    std::vector<GptPartitionSpec> partitions;
    bool protectiveMbr = true;
};

struct GptLayout {
    std::uint64_t firstUsableLba = 0;
    std::uint64_t lastUsableLba = 0;
    std::uint64_t primaryEntriesLba = 0;
    std::uint64_t backupEntriesLba = 0;
    std::uint64_t backupHeaderLba = 0;
};

// Writes protective MBR (optional), primary and backup GPT. The disk size is
// the size of `disk`.
GptLayout writeGpt(std::vector<std::byte>& disk, const GptSpec& spec);

// Recomputes the header CRC after a test modified header fields.
void resealGptHeader(std::vector<std::byte>& disk, std::uint32_t sectorSize, std::uint64_t headerLba);

// Recomputes the entry-array CRC (stored in the header) and then the header CRC.
void resealGptEntries(std::vector<std::byte>& disk, std::uint32_t sectorSize, std::uint64_t headerLba);

// Minimal volume boot records for superfloppy detection tests.
[[nodiscard]] std::vector<std::byte> makeFat32BootRecordStub(std::uint32_t sectorSize = 512);
[[nodiscard]] std::vector<std::byte> makeOemBootRecordStub(std::string_view oem, std::uint32_t sectorSize = 512);

}  // namespace recovery::test
