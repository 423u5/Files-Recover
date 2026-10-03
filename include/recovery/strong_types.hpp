#pragma once

#include <compare>
#include <cstdint>

namespace recovery {

// A value that cannot be silently mixed with other values of the same
// representation (e.g. a byte offset passed where a sector number is expected).
template <typename Tag, typename Rep>
class StrongValue {
public:
    using RepType = Rep;

    constexpr StrongValue() noexcept = default;
    constexpr explicit StrongValue(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

    friend constexpr bool operator==(const StrongValue&, const StrongValue&) noexcept = default;
    friend constexpr auto operator<=>(const StrongValue&, const StrongValue&) noexcept = default;

private:
    Rep value_{};
};

using ByteOffset = StrongValue<struct ByteOffsetTag, std::uint64_t>;
using SectorNumber = StrongValue<struct SectorNumberTag, std::uint64_t>;
using SectorCount = StrongValue<struct SectorCountTag, std::uint64_t>;

}  // namespace recovery
