#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace recovery {

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), as used by GPT, PNG
// and ZIP. crc32("123456789") == 0xCBF43926.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data) noexcept;

// Incremental form: start with 0 and feed consecutive pieces.
[[nodiscard]] std::uint32_t crc32Update(std::uint32_t crc, std::span<const std::byte> data) noexcept;

}  // namespace recovery
