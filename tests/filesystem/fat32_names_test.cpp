#include "filesystem/fat32/fat32_names.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace recovery::filesystem::fat32 {
namespace {

std::vector<std::byte> name11(const char* text) {
    std::vector<std::byte> out(11);
    std::memcpy(out.data(), text, 11);
    return out;
}

TEST(Fat32NamesTest, LongNameChecksumMatchesReferenceValues) {
    // Reference values computed independently from the FAT specification algorithm.
    EXPECT_EQ(longNameChecksum(name11("README  TXT")), 0x73);
    EXPECT_EQ(longNameChecksum(name11("IMG_0001JPG")), 0x42);
    EXPECT_EQ(longNameChecksum(name11("FOO~1   JPE")), 0xD0);
}

TEST(Fat32NamesTest, FormatsShortNames) {
    EXPECT_EQ(formatShortName(name11("README  TXT"), 0), "README.TXT");
    EXPECT_EQ(formatShortName(name11("MAKEFILE   "), 0), "MAKEFILE");
    EXPECT_EQ(formatShortName(name11("A       B  "), 0), "A.B");
}

TEST(Fat32NamesTest, AppliesWindowsLowercaseFlags) {
    EXPECT_EQ(formatShortName(name11("README  TXT"), 0x08), "readme.TXT");
    EXPECT_EQ(formatShortName(name11("README  TXT"), 0x10), "README.txt");
    EXPECT_EQ(formatShortName(name11("README  TXT"), 0x18), "readme.txt");
}

TEST(Fat32NamesTest, DecodesOemCharacters) {
    std::vector<std::byte> name = name11("XBER    TXT");
    name[0] = std::byte{0x9A};  // CP437 'Ü'
    EXPECT_EQ(formatShortName(name, 0), "\xC3\x9C" "BER.TXT");

    name[0] = std::byte{0x05};  // escaped 0xE5, CP437 'σ'
    EXPECT_EQ(formatShortName(name, 0), "\xCF\x83" "BER.TXT");
    EXPECT_EQ(oemToUtf8(0xFF), "\xC2\xA0");
    EXPECT_EQ(oemToUtf8('A'), "A");
}

TEST(Fat32NamesTest, ValidShortNameBytes) {
    for (const char c : std::string("ABZ09$%'-_@~`!(){}^#& ")) {
        EXPECT_TRUE(isValidShortNameByte(static_cast<std::uint8_t>(c))) << c;
    }
    for (const char c : std::string("\"*+,./:;<=>?[\\]|")) {
        EXPECT_FALSE(isValidShortNameByte(static_cast<std::uint8_t>(c))) << c;
    }
    EXPECT_FALSE(isValidShortNameByte(0x00));
    EXPECT_FALSE(isValidShortNameByte(0x1F));
    EXPECT_TRUE(isValidShortNameByte(0xE0));
}

}  // namespace
}  // namespace recovery::filesystem::fat32
