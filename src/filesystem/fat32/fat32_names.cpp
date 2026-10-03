#include "filesystem/fat32/fat32_names.hpp"

#include "recovery/unicode.hpp"

#include <array>

namespace recovery::filesystem::fat32 {

namespace {

// Code page 437, bytes 0x80-0xFF.
constexpr std::array<char16_t, 128> kCp437High = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE,
    0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6,
    0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA,
    0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502,
    0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514,
    0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550,
    0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C,
    0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
    0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

void appendNamePart(std::string& out, std::span<const std::byte> part, bool lowercase, bool firstIsName) {
    std::size_t length = part.size();
    while (length > 0 && part[length - 1] == std::byte{' '}) {
        --length;
    }
    for (std::size_t i = 0; i < length; ++i) {
        auto c = static_cast<std::uint8_t>(part[i]);
        if (i == 0 && firstIsName && c == 0x05) {
            c = 0xE5;  // 0x05 escapes a real 0xE5 first character
        }
        if (lowercase && c >= 'A' && c <= 'Z') {
            c = static_cast<std::uint8_t>(c - 'A' + 'a');
        }
        out += oemToUtf8(c);
    }
}

}  // namespace

std::uint8_t longNameChecksum(std::span<const std::byte> shortName) noexcept {
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < 11 && i < shortName.size(); ++i) {
        sum = static_cast<std::uint8_t>(((sum & 1U) << 7) + (sum >> 1) + static_cast<std::uint8_t>(shortName[i]));
    }
    return sum;
}

bool isValidShortNameByte(std::uint8_t c) noexcept {
    if (c < 0x20) {
        return false;
    }
    switch (c) {
    case '"':
    case '*':
    case '+':
    case ',':
    case '.':
    case '/':
    case ':':
    case ';':
    case '<':
    case '=':
    case '>':
    case '?':
    case '[':
    case '\\':
    case ']':
    case '|':
        return false;
    default:
        return true;
    }
}

std::string oemToUtf8(std::uint8_t c) {
    std::string out;
    appendUtf8(out, c < 0x80 ? static_cast<char32_t>(c) : static_cast<char32_t>(kCp437High[c - 0x80]));
    return out;
}

std::string formatShortName(std::span<const std::byte> shortName, std::uint8_t ntFlags) {
    std::string out;
    if (shortName.size() < 11) {
        return out;
    }
    appendNamePart(out, shortName.first(8), (ntFlags & 0x08) != 0, true);
    std::string extension;
    appendNamePart(extension, shortName.subspan(8, 3), (ntFlags & 0x10) != 0, false);
    if (!extension.empty()) {
        out += '.';
        out += extension;
    }
    return out;
}

}  // namespace recovery::filesystem::fat32
