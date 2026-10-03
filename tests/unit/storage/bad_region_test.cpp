#include "storage/bad_region.hpp"

#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace recovery::storage {
namespace {

TEST(BadRegionMapTest, StartsEmpty) {
    const BadRegionMap map;
    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.totalBytes(), 0u);
    EXPECT_FALSE(map.intersects(0, 100));
}

TEST(BadRegionMapTest, RejectsInvalidRegions) {
    BadRegionMap map;
    RECOVERY_EXPECT_ERROR(map.add({100, 0, 23}), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(map.add({std::numeric_limits<std::uint64_t>::max(), 2, 23}), ErrorCode::InvalidInput);
    EXPECT_TRUE(map.empty());
}

TEST(BadRegionMapTest, MergesAdjacentRegionsWithSameCode) {
    BadRegionMap map;
    RECOVERY_ASSERT_OK(map.add({512, 512, 23}));
    RECOVERY_ASSERT_OK(map.add({1024, 512, 23}));
    RECOVERY_ASSERT_OK(map.add({0, 512, 23}));
    ASSERT_EQ(map.size(), 1u);
    EXPECT_EQ(map.regions()[0], (BadRegion{0, 1536, 23}));
    EXPECT_EQ(map.totalBytes(), 1536u);
}

TEST(BadRegionMapTest, KeepsAdjacentRegionsWithDifferentCodesSeparate) {
    BadRegionMap map;
    RECOVERY_ASSERT_OK(map.add({0, 512, 23}));
    RECOVERY_ASSERT_OK(map.add({512, 512, 27}));
    ASSERT_EQ(map.size(), 2u);
    EXPECT_EQ(map.totalBytes(), 1024u);
}

TEST(BadRegionMapTest, MergesOverlapsKeepingFirstCode) {
    BadRegionMap map;
    RECOVERY_ASSERT_OK(map.add({0, 1000, 23}));
    RECOVERY_ASSERT_OK(map.add({500, 1000, 27}));
    ASSERT_EQ(map.size(), 1u);
    EXPECT_EQ(map.regions()[0], (BadRegion{0, 1500, 23}));
    EXPECT_EQ(map.totalBytes(), 1500u);
}

TEST(BadRegionMapTest, RegionSpanningSeveralExistingOnesCollapsesThem) {
    BadRegionMap map;
    RECOVERY_ASSERT_OK(map.add({0, 10, 1}));
    RECOVERY_ASSERT_OK(map.add({20, 10, 2}));
    RECOVERY_ASSERT_OK(map.add({40, 10, 3}));
    RECOVERY_ASSERT_OK(map.add({5, 40, 9}));
    ASSERT_EQ(map.size(), 1u);
    EXPECT_EQ(map.regions()[0], (BadRegion{0, 50, 1}));
    EXPECT_EQ(map.totalBytes(), 50u);
}

TEST(BadRegionMapTest, DuplicateAddIsIdempotent) {
    BadRegionMap map;
    RECOVERY_ASSERT_OK(map.add({4096, 512, 23}));
    RECOVERY_ASSERT_OK(map.add({4096, 512, 23}));
    EXPECT_EQ(map.size(), 1u);
    EXPECT_EQ(map.totalBytes(), 512u);
}

TEST(BadRegionMapTest, IntersectionQueries) {
    BadRegionMap map;
    RECOVERY_ASSERT_OK(map.add({1000, 100, 23}));
    RECOVERY_ASSERT_OK(map.add({5000, 100, 23}));

    EXPECT_FALSE(map.intersects(0, 1000));
    EXPECT_TRUE(map.intersects(0, 1001));
    EXPECT_TRUE(map.intersects(1099, 1));
    EXPECT_FALSE(map.intersects(1100, 3900));
    EXPECT_TRUE(map.intersects(1050, 10));
    EXPECT_FALSE(map.intersects(1050, 0));
    EXPECT_TRUE(map.intersects(0, std::numeric_limits<std::uint64_t>::max()));

    const auto hits = map.overlapping(1050, 4000);
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].offset, 1000u);
    EXPECT_EQ(hits[1].offset, 5000u);
    EXPECT_TRUE(map.overlapping(2000, 100).empty());
}

}  // namespace
}  // namespace recovery::storage
