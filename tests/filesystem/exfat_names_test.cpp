// exFAT checksums, name hashes and up-case tables. Expected values were
// computed independently from the specification's pseudo-code.

#include "filesystem/exfat/exfat_names.hpp"

#include "recovery/byte_order.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace recovery::filesystem::exfat {
namespace {

std::vector<std::byte> sequence(std::size_t size, unsigned multiplier, unsigned offset) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * multiplier + offset) & 0xFF);
    }
    return bytes;
}

std::vector<std::byte> units(std::initializer_list<char16_t> values) {
    std::vector<std::byte> bytes(values.size() * 2);
    std::size_t offset = 0;
    for (const char16_t value : values) {
        storeLe16(bytes, offset, value);
        offset += 2;
    }
    return bytes;
}

TEST(ExFatNamesTest, EntrySetChecksumMatchesReferenceValues) {
    EXPECT_EQ(entrySetChecksum(sequence(64, 1, 0)), 0x8047);
    EXPECT_EQ(entrySetChecksum(sequence(96, 7, 3)), 0x3FC3);
}

TEST(ExFatNamesTest, EntrySetChecksumSkipsItsOwnField) {
    std::vector<std::byte> set = sequence(96, 7, 3);
    const std::uint16_t before = entrySetChecksum(set);
    set[2] = std::byte{0xAA};
    set[3] = std::byte{0x55};
    EXPECT_EQ(entrySetChecksum(set), before);
    set[4] ^= std::byte{1};
    EXPECT_NE(entrySetChecksum(set), before);
}

TEST(ExFatNamesTest, UpcaseTableChecksumMatchesReferenceValue) {
    EXPECT_EQ(upcaseTableChecksum(sequence(256, 1, 0)), 0x00000200u);
}

TEST(ExFatNamesTest, NameHashMatchesReferenceValues) {
    const UpcaseTable table = UpcaseTable::basic();
    EXPECT_EQ(nameHash(u"A", table), 0x8020);
    EXPECT_EQ(nameHash(u"abc", table), 0xC82B);
    EXPECT_EQ(nameHash(u"ABC", table), 0xC82B);  // hashes are case-insensitive
    EXPECT_EQ(nameHash(u"IMG_0001.JPG", table), 0xA6CB);
    EXPECT_EQ(nameHash(u"été.txt", table), 0x7180);
    EXPECT_EQ(nameHash(u"ÉTÉ.TXT", table), 0x7180);
    EXPECT_EQ(nameHash(u"ÿ", table), 0x003D);  // maps to U+0178
}

TEST(ExFatNamesTest, BasicTableCoversOnlyLatin1) {
    const UpcaseTable table = UpcaseTable::basic();
    EXPECT_TRUE(table.isBasic());
    EXPECT_EQ(table.map(u'q'), u'Q');
    EXPECT_EQ(table.map(u'Q'), u'Q');
    EXPECT_EQ(table.map(0xF7), 0xF7);  // division sign
    EXPECT_EQ(table.map(0x3B1), 0x3B1);  // Greek alpha: unknown to the fallback
    EXPECT_TRUE(table.covers(0xFF));
    EXPECT_FALSE(table.covers(0x3B1));
}

TEST(ExFatNamesTest, DecodesUncompressedTable) {
    // Stores only the first four entries (U+0001 maps to 'B'); the rest are identity.
    const std::optional<UpcaseTable> table = UpcaseTable::decode(units({0x0000, 0x0042, 0x0002, 0x0003}));
    ASSERT_TRUE(table.has_value());
    EXPECT_FALSE(table->isBasic());
    EXPECT_EQ(table->map(0x0001), 0x0042);
    EXPECT_EQ(table->map(0x0003), 0x0003);
    EXPECT_EQ(table->map(u'a'), u'a');  // beyond the stored table
}

TEST(ExFatNamesTest, DecodesCompressedTable) {
    // Identity for 0x0000-0x0060, then 'a'->'A', 'b'->'B', then identity for 0x0063-0x03B0, then alpha->Alpha.
    const std::optional<UpcaseTable> table =
        UpcaseTable::decode(units({0xFFFF, 0x0061, u'A', u'B', 0xFFFF, 0x03B1 - 0x0063, 0x0391}));
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ(table->map(u'a'), u'A');
    EXPECT_EQ(table->map(u'b'), u'B');
    EXPECT_EQ(table->map(u'c'), u'c');
    EXPECT_EQ(table->map(0x3B1), 0x391);
    EXPECT_EQ(table->map(0x3B2), 0x3B2);
    EXPECT_EQ(nameHash(u"ab", *table), nameHash(u"AB", *table));
}

TEST(ExFatNamesTest, IdentityRunPastTheEndIsBounded) {
    const std::optional<UpcaseTable> table = UpcaseTable::decode(units({0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, u'X'}));
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ(table->map(0xFFFE), 0xFFFE);
}

TEST(ExFatNamesTest, RejectsMalformedTables) {
    EXPECT_FALSE(UpcaseTable::decode({}).has_value());
    EXPECT_FALSE(UpcaseTable::decode(std::vector<std::byte>(3)).has_value());
    EXPECT_FALSE(UpcaseTable::decode(std::vector<std::byte>(kMaxUpcaseTableBytes + 2)).has_value());
    EXPECT_TRUE(UpcaseTable::decode(std::vector<std::byte>(kMaxUpcaseTableBytes)).has_value());
}

}  // namespace
}  // namespace recovery::filesystem::exfat
