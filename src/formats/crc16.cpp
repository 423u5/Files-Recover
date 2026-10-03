#include "crc16.hpp"

#include <array>

namespace recovery::formats::detail {

namespace {

// Entries filled in order, as crc32.cpp does, which /analyze can follow.
constexpr std::array<std::uint16_t, 256> makeMsbFirstTable() noexcept {
    std::array<std::uint16_t, 256> table{};
    std::uint32_t index = 0;
    for (std::uint16_t& entry : table) {
        std::uint32_t crc = index++ << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) != 0 ? (crc << 1) ^ 0x8005 : crc << 1;
        }
        entry = static_cast<std::uint16_t>(crc & 0xFFFF);
    }
    return table;
}

constexpr std::array<std::uint16_t, 256> makeLsbFirstTable() noexcept {
    std::array<std::uint16_t, 256> table{};
    std::uint32_t index = 0;
    for (std::uint16_t& entry : table) {
        std::uint32_t crc = index++;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
        entry = static_cast<std::uint16_t>(crc);
    }
    return table;
}

constexpr std::array<std::uint16_t, 256> kMsbFirst = makeMsbFirstTable();
constexpr std::array<std::uint16_t, 256> kLsbFirst = makeLsbFirstTable();

}  // namespace

std::uint16_t crc16Mpeg(std::span<const std::byte> data, std::uint16_t crc) noexcept {
    for (const std::byte byte : data) {
        const auto index = static_cast<std::uint8_t>((crc >> 8) ^ static_cast<std::uint8_t>(byte));
        crc = static_cast<std::uint16_t>((crc << 8) ^ kMsbFirst[index]);
    }
    return crc;
}

std::uint16_t crc16Arc(std::span<const std::byte> data, std::uint16_t crc) noexcept {
    for (const std::byte byte : data) {
        const auto index = static_cast<std::uint8_t>((crc ^ static_cast<std::uint8_t>(byte)) & 0xFF);
        crc = static_cast<std::uint16_t>((crc >> 8) ^ kLsbFirst[index]);
    }
    return crc;
}

}  // namespace recovery::formats::detail
