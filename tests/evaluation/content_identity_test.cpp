// Content identity (P14): SHA-256 of exactly the content, the preliminary
// hash (size and the CRC-32 of the first and last 64 KiB), and duplicate
// detection by digest.

#include "evaluation/content_identity.hpp"

#include "carving/content_reader.hpp"
#include "recovery/crc32.hpp"
#include "recovery/sha256.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <vector>

namespace recovery::evaluation {
namespace {

using Bytes = std::vector<std::byte>;

Bytes text(std::string_view value) {
    Bytes out;
    for (const char c : value) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

ContentIdentity identityOf(const Bytes& data, IdentityOptions options = {}) {
    carving::MemoryContentReader content(data);
    Result<ContentIdentity> identity = computeIdentity(content, options);
    EXPECT_TRUE(identity.ok());
    return identity.ok() ? *identity : ContentIdentity{};
}

TEST(ContentIdentityTest, Sha256IsOfExactlyTheContent) {
    const ContentIdentity abc = identityOf(text("abc"));
    ASSERT_TRUE(abc.sha256.has_value());
    EXPECT_EQ(abc.sha256->hex(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(abc.size, 3U);
    const ContentIdentity empty = identityOf({});
    ASSERT_TRUE(empty.sha256.has_value());
    EXPECT_EQ(empty.sha256->hex(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // Several reads (more than one chunk of the reader).
    const Bytes large = test::makePattern(3 * carving::IContentReader::kMaxReadLength + 12345, 7);
    EXPECT_EQ(identityOf(large).sha256, sha256(large));
}

TEST(ContentIdentityTest, ThePreliminaryHashIsTheSizeAndTheEnds) {
    const Bytes small = test::makePattern(1000, 1);
    const ContentIdentity a = identityOf(small);
    ASSERT_TRUE(a.preliminary.has_value());
    EXPECT_EQ(a.preliminary->size, 1000U);
    EXPECT_EQ(a.preliminary->head, crc32(small));
    EXPECT_EQ(a.preliminary->tail, crc32(small));
    const Bytes large = test::makePattern(200 * 1024, 2);
    const ContentIdentity b = identityOf(large);
    ASSERT_TRUE(b.preliminary.has_value());
    const std::size_t window = PreliminaryHash::kWindow;
    EXPECT_EQ(b.preliminary->head, crc32(std::span(large).first(window)));
    EXPECT_EQ(b.preliminary->tail, crc32(std::span(large).last(window)));
    // Without SHA-256 the preliminary hash is still computed, and is the same.
    const ContentIdentity quick = identityOf(large, IdentityOptions{false, true});
    EXPECT_FALSE(quick.sha256.has_value());
    EXPECT_EQ(quick.preliminary, b.preliminary);
    // A change in the middle leaves it alone; SHA-256 tells.
    Bytes middle = large;
    middle[100 * 1024] ^= std::byte{1};
    const ContentIdentity changed = identityOf(middle);
    EXPECT_EQ(changed.preliminary, b.preliminary);
    EXPECT_NE(changed.sha256, b.sha256);
    // Neither: only the size.
    const ContentIdentity none = identityOf(large, IdentityOptions{false, false});
    EXPECT_FALSE(none.sha256.has_value());
    EXPECT_FALSE(none.preliminary.has_value());
    EXPECT_EQ(none.size, large.size());
}

TEST(ContentIdentityTest, DuplicatesAreLaterCandidatesWithTheSameDigest) {
    DuplicateIndex index;
    const ContentIdentity first = identityOf(text("the same bytes"));
    const ContentIdentity other = identityOf(text("other bytes"));
    EXPECT_FALSE(index.add(first, 1).has_value());
    EXPECT_FALSE(index.add(other, 2).has_value());
    EXPECT_EQ(index.add(first, 3), std::optional<std::uint64_t>{1});
    EXPECT_EQ(index.add(first, 4), std::optional<std::uint64_t>{1});
    EXPECT_EQ(index.originalOf(other), std::optional<std::uint64_t>{2});
    EXPECT_EQ(index.size(), 2U);
    // Empty content and content without a digest are never duplicates.
    const ContentIdentity empty = identityOf({});
    EXPECT_FALSE(index.add(empty, 5).has_value());
    EXPECT_FALSE(index.add(empty, 6).has_value());
    const ContentIdentity unhashed = identityOf(text("the same bytes"), IdentityOptions{false, true});
    EXPECT_FALSE(index.add(unhashed, 7).has_value());
    EXPECT_FALSE(index.originalOf(unhashed).has_value());
    EXPECT_EQ(index.size(), 2U);
}

}  // namespace
}  // namespace recovery::evaluation
