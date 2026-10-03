#include "recovery/unicode.hpp"

#include "recovery/byte_order.hpp"

namespace recovery {

void appendUtf8(std::string& out, char32_t codePoint) {
    if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
        codePoint = kReplacementCharacter;
    }
    if (codePoint < 0x80) {
        out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        out += static_cast<char>(0xC0 | (codePoint >> 6));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codePoint >> 12));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codePoint >> 18));
        out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

std::string utf16ToUtf8(std::span<const char16_t> units) {
    std::string out;
    out.reserve(units.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        const char32_t unit = units[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            const char32_t low = units[i + 1];
            appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
            ++i;
        } else {
            appendUtf8(out, unit);  // unpaired surrogates become U+FFFD
        }
    }
    return out;
}

std::u16string loadUtf16Le(std::span<const std::byte> bytes, std::size_t count) {
    std::u16string units;
    units.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        units += static_cast<char16_t>(loadLe16(bytes, 2 * i));
    }
    return units;
}

}  // namespace recovery
