#include "filesystem/exfat/exfat_names.hpp"

#include "recovery/byte_order.hpp"

namespace recovery::filesystem::exfat {

namespace {

constexpr std::size_t kTableSize = 65536;
constexpr char16_t kIdentityRunMarker = 0xFFFF;

}  // namespace

std::uint16_t entrySetChecksum(std::span<const std::byte> entries) noexcept {
    std::uint16_t checksum = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i == 2 || i == 3) {
            continue;
        }
        checksum = static_cast<std::uint16_t>(((checksum & 1U) != 0 ? 0x8000U : 0U) + (checksum >> 1) +
                                              static_cast<std::uint8_t>(entries[i]));
    }
    return checksum;
}

std::uint32_t upcaseTableChecksum(std::span<const std::byte> table) noexcept {
    std::uint32_t checksum = 0;
    for (const std::byte b : table) {
        checksum = ((checksum & 1U) != 0 ? 0x80000000U : 0U) + (checksum >> 1) + static_cast<std::uint8_t>(b);
    }
    return checksum;
}

UpcaseTable::UpcaseTable() : table_(kTableSize) {
    for (std::size_t i = 0; i < kTableSize; ++i) {
        table_[i] = static_cast<char16_t>(i);
    }
}

UpcaseTable UpcaseTable::basic() {
    UpcaseTable table;
    table.basic_ = true;
    for (char16_t c = u'a'; c <= u'z'; ++c) {
        table.table_[c] = static_cast<char16_t>(c - 0x20);
    }
    // Latin-1: U+00E0..U+00FE map to U+00C0..U+00DE, except U+00F7 (division sign).
    for (char16_t c = 0xE0; c <= 0xFE; ++c) {
        if (c != 0xF7) {
            table.table_[c] = static_cast<char16_t>(c - 0x20);
        }
    }
    table.table_[0xFF] = 0x178;  // y with diaeresis
    return table;
}

std::optional<UpcaseTable> UpcaseTable::decode(std::span<const std::byte> data) {
    if (data.empty() || data.size() % 2 != 0 || data.size() > kMaxUpcaseTableBytes) {
        return std::nullopt;
    }
    UpcaseTable table;
    std::size_t index = 0;
    bool identityRun = false;
    // Bounded: one step per stored unit.
    for (std::size_t offset = 0; offset < data.size() && index < kTableSize; offset += 2) {
        const auto unit = static_cast<char16_t>(loadLe16(data, offset));
        if (identityRun) {
            index += unit;  // the next `unit` characters map to themselves
            identityRun = false;
        } else if (unit == kIdentityRunMarker && index != kIdentityRunMarker) {
            identityRun = true;
        } else {
            table.table_[index] = unit;
            ++index;
        }
    }
    return table;
}

std::uint16_t nameHash(std::u16string_view name, const UpcaseTable& upcase) noexcept {
    std::uint16_t hash = 0;
    const auto add = [&hash](std::uint8_t byte) {
        hash = static_cast<std::uint16_t>(((hash & 1U) != 0 ? 0x8000U : 0U) + (hash >> 1) + byte);
    };
    for (const char16_t unit : name) {
        const char16_t upper = upcase.map(unit);
        add(static_cast<std::uint8_t>(upper & 0xFF));
        add(static_cast<std::uint8_t>(upper >> 8));
    }
    return hash;
}

}  // namespace recovery::filesystem::exfat
