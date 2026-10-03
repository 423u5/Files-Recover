#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace recovery::filesystem::fat32 {

// Checksum of an 11-byte 8.3 name, stored in every long-name entry that
// belongs to it (FAT specification, "ChkSum").
[[nodiscard]] std::uint8_t longNameChecksum(std::span<const std::byte> shortName) noexcept;

// True when `c` may appear in an 8.3 name (bytes >= 0x80 are OEM characters).
[[nodiscard]] bool isValidShortNameByte(std::uint8_t c) noexcept;

// Converts an OEM (code page 437) byte to UTF-8.
[[nodiscard]] std::string oemToUtf8(std::uint8_t c);

// Formats the 11-byte on-disk name as "NAME.EXT" in UTF-8.
//
// `ntFlags` is directory-entry byte 12: bit 3 = lowercase base name, bit 4 =
// lowercase extension (how Windows stores names such as "readme.txt"
// without a long name). A leading 0x05 byte stands for 0xE5.
[[nodiscard]] std::string formatShortName(std::span<const std::byte> shortName, std::uint8_t ntFlags);

}  // namespace recovery::filesystem::fat32
