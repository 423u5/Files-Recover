#pragma once

// The two CRC-16s of MPEG audio (private to recovery_formats). Both use the
// polynomial x^16 + x^15 + x^2 + 1 (0x8005), in opposite bit orders.

#include <cstddef>
#include <cstdint>
#include <span>

namespace recovery::formats::detail {

// ISO/IEC 11172-3 2.4.3.1: the error check of a protected MPEG audio frame.
// Most significant bit first, initial value 0xFFFF. crc16Mpeg("123456789")
// == 0xAEE7 (CRC-16/CMS).
[[nodiscard]] std::uint16_t crc16Mpeg(std::span<const std::byte> data, std::uint16_t crc = 0xFFFF) noexcept;

// CRC-16/ARC: least significant bit first (reflected polynomial 0xA001),
// initial value 0. The LAME tag's checksums of itself and of the music.
// crc16Arc("123456789") == 0xBB3D.
[[nodiscard]] std::uint16_t crc16Arc(std::span<const std::byte> data, std::uint16_t crc = 0) noexcept;

}  // namespace recovery::formats::detail
