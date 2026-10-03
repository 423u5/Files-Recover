// SHA-256 against the FIPS 180-4 example messages and digests made by
// Python's hashlib, fed whole, in pieces and with zero runs.

#include "recovery/sha256.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace recovery {
namespace {

std::vector<std::byte> bytesOf(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(text[i]);
    }
    return out;
}

// Byte i is (i * 131 + 7) mod 256, as in the hashlib script that made the references.
std::vector<std::byte> pattern(std::size_t size) {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((i * 131 + 7) & 0xFF);
    }
    return out;
}

TEST(Sha256Test, FipsExampleMessages) {
    EXPECT_EQ(sha256({}).hex(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256(bytesOf("abc")).hex(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256(bytesOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")).hex(),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(sha256(bytesOf("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmn"
                             "opqrsmnopqrstnopqrstu"))
                  .hex(),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(Sha256Test, OneMillionAs) {
    const std::vector<std::byte> block = bytesOf(std::string(1000, 'a'));
    Sha256 hash;
    for (int i = 0; i < 1000; ++i) {
        hash.update(block);
    }
    EXPECT_EQ(hash.size(), 1'000'000U);
    EXPECT_EQ(hash.finish().hex(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256Test, EveryLengthAroundTheBlockAndPaddingBoundaries) {
    // The digests of every prefix of the pattern, 0 to 300 bytes (lengths 55,
    // 56, 63, 64 and 65 put the padding in every place), hashed together.
    const std::vector<std::byte> data = pattern(300);
    Sha256 fold;
    for (std::size_t n = 0; n <= data.size(); ++n) {
        const Sha256Digest digest = sha256(std::span(data).first(n));
        fold.update(std::as_bytes(std::span(digest.bytes())));
    }
    EXPECT_EQ(fold.finish().hex(), "7722024365d27836079cbf29de35d060b9383d7c713083199661392f983216d4");
}

TEST(Sha256Test, PiecesGiveTheDigestOfTheWhole) {
    const std::vector<std::byte> data = pattern(1000);
    const Sha256Digest whole = sha256(data);
    for (std::size_t split = 0; split <= data.size(); split += 7) {
        Sha256 hash;
        hash.update(std::span(data).first(split));
        hash.update(std::span(data).subspan(split));
        EXPECT_EQ(hash.finish(), whole) << "split at " << split;
    }
    Sha256 bytewise;
    for (const std::byte b : data) {
        bytewise.update(std::span(&b, 1));
    }
    EXPECT_EQ(bytewise.finish(), whole);
}

TEST(Sha256Test, ZeroRunsHashLikeZeroBytes) {
    Sha256 hash;
    hash.updateZeros(100'000);
    EXPECT_EQ(hash.size(), 100'000U);
    EXPECT_EQ(hash.finish().hex(), "9192c25b734fcbadbe32dadc28089c60db0e39f90cc20ce2e5733f57261acc0c");

    // Mixed with data, at unaligned positions.
    const std::vector<std::byte> head = pattern(37);
    std::vector<std::byte> expected = head;
    expected.resize(expected.size() + 1000);
    const std::vector<std::byte> tail = pattern(5);
    expected.insert(expected.end(), tail.begin(), tail.end());
    Sha256 mixed;
    mixed.update(head);
    mixed.updateZeros(1000);
    mixed.update(tail);
    EXPECT_EQ(mixed.finish(), sha256(expected));
}

TEST(Sha256Test, FinishStartsOver) {
    Sha256 hash;
    hash.update(bytesOf("abc"));
    const Sha256Digest first = hash.finish();
    EXPECT_EQ(hash.size(), 0U);
    hash.update(bytesOf("abc"));
    EXPECT_EQ(hash.finish(), first);
    hash.update(bytesOf("x"));
    hash.reset();
    EXPECT_EQ(hash.finish(), sha256({}));
}

TEST(Sha256Test, DigestsOrderAndCompare) {
    const Sha256Digest a = sha256(bytesOf("a"));
    const Sha256Digest b = sha256(bytesOf("b"));
    EXPECT_NE(a, b);
    EXPECT_EQ(a, sha256(bytesOf("a")));
    const std::set<Sha256Digest> digests = {a, b, sha256(bytesOf("a"))};
    EXPECT_EQ(digests.size(), 2U);
    EXPECT_EQ(Sha256Digest{}.hex(), std::string(64, '0'));
}

}  // namespace
}  // namespace recovery
