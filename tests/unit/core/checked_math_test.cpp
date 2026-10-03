#include "recovery/checked_math.hpp"
#include "recovery/strong_types.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace recovery {
namespace {

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

TEST(CheckedMathTest, AddDetectsOverflow) {
    EXPECT_EQ(checkedAdd<std::uint64_t>(1, 2), 3u);
    EXPECT_EQ(checkedAdd<std::uint64_t>(kMax - 1, 1), kMax);
    EXPECT_FALSE(checkedAdd<std::uint64_t>(kMax, 1).has_value());
    EXPECT_FALSE(checkedAdd<std::uint32_t>(0xFFFFFFFFu, 1u).has_value());
}

TEST(CheckedMathTest, MulDetectsOverflow) {
    EXPECT_EQ(checkedMul<std::uint64_t>(0, kMax), 0u);
    EXPECT_EQ(checkedMul<std::uint64_t>(kMax, 1), kMax);
    EXPECT_EQ(checkedMul<std::uint64_t>(512, 1024), 524288u);
    EXPECT_FALSE(checkedMul<std::uint64_t>(kMax / 2 + 1, 2).has_value());
    EXPECT_FALSE(checkedMul<std::uint64_t>(1ULL << 32, 1ULL << 32).has_value());
}

TEST(CheckedMathTest, RangeWithinHandlesEdges) {
    EXPECT_TRUE(rangeWithin<std::uint64_t>(0, 0, 0));
    EXPECT_TRUE(rangeWithin<std::uint64_t>(0, 100, 100));
    EXPECT_TRUE(rangeWithin<std::uint64_t>(100, 0, 100));
    EXPECT_FALSE(rangeWithin<std::uint64_t>(101, 0, 100));
    EXPECT_FALSE(rangeWithin<std::uint64_t>(1, 100, 100));
    // offset + length would wrap around to a small number.
    EXPECT_FALSE(rangeWithin<std::uint64_t>(10, kMax, 100));
    EXPECT_FALSE(rangeWithin<std::uint64_t>(kMax, 1, kMax));
}

TEST(StrongTypesTest, CompareByValue) {
    EXPECT_EQ(ByteOffset{5}, ByteOffset{5});
    EXPECT_LT(SectorNumber{1}, SectorNumber{2});
    EXPECT_EQ(SectorCount{}.value(), 0u);
}

}  // namespace
}  // namespace recovery
