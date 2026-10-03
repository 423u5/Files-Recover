#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace recovery {

inline constexpr char32_t kReplacementCharacter = U'�';

// Appends the UTF-8 encoding of `codePoint`; invalid code points (surrogates,
// values above U+10FFFF) are replaced by U+FFFD.
void appendUtf8(std::string& out, char32_t codePoint);

// Converts UTF-16 code units (e.g. from on-disk names) to UTF-8. Unpaired
// surrogates become U+FFFD; conversion never fails.
[[nodiscard]] std::string utf16ToUtf8(std::span<const char16_t> units);

// Reads `count` little-endian UTF-16 code units from `bytes`.
[[nodiscard]] std::u16string loadUtf16Le(std::span<const std::byte> bytes, std::size_t count);

}  // namespace recovery
