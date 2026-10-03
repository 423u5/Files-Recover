#pragma once

#include <concepts>
#include <limits>
#include <optional>

namespace recovery {

// Overflow-checked arithmetic for values derived from untrusted on-disk data.

template <std::unsigned_integral T>
[[nodiscard]] constexpr std::optional<T> checkedAdd(T a, T b) noexcept {
    if (b > std::numeric_limits<T>::max() - a) {
        return std::nullopt;
    }
    return static_cast<T>(a + b);
}

template <std::unsigned_integral T>
[[nodiscard]] constexpr std::optional<T> checkedMul(T a, T b) noexcept {
    if (a != 0 && b > std::numeric_limits<T>::max() / a) {
        return std::nullopt;
    }
    return static_cast<T>(a * b);
}

// True when [offset, offset + length) lies inside [0, total). Never overflows.
template <std::unsigned_integral T>
[[nodiscard]] constexpr bool rangeWithin(T offset, T length, T total) noexcept {
    return offset <= total && length <= total - offset;
}

}  // namespace recovery
