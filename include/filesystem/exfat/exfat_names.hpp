#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::filesystem::exfat {

inline constexpr std::size_t kDirectoryEntrySize = 32;
// Characters stored in one File Name entry.
inline constexpr std::size_t kNameCharsPerEntry = 15;
// A complete up-case table (one UTF-16 unit for each of 65,536 characters).
inline constexpr std::size_t kMaxUpcaseTableBytes = 65536 * 2;

// Checksum of a directory entry set: every byte of `entries` (primary entry
// first) except bytes 2-3, which hold the checksum (exFAT specification 6.3.3).
[[nodiscard]] std::uint16_t entrySetChecksum(std::span<const std::byte> entries) noexcept;

// Checksum of an up-case table as stored on the volume (specification 7.2.2).
[[nodiscard]] std::uint32_t upcaseTableChecksum(std::span<const std::byte> table) noexcept;

// Upper-case mapping for UTF-16 code units, used for name hashes.
class UpcaseTable {
public:
    // Fallback when the volume's table is unusable: maps only ASCII and
    // Latin-1 letters. Hashes of names with other lowercase letters may
    // then differ from what the volume recorded.
    [[nodiscard]] static UpcaseTable basic();

    // Decodes a volume up-case table, compressed (0xFFFF followed by the
    // length of an identity run) or not. Characters the table does not reach
    // map to themselves. Returns nullopt for a table that is empty, has an
    // odd length, or is larger than a complete table.
    [[nodiscard]] static std::optional<UpcaseTable> decode(std::span<const std::byte> data);

    [[nodiscard]] char16_t map(char16_t unit) const noexcept { return table_[unit]; }
    // True for the basic() fallback table.
    [[nodiscard]] bool isBasic() const noexcept { return basic_; }
    // True when the table is known to cover `unit` the way a volume table
    // would (always, except for the fallback beyond Latin-1).
    [[nodiscard]] bool covers(char16_t unit) const noexcept { return !basic_ || unit < 0x100; }

private:
    UpcaseTable();

    std::vector<char16_t> table_;
    bool basic_ = false;
};

// Name hash stored in the Stream Extension entry: a 16-bit rotating sum of
// the up-cased name's UTF-16LE bytes (specification 7.6.4).
[[nodiscard]] std::uint16_t nameHash(std::u16string_view name, const UpcaseTable& upcase) noexcept;

}  // namespace recovery::filesystem::exfat
