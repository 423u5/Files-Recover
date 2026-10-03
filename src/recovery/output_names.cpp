#include "recovery/output_names.hpp"

#include <algorithm>
#include <array>

namespace recovery {

namespace {

constexpr wchar_t kReplacement = L'\uFFFD';

// Decodes UTF-8 into UTF-16. Invalid, overlong and truncated sequences and
// encoded surrogates each become one U+FFFD.
std::wstring utf8ToWide(std::string_view text) {
    std::wstring out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        char32_t codePoint = 0;
        char32_t minimum = 0;
        if (lead < 0x80) {
            out.push_back(static_cast<wchar_t>(lead));
            ++i;
            continue;
        }
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            codePoint = lead & 0x1F;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            codePoint = lead & 0x0F;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            codePoint = lead & 0x07;
            minimum = 0x10000;
        } else {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        std::size_t consumed = 1;
        while (consumed < length && i + consumed < text.size() &&
               (static_cast<unsigned char>(text[i + consumed]) & 0xC0) == 0x80) {
            codePoint = (codePoint << 6) | (static_cast<unsigned char>(text[i + consumed]) & 0x3F);
            ++consumed;
        }
        i += consumed;
        if (consumed < length || codePoint < minimum || codePoint > 0x10FFFF ||
            (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            out.push_back(kReplacement);
        } else if (codePoint >= 0x10000) {
            const char32_t value = codePoint - 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (value >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (value & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(codePoint));
        }
    }
    return out;
}

bool isForbidden(wchar_t c) noexcept {
    switch (c) {
    case L'<':
    case L'>':
    case L':':
    case L'"':
    case L'/':
    case L'\\':
    case L'|':
    case L'?':
    case L'*':
        return true;
    default:
        return c < 0x20;
    }
}

wchar_t upperAscii(wchar_t c) noexcept {
    return c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - L'a' + L'A') : c;
}

bool equalsUpper(std::wstring_view text, std::wstring_view upper) noexcept {
    return text.size() == upper.size() &&
           std::equal(text.begin(), text.end(), upper.begin(), [](wchar_t a, wchar_t b) { return upperAscii(a) == b; });
}

// Windows treats these as devices, with or without an extension.
bool isReservedDeviceName(std::wstring_view name) noexcept {
    std::wstring_view base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ') {
        base.remove_suffix(1);
    }
    constexpr std::array<std::wstring_view, 6> kNames{L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    if (std::any_of(kNames.begin(), kNames.end(),
                    [&](std::wstring_view reserved) { return equalsUpper(base, reserved); })) {
        return true;
    }
    if (base.size() != 4 || !(equalsUpper(base.substr(0, 3), L"COM") || equalsUpper(base.substr(0, 3), L"LPT"))) {
        return false;
    }
    const wchar_t digit = base[3];
    // Superscript one, two and three count as digits too.
    return (digit >= L'0' && digit <= L'9') || digit == L'\u00B9' || digit == L'\u00B2' || digit == L'\u00B3';
}

// Where the extension starts (its dot), or npos. A leading dot starts the stem, not an extension.
std::size_t extensionStart(std::wstring_view name) noexcept {
    const std::size_t dot = name.rfind(L'.');
    return dot == 0 ? std::wstring_view::npos : dot;
}

// The first `count` units of `text`, without splitting a surrogate pair.
std::wstring_view prefix(std::wstring_view text, std::size_t count) noexcept {
    if (count >= text.size()) {
        return text;
    }
    if (count > 0 && text[count - 1] >= 0xD800 && text[count - 1] <= 0xDBFF) {
        --count;
    }
    return text.substr(0, count);
}

void trimTrailingDotsAndSpaces(std::wstring& name) {
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) {
        name.pop_back();
    }
}

// `stem` + `suffix` + extension of `name`, shortened to kMaxNameLength by
// shortening the stem. An extension too long to keep is cut with the stem.
std::wstring compose(std::wstring_view name, std::wstring_view suffix) {
    std::size_t dot = extensionStart(name);
    if (dot != std::wstring_view::npos && name.size() - dot + suffix.size() > kMaxNameLength / 2) {
        dot = std::wstring_view::npos;
    }
    const std::wstring_view stem = dot == std::wstring_view::npos ? name : name.substr(0, dot);
    const std::wstring_view extension = dot == std::wstring_view::npos ? std::wstring_view{} : name.substr(dot);
    const std::size_t room = kMaxNameLength - suffix.size() - extension.size();
    std::wstring result(prefix(stem, room));
    result += suffix;
    result += extension;
    trimTrailingDotsAndSpaces(result);
    if (result.empty()) {
        result = L"_";
    }
    return result;
}

}  // namespace

std::wstring safeFileName(std::string_view originalName) {
    if (originalName.empty()) {
        return std::wstring(kUnnamed);
    }
    std::wstring name = utf8ToWide(originalName);
    std::replace_if(name.begin(), name.end(), isForbidden, L'_');
    // Windows drops trailing dots and spaces; "." and ".." end up empty here.
    trimTrailingDotsAndSpaces(name);
    if (name.empty()) {
        return L"_";
    }
    if (isReservedDeviceName(name)) {
        name.insert(name.begin(), L'_');
    }
    if (name.size() > kMaxNameLength) {
        name = compose(name, {});
    }
    return name;
}

std::wstring numberedName(std::wstring_view safeName, std::uint32_t number) {
    if (number == 0) {
        return std::wstring(safeName);
    }
    return compose(safeName, L" (" + std::to_wstring(number) + L")");
}

}  // namespace recovery
