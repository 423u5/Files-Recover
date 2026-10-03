#pragma once

// Names for recovered files on a Windows destination.
//
// Names read from a filesystem are untrusted: a damaged or crafted name may
// hold path separators, "..", characters Windows forbids, or reserved device
// names. Recovery output uses these functions so that a name always becomes
// exactly one ordinary path component inside the destination.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace recovery {

// Longest name component Windows accepts, in UTF-16 code units.
inline constexpr std::size_t kMaxNameLength = 255;

// Name used when nothing of the original name is usable.
inline constexpr std::wstring_view kUnnamed = L"unnamed";

// Converts an original name (UTF-8; invalid sequences become U+FFFD) into a
// single Windows path component:
//  * control characters and < > : " / \ | ? * become '_';
//  * trailing dots and spaces are removed (Windows would drop them);
//  * reserved device names (CON, PRN, AUX, NUL, COM0-9, LPT0-9, CONIN$,
//    CONOUT$, also with an extension) get a leading '_';
//  * an empty result, "." and ".." become "_", and an empty input kUnnamed;
//  * names longer than kMaxNameLength are shortened, keeping the extension.
[[nodiscard]] std::wstring safeFileName(std::string_view originalName);

// The name to try after `number` collisions: `safeName` itself for 0, then
// "stem (1).ext", "stem (2).ext", ... The stem is shortened when needed, so
// the result stays within kMaxNameLength.
[[nodiscard]] std::wstring numberedName(std::wstring_view safeName, std::uint32_t number);

}  // namespace recovery
