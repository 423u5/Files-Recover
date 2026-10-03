#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace recovery::partition {

// A GUID in its on-disk (GPT, mixed-endian) byte order: the first three
// fields are little-endian, the last eight bytes are stored as-is.
class Guid {
public:
    constexpr Guid() noexcept = default;

    // Copies 16 on-disk bytes; `bytes` must hold at least 16.
    [[nodiscard]] static Guid fromDisk(std::span<const std::byte> bytes) noexcept;

    // Parses the canonical text form "C12A7328-F81F-11D2-BA4B-00A0C93EC93B".
    [[nodiscard]] static constexpr std::optional<Guid> parse(std::string_view text) noexcept;

    [[nodiscard]] std::string toString() const;
    [[nodiscard]] constexpr bool isZero() const noexcept {
        for (const std::byte b : bytes_) {
            if (b != std::byte{0}) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] constexpr const std::array<std::byte, 16>& bytes() const noexcept { return bytes_; }

    friend constexpr bool operator==(const Guid&, const Guid&) noexcept = default;

private:
    std::array<std::byte, 16> bytes_{};
};

constexpr std::optional<Guid> Guid::parse(std::string_view text) noexcept {
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> canonical{};
    std::size_t out = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '-') {
            ++i;
            continue;
        }
        int value = 0;
        for (int nibble = 0; nibble < 2; ++nibble, ++i) {
            const char c = text[i];
            int digit = -1;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = c - 'A' + 10;
            }
            if (digit < 0 || c == '-') {
                return std::nullopt;
            }
            value = value * 16 + digit;
        }
        canonical[out++] = static_cast<std::uint8_t>(value);
    }
    // Canonical (big-endian text) order -> on-disk order.
    constexpr std::array<std::size_t, 16> kOrder = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    Guid guid;
    for (std::size_t i = 0; i < 16; ++i) {
        guid.bytes_[i] = static_cast<std::byte>(canonical[kOrder[i]]);
    }
    return guid;
}

// Well-known GPT partition types.
namespace gpt_types {
inline constexpr Guid kEfiSystem = *Guid::parse("C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
inline constexpr Guid kMicrosoftBasicData = *Guid::parse("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7");
inline constexpr Guid kMicrosoftReserved = *Guid::parse("E3C9E316-0B5C-4DB8-817D-F92DF00215AE");
inline constexpr Guid kWindowsRecovery = *Guid::parse("DE94BBA4-06D1-4D40-A16A-BFD50179D6AC");
inline constexpr Guid kLinuxFilesystem = *Guid::parse("0FC63DAF-8483-4772-8E79-3D69D8477DE4");
inline constexpr Guid kAppleHfsPlus = *Guid::parse("48465300-0000-11AA-AA11-00306543ECAC");
inline constexpr Guid kAppleApfs = *Guid::parse("7C3457EF-0000-11AA-AA11-00306543ECAC");
}  // namespace gpt_types

}  // namespace recovery::partition
