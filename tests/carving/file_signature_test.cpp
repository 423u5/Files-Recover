// File signatures: matching (exact and masked), reach, the limits
// validateSignature enforces, and the construction helpers.

#include "carving/file_signature.hpp"

#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace recovery::carving {
namespace {

std::vector<std::byte> bytes(std::initializer_list<std::uint8_t> values) {
    std::vector<std::byte> out;
    for (const std::uint8_t value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

TEST(FileSignatureTest, MatchesItsPatternAtTheStartOfTheData) {
    const FileSignature signature = byteSignature("png", {0x89, 'P', 'N', 'G'});
    EXPECT_TRUE(signature.matches(bytes({0x89, 'P', 'N', 'G'})));
    EXPECT_TRUE(signature.matches(bytes({0x89, 'P', 'N', 'G', 0x0D, 0x0A})));
    EXPECT_FALSE(signature.matches(bytes({0x89, 'P', 'N', 'g'})));
    EXPECT_FALSE(signature.matches(bytes({0x00, 0x89, 'P', 'N', 'G'})));
}

TEST(FileSignatureTest, DataShorterThanThePatternNeverMatches) {
    const FileSignature signature = textSignature("gif", "GIF89a");
    EXPECT_FALSE(signature.matches(bytes({'G', 'I', 'F', '8', '9'})));
    EXPECT_FALSE(signature.matches({}));
}

TEST(FileSignatureTest, MaskedBitsAreIgnored) {
    // MPEG audio frame sync: eleven set bits.
    const FileSignature signature = byteSignature("sync", {0xFF, 0xE0}, 0, {0xFF, 0xE0});
    EXPECT_TRUE(signature.matches(bytes({0xFF, 0xE0})));
    EXPECT_TRUE(signature.matches(bytes({0xFF, 0xFB})));
    EXPECT_TRUE(signature.matches(bytes({0xFF, 0xFF})));
    EXPECT_FALSE(signature.matches(bytes({0xFF, 0xDF})));
    EXPECT_FALSE(signature.matches(bytes({0xFE, 0xE0})));
}

TEST(FileSignatureTest, ReachCoversOffsetAndPattern) {
    EXPECT_EQ(textSignature("ftyp", "ftyp", 4).reach(), 8u);
    EXPECT_EQ(textSignature("riff", "RIFF").reach(), 4u);
}

TEST(FileSignatureTest, HelpersBuildPatternMaskAndOffset) {
    const FileSignature text = textSignature("webp", "WEBP", 8);
    EXPECT_EQ(text.name, "webp");
    EXPECT_EQ(text.pattern, bytes({'W', 'E', 'B', 'P'}));
    EXPECT_TRUE(text.mask.empty());
    EXPECT_EQ(text.offset, 8u);

    const FileSignature masked = byteSignature("sync", {0xFF, 0xF0}, 2, {0xFF, 0xF6});
    EXPECT_EQ(masked.pattern, bytes({0xFF, 0xF0}));
    EXPECT_EQ(masked.mask, bytes({0xFF, 0xF6}));
    EXPECT_EQ(masked.offset, 2u);
}

TEST(FileSignatureTest, ValidSignaturesPass) {
    RECOVERY_EXPECT_OK(validateSignature(textSignature("two", "ab")));
    RECOVERY_EXPECT_OK(validateSignature(textSignature("max", std::string(FileSignature::kMaxLength, 'x'))));
    RECOVERY_EXPECT_OK(validateSignature(textSignature("far", "ab", FileSignature::kMaxOffset)));
    RECOVERY_EXPECT_OK(validateSignature(byteSignature("masked", {0xFF, 0xE0}, 0, {0xFF, 0x00})));
}

TEST(FileSignatureTest, RejectsSignaturesBreakingTheLimits) {
    RECOVERY_EXPECT_ERROR(validateSignature(textSignature("", "ab")), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(validateSignature(textSignature("short", "a")), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(validateSignature(textSignature("empty", "")), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(validateSignature(textSignature("long", std::string(FileSignature::kMaxLength + 1, 'x'))),
                          ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(validateSignature(textSignature("far", "ab", FileSignature::kMaxOffset + 1)),
                          ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(validateSignature(byteSignature("mask length", {0xFF, 0xE0}, 0, {0xFF})),
                          ErrorCode::InvalidInput);
    // The scanner indexes signatures by their first byte, so it must be compared in full.
    RECOVERY_EXPECT_ERROR(validateSignature(byteSignature("masked first", {0xFF, 0xE0}, 0, {0xF0, 0xE0})),
                          ErrorCode::InvalidInput);
}

}  // namespace
}  // namespace recovery::carving
