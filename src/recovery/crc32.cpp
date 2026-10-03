#include "recovery/crc32.hpp"

#include <array>

namespace recovery {

namespace {

constexpr std::array<std::uint32_t, 256> makeTable() noexcept {
    std::array<std::uint32_t, 256> table{};
    std::uint32_t index = 0;
    for (std::uint32_t& entry : table) {
        std::uint32_t value = index++;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0 ? (value >> 1) ^ 0xEDB88320U : value >> 1;
        }
        entry = value;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = makeTable();

}  // namespace

std::uint32_t crc32Update(std::uint32_t crc, std::span<const std::byte> data) noexcept {
    std::uint32_t value = ~crc;
    for (const std::byte b : data) {
        value = kTable[(value ^ static_cast<std::uint32_t>(b)) & 0xFFU] ^ (value >> 8);
    }
    return ~value;
}

std::uint32_t crc32(std::span<const std::byte> data) noexcept {
    return crc32Update(0, data);
}

}  // namespace recovery
