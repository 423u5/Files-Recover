#pragma once

// SHA-256 (FIPS 180-4): the content identity of recovered files (P14).
// Duplicate detection compares these digests, so two files are the same
// content only when every byte is equal (up to the hash's collision
// resistance), whatever their names.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace recovery {

class Sha256Digest {
public:
    static constexpr std::size_t kSize = 32;

    constexpr Sha256Digest() noexcept = default;
    constexpr explicit Sha256Digest(const std::array<std::uint8_t, kSize>& bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] constexpr const std::array<std::uint8_t, kSize>& bytes() const noexcept { return bytes_; }
    // 64 lower-case hexadecimal digits.
    [[nodiscard]] std::string hex() const;

    friend constexpr bool operator==(const Sha256Digest&, const Sha256Digest&) noexcept = default;
    friend constexpr auto operator<=>(const Sha256Digest&, const Sha256Digest&) noexcept = default;

private:
    std::array<std::uint8_t, kSize> bytes_{};
};

// Incremental SHA-256. Messages may be up to 2^61 - 1 bytes long (FIPS
// 180-4's limit of 2^64 bits); recovered files are far smaller.
class Sha256 {
public:
    Sha256() noexcept;

    void update(std::span<const std::byte> data) noexcept;
    // Feeds `count` zero bytes: the parts of a reconstructed file that read as
    // zeros without being delivered.
    void updateZeros(std::uint64_t count) noexcept;
    // The digest of everything fed since construction or the last reset();
    // the hash starts over afterwards.
    [[nodiscard]] Sha256Digest finish() noexcept;
    void reset() noexcept;

    // Bytes fed since construction or the last reset().
    [[nodiscard]] std::uint64_t size() const noexcept { return length_; }

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;
};

// One-shot form.
[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> data) noexcept;

}  // namespace recovery
