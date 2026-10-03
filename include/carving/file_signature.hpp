#pragma once

// File signatures: the fixed bytes a format's files carry at a known position
// from their start. The signature scanner looks for them on raw media to find
// where files may begin; a match is only a hint until the format has checked
// the header and the structure behind it.

#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::carving {

struct FileSignature {
    static constexpr std::size_t kMinLength = 2;
    static constexpr std::size_t kMaxLength = 64;
    // Largest distance of a pattern from the start of its file.
    static constexpr std::uint32_t kMaxOffset = 4096;

    // Shown in evidence and logs ("PNG signature").
    std::string name;
    // The bytes compared, `offset` bytes into the file.
    std::vector<std::byte> pattern;
    // Bits of each pattern byte that are compared. Empty compares every bit;
    // otherwise it has one byte per pattern byte. The first byte must be 0xFF
    // (fully compared), because the scanner indexes signatures by it.
    std::vector<std::byte> mask;
    // Position of the pattern in the file, e.g. 4 for the type of an ISO BMFF
    // "ftyp" box that follows its 4-byte size.
    std::uint32_t offset = 0;

    // Bytes a file needs to hold the pattern: offset + pattern.size().
    [[nodiscard]] std::size_t reach() const noexcept;

    // True when `data`, which starts at the pattern's position, begins with
    // the pattern under the mask. False when `data` is shorter than the pattern.
    [[nodiscard]] bool matches(std::span<const std::byte> data) const noexcept;
};

// Checks a signature against the limits above: a non-empty name, a pattern
// of kMinLength to kMaxLength bytes, a mask that is empty or as long as the
// pattern and starts with 0xFF, and an offset of at most kMaxOffset.
[[nodiscard]] Status validateSignature(const FileSignature& signature);

// A signature from byte values, optionally masked:
// byteSignature("MPEG frame sync", {0xFF, 0xE0}, 0, {0xFF, 0xE0}).
[[nodiscard]] FileSignature byteSignature(std::string name, std::initializer_list<std::uint8_t> pattern,
                                          std::uint32_t offset = 0, std::initializer_list<std::uint8_t> mask = {});

// A signature from the bytes of `text`: textSignature("GIF89a", "GIF89a").
[[nodiscard]] FileSignature textSignature(std::string name, std::string_view text, std::uint32_t offset = 0);

}  // namespace recovery::carving
