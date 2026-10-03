#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace recovery {

namespace detail {
// Terminates the process. Structure fields are read at offsets fixed by the
// parser, so an out-of-bounds field read is a programming error; it must
// never turn into an out-of-bounds memory access.
[[noreturn]] void failedBoundsCheck(std::size_t offset, std::size_t width, std::size_t size) noexcept;

inline void checkField(std::span<const std::byte> data, std::size_t offset, std::size_t width) noexcept {
    if (offset > data.size() || data.size() - offset < width) {
        failedBoundsCheck(offset, width, data.size());
    }
}
}  // namespace detail

// Bounds-checked little-endian field access for on-disk structures.

[[nodiscard]] inline std::uint8_t loadU8(std::span<const std::byte> data, std::size_t offset) noexcept {
    detail::checkField(data, offset, 1);
    return static_cast<std::uint8_t>(data[offset]);
}

[[nodiscard]] inline std::uint16_t loadLe16(std::span<const std::byte> data, std::size_t offset) noexcept {
    detail::checkField(data, offset, 2);
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset]) |
                                      static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8));
}

[[nodiscard]] inline std::uint32_t loadLe32(std::span<const std::byte> data, std::size_t offset) noexcept {
    detail::checkField(data, offset, 4);
    std::uint32_t value = 0;
    for (std::size_t i = 4; i-- > 0;) {
        value = (value << 8) | static_cast<std::uint32_t>(data[offset + i]);
    }
    return value;
}

[[nodiscard]] inline std::uint64_t loadLe64(std::span<const std::byte> data, std::size_t offset) noexcept {
    detail::checkField(data, offset, 8);
    std::uint64_t value = 0;
    for (std::size_t i = 8; i-- > 0;) {
        value = (value << 8) | static_cast<std::uint64_t>(data[offset + i]);
    }
    return value;
}

// Bounds-checked big-endian field access (JPEG, PNG and other network-order formats).

[[nodiscard]] inline std::uint16_t loadBe16(std::span<const std::byte> data, std::size_t offset) noexcept {
    detail::checkField(data, offset, 2);
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset]) << 8) |
                                      static_cast<std::uint16_t>(data[offset + 1]));
}

[[nodiscard]] inline std::uint32_t loadBe32(std::span<const std::byte> data, std::size_t offset) noexcept {
    detail::checkField(data, offset, 4);
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<std::uint32_t>(data[offset + i]);
    }
    return value;
}

[[nodiscard]] inline std::uint64_t loadBe64(std::span<const std::byte> data, std::size_t offset) noexcept {
    detail::checkField(data, offset, 8);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(data[offset + i]);
    }
    return value;
}

inline void storeLe16(std::span<std::byte> data, std::size_t offset, std::uint16_t value) noexcept {
    detail::checkField(data, offset, 2);
    data[offset] = static_cast<std::byte>(value & 0xFF);
    data[offset + 1] = static_cast<std::byte>(value >> 8);
}

inline void storeLe32(std::span<std::byte> data, std::size_t offset, std::uint32_t value) noexcept {
    detail::checkField(data, offset, 4);
    for (std::size_t i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
    }
}

inline void storeLe64(std::span<std::byte> data, std::size_t offset, std::uint64_t value) noexcept {
    detail::checkField(data, offset, 8);
    for (std::size_t i = 0; i < 8; ++i) {
        data[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
    }
}

inline void storeBe16(std::span<std::byte> data, std::size_t offset, std::uint16_t value) noexcept {
    detail::checkField(data, offset, 2);
    data[offset] = static_cast<std::byte>(value >> 8);
    data[offset + 1] = static_cast<std::byte>(value & 0xFF);
}

inline void storeBe32(std::span<std::byte> data, std::size_t offset, std::uint32_t value) noexcept {
    detail::checkField(data, offset, 4);
    for (std::size_t i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<std::byte>((value >> (8 * (3 - i))) & 0xFF);
    }
}

inline void storeBe64(std::span<std::byte> data, std::size_t offset, std::uint64_t value) noexcept {
    detail::checkField(data, offset, 8);
    for (std::size_t i = 0; i < 8; ++i) {
        data[offset + i] = static_cast<std::byte>((value >> (8 * (7 - i))) & 0xFF);
    }
}

}  // namespace recovery
