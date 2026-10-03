// Names of recovered files: untrusted names become one safe Windows path
// component, and collisions get numbered names within the length limit.

#include "recovery/output_names.hpp"

#include <gtest/gtest.h>

#include <string>

namespace recovery {
namespace {

TEST(OutputNamesTest, OrdinaryNamesAreKept) {
    EXPECT_EQ(safeFileName("IMG_0001.JPG"), L"IMG_0001.JPG");
    EXPECT_EQ(safeFileName("My Photo (2).jpeg"), L"My Photo (2).jpeg");
    EXPECT_EQ(safeFileName(".bashrc"), L".bashrc");
    EXPECT_EQ(safeFileName(" leading space.txt"), L" leading space.txt");
    EXPECT_EQ(safeFileName("$OrphanFiles"), L"$OrphanFiles");
}

TEST(OutputNamesTest, UnicodeIsConvertedExactly) {
    EXPECT_EQ(safeFileName("Фото 日本.jpg"), L"Фото 日本.jpg");
    // Outside the BMP: a surrogate pair.
    EXPECT_EQ(safeFileName("beach 🏖.png"), std::wstring(L"beach \xD83C\xDFD6.png"));
}

TEST(OutputNamesTest, InvalidUtf8BecomesReplacementCharacters) {
    EXPECT_EQ(safeFileName("a\xFF" "b.txt"), L"a\uFFFDb.txt");
    EXPECT_EQ(safeFileName("\xC3"), L"\uFFFD");               // truncated sequence
    EXPECT_EQ(safeFileName("\xC0\xAF.txt"), L"\uFFFD.txt");   // overlong '/'
    EXPECT_EQ(safeFileName("\xED\xA0\x80.txt"), L"\uFFFD.txt");  // encoded surrogate
}

TEST(OutputNamesTest, SeparatorsAndForbiddenCharactersAreReplaced) {
    EXPECT_EQ(safeFileName("../../Windows/evil.dll"), L".._.._Windows_evil.dll");
    EXPECT_EQ(safeFileName("a\\b"), L"a_b");
    EXPECT_EQ(safeFileName("C:evil"), L"C_evil");
    EXPECT_EQ(safeFileName("file.txt:stream"), L"file.txt_stream");
    EXPECT_EQ(safeFileName("a<b>c\"d|e?f*g"), L"a_b_c_d_e_f_g");
    EXPECT_EQ(safeFileName(std::string("tab\there\x01") + "x"), L"tab_here_x");
}

TEST(OutputNamesTest, TrailingDotsAndSpacesAreRemoved) {
    EXPECT_EQ(safeFileName("name. . "), L"name");
    EXPECT_EQ(safeFileName("."), L"_");
    EXPECT_EQ(safeFileName(".."), L"_");
    EXPECT_EQ(safeFileName("   "), L"_");
    EXPECT_EQ(safeFileName(""), std::wstring(kUnnamed));
}

TEST(OutputNamesTest, ReservedDeviceNamesArePrefixed) {
    for (const char* name : {"CON", "con", "PRN", "AUX", "NUL", "COM1", "com9", "LPT0", "CONIN$", "conout$"}) {
        const std::wstring safe = safeFileName(name);
        ASSERT_FALSE(safe.empty());
        EXPECT_EQ(safe[0], L'_') << name;
    }
    EXPECT_EQ(safeFileName("nul.txt"), L"_nul.txt");
    EXPECT_EQ(safeFileName("NUL .tar.gz"), L"_NUL .tar.gz");
    EXPECT_EQ(safeFileName("COM\xC2\xB9"), L"_COM\u00B9");  // COM + superscript one
    EXPECT_EQ(safeFileName("CONSOLE.txt"), L"CONSOLE.txt");
    EXPECT_EQ(safeFileName("COM10"), L"COM10");
    EXPECT_EQ(safeFileName("LPT"), L"LPT");
}

TEST(OutputNamesTest, LongNamesAreShortenedKeepingTheExtension) {
    const std::string longName = std::string(300, 'a') + ".jpg";
    const std::wstring safe = safeFileName(longName);
    EXPECT_EQ(safe.size(), kMaxNameLength);
    EXPECT_TRUE(safe.ends_with(L".jpg"));

    // A surrogate pair straddling the limit is dropped whole.
    std::string emoji;
    for (int i = 0; i < 200; ++i) {
        emoji += "😀";  // two UTF-16 units each
    }
    const std::wstring shortened = safeFileName(emoji);
    EXPECT_LE(shortened.size(), kMaxNameLength);
    EXPECT_EQ(shortened.size() % 2, 0u);
    EXPECT_TRUE(shortened.back() >= 0xDC00 && shortened.back() <= 0xDFFF);

    // An absurdly long "extension" is not kept.
    const std::wstring noExtension = safeFileName("x." + std::string(400, 'e'));
    EXPECT_EQ(noExtension.size(), kMaxNameLength);
}

TEST(OutputNamesTest, NumberedNames) {
    EXPECT_EQ(numberedName(L"photo.jpg", 0), L"photo.jpg");
    EXPECT_EQ(numberedName(L"photo.jpg", 1), L"photo (1).jpg");
    EXPECT_EQ(numberedName(L"photo.jpg", 12), L"photo (12).jpg");
    EXPECT_EQ(numberedName(L"archive.tar.gz", 2), L"archive.tar (2).gz");
    EXPECT_EQ(numberedName(L".bashrc", 1), L".bashrc (1)");
    EXPECT_EQ(numberedName(L"README", 3), L"README (3)");

    const std::wstring longName = std::wstring(kMaxNameLength - 4, L'a') + L".jpg";
    const std::wstring numbered = numberedName(longName, 9999);
    EXPECT_EQ(numbered.size(), kMaxNameLength);
    EXPECT_TRUE(numbered.ends_with(L" (9999).jpg"));
}

}  // namespace
}  // namespace recovery
