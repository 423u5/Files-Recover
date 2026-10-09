#pragma once

// UTF-8 decoding and encoding for the reports' text and JSON (and the CLI's
// console output).

#include <cstddef>
#include <string>
#include <string_view>

namespace recovery::report {

inline constexpr char32_t kReplacementCharacter = 0xFFFD;

// Decodes the UTF-8 sequence at text[i] and advances i past it. Invalid
// sequences (stray continuation bytes, overlong forms, surrogates, values
// beyond U+10FFFF, sequences cut short) give U+FFFD and advance by one byte.
// Requires i < text.size().
[[nodiscard]] inline char32_t decodeUtf8(std::string_view text, std::size_t& i) noexcept {
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const unsigned char lead = byte(i);
    if (lead < 0x80) {
        ++i;
        return lead;
    }
    std::size_t length = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        value = lead & 0x1Fu;
        minimum = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        value = lead & 0x0Fu;
        minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        value = lead & 0x07u;
        minimum = 0x10000;
    } else {
        ++i;
        return kReplacementCharacter;
    }
    if (text.size() - i < length) {
        ++i;
        return kReplacementCharacter;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const unsigned char next = byte(i + k);
        if ((next & 0xC0u) != 0x80u) {
            ++i;
            return kReplacementCharacter;
        }
        value = (value << 6) | (next & 0x3Fu);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        ++i;
        return kReplacementCharacter;
    }
    i += length;
    return value;
}

inline void appendUtf8(std::string& out, char32_t value) {
    if (value < 0x80) {
        out.push_back(static_cast<char>(value));
    } else if (value < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (value >> 6)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else if (value < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (value >> 12)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (value >> 18)));
        out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    }
}

}  // namespace recovery::report
