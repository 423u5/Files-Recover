// Byte order, CRC-32, UTF-16 and GUID helpers used by the on-disk parsers.

#include "partition/guid.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/crc32.hpp"
#include "recovery/unicode.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace recovery {
namespace {

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

TEST(ByteOrderTest, LoadsLittleEndian) {
    const auto data = bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xFF});
    EXPECT_EQ(loadU8(data, 8), 0xFFu);
    EXPECT_EQ(loadLe16(data, 0), 0x0201u);
    EXPECT_EQ(loadLe32(data, 1), 0x05040302u);
    EXPECT_EQ(loadLe64(data, 0), 0x0807060504030201ULL);
}

TEST(ByteOrderTest, StoreRoundTrips) {
    std::vector<std::byte> data(16);
    storeLe16(data, 0, 0xBEEF);
    storeLe32(data, 2, 0xDEADBEEF);
    storeLe64(data, 6, 0x0123456789ABCDEFULL);
    EXPECT_EQ(loadLe16(data, 0), 0xBEEFu);
    EXPECT_EQ(loadLe32(data, 2), 0xDEADBEEFu);
    EXPECT_EQ(loadLe64(data, 6), 0x0123456789ABCDEFULL);
}

TEST(ByteOrderTest, LoadsBigEndian) {
    const auto data = bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0xFF});
    EXPECT_EQ(loadBe16(data, 0), 0x0102u);
    EXPECT_EQ(loadBe16(data, 4), 0x05FFu);
    EXPECT_EQ(loadBe32(data, 1), 0x02030405u);
    EXPECT_EQ(loadBe32(data, 2), 0x030405FFu);
    const auto wide = bytes({0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF});
    EXPECT_EQ(loadBe64(wide, 1), 0x0123456789ABCDEFULL);
}

TEST(ByteOrderTest, BigEndianStoreRoundTrips) {
    std::vector<std::byte> data(6);
    storeBe16(data, 0, 0xBEEF);
    storeBe32(data, 2, 0xDEADBEEF);
    EXPECT_EQ(data, bytes({0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF}));
    EXPECT_EQ(loadBe16(data, 0), 0xBEEFu);
    EXPECT_EQ(loadBe32(data, 2), 0xDEADBEEFu);
    std::vector<std::byte> wide(8);
    storeBe64(wide, 0, 0x0123456789ABCDEFULL);
    EXPECT_EQ(wide, bytes({0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}));
    EXPECT_EQ(loadBe64(wide, 0), 0x0123456789ABCDEFULL);
}

TEST(ByteOrderDeathTest, OutOfBoundsFieldTerminates) {
    const auto data = bytes({1, 2, 3});
    EXPECT_DEATH({ (void)loadLe32(data, 0); }, "out of bounds");
    EXPECT_DEATH({ (void)loadLe16(data, static_cast<std::size_t>(-1)); }, "out of bounds");
    EXPECT_DEATH({ (void)loadBe32(data, 0); }, "out of bounds");
    EXPECT_DEATH({ (void)loadBe16(data, 2); }, "out of bounds");
    EXPECT_DEATH({ (void)loadBe64(data, 0); }, "out of bounds");
}

TEST(Crc32Test, KnownVectors) {
    const std::string check = "123456789";
    EXPECT_EQ(crc32(std::as_bytes(std::span(check))), 0xCBF43926u);
    EXPECT_EQ(crc32({}), 0u);
    const std::string fox = "The quick brown fox jumps over the lazy dog";
    EXPECT_EQ(crc32(std::as_bytes(std::span(fox))), 0x414FA339u);
}

TEST(Crc32Test, IncrementalMatchesOneShot) {
    const std::string text = "The quick brown fox jumps over the lazy dog";
    const auto all = std::as_bytes(std::span(text));
    const std::uint32_t partial = crc32Update(crc32Update(0, all.first(10)), all.subspan(10));
    EXPECT_EQ(partial, crc32(all));
}

TEST(UnicodeTest, ConvertsUtf16) {
    EXPECT_EQ(utf16ToUtf8(std::u16string(u"abc")), "abc");
    EXPECT_EQ(utf16ToUtf8(std::u16string(u"é写")), "\xC3\xA9\xE5\x86\x99");
    EXPECT_EQ(utf16ToUtf8(std::u16string(u"\U0001F600")), "\xF0\x9F\x98\x80");
}

TEST(UnicodeTest, UnpairedSurrogatesBecomeReplacementCharacter) {
    const std::u16string loneHigh = {0xD83D, u'a'};
    const std::u16string loneLow = {0xDE00};
    const std::u16string reversed = {0xDE00, 0xD83D};
    EXPECT_EQ(utf16ToUtf8(loneHigh), "\xEF\xBF\xBD" "a");
    EXPECT_EQ(utf16ToUtf8(loneLow), "\xEF\xBF\xBD");
    EXPECT_EQ(utf16ToUtf8(reversed), "\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(UnicodeTest, AppendUtf8RejectsInvalidCodePoints) {
    std::string out;
    appendUtf8(out, 0x110000);
    appendUtf8(out, 0xD800);
    EXPECT_EQ(out, "\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(UnicodeTest, LoadsUtf16LittleEndian) {
    const auto data = bytes({0x41, 0x00, 0xE9, 0x00, 0x99, 0x51});
    EXPECT_EQ(loadUtf16Le(data, 3), std::u16string(u"Aé写"));
}

TEST(GuidTest, ParsesToMixedEndianDiskLayout) {
    const auto guid = partition::Guid::parse("C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
    ASSERT_TRUE(guid.has_value());
    const auto expected = bytes({0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E,
                                 0xC9, 0x3B});
    EXPECT_TRUE(std::equal(guid->bytes().begin(), guid->bytes().end(), expected.begin()));
    EXPECT_EQ(partition::Guid::fromDisk(expected), *guid);
    EXPECT_EQ(guid->toString(), "C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
    EXPECT_EQ(partition::Guid::parse("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"), guid);
}

TEST(GuidTest, RejectsMalformedText) {
    for (const char* text : {"", "C12A7328F81F11D2BA4B00A0C93EC93B", "C12A7328-F81F-11D2-BA4B-00A0C93EC93",
                             "C12A7328-F81F-11D2-BA4B-00A0C93EC93BX", "G12A7328-F81F-11D2-BA4B-00A0C93EC93B",
                             "C12A732-8F81F-11D2-BA4B-00A0C93EC93B"}) {
        EXPECT_FALSE(partition::Guid::parse(text).has_value()) << text;
    }
    EXPECT_TRUE(partition::Guid{}.isZero());
    EXPECT_FALSE(partition::gpt_types::kEfiSystem.isZero());
}

}  // namespace
}  // namespace recovery
