#pragma once

// Private to the FAT32 module.

#include "filesystem/fat32/fat32_boot_sector.hpp"
#include "filesystem/filesystem.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace recovery::filesystem::fat32 {

inline constexpr std::size_t kDirectoryEntrySize = 32;

// Decodes a stream of 32-byte directory entries, assembling long names.
//
// Active long names must have consecutive ordinals, a consistent checksum
// and a checksum matching the 8.3 entry; anything else is discarded.
// Deleted entries lose the first byte of every slot (ordinals included), so
// deleted long names are assembled from the adjacent deleted slots that share
// one checksum, and verified against the 8.3 name where the lost first
// character can be inferred.
class DirectoryParser {
public:
    explicit DirectoryParser(const BootSector& boot) noexcept : boot_(boot) {}

    // Processes one entry. Returns false at the end-of-directory marker.
    bool feed(std::span<const std::byte> entry, std::uint64_t volumeOffset, std::vector<DirectoryEntry>& out);

    // Active volume-label entry, if one was seen.
    [[nodiscard]] const std::optional<std::string>& volumeLabel() const noexcept { return volumeLabel_; }

private:
    struct LongNamePart {
        std::uint8_t ordinal = 0;  // 0 for deleted slots
        bool last = false;         // 0x40 flag
        std::uint8_t checksum = 0;
        std::array<char16_t, 13> units{};
    };

    [[nodiscard]] std::optional<std::u16string> takeActiveLongName(std::uint8_t checksum, bool& mismatch);
    [[nodiscard]] std::optional<std::u16string> takeDeletedLongName(std::optional<std::uint8_t>& checksum);

    const BootSector& boot_;
    std::vector<LongNamePart> pending_;  // on-disk order
    bool pendingDeleted_ = false;
    std::optional<std::string> volumeLabel_;
};

// Decodes a FAT date/time. Returns nullopt for an unset date; sets `invalid`
// when fields are out of range.
[[nodiscard]] std::optional<Timestamp> decodeTimestamp(std::uint16_t date, std::uint16_t time,
                                                       std::uint8_t hundredths, bool& invalid);

}  // namespace recovery::filesystem::fat32
