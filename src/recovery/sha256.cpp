#include "recovery/sha256.hpp"

#include <algorithm>
#include <bit>

namespace recovery {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
};

constexpr std::array<std::uint32_t, 8> kInitialState = {
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
};

constexpr std::array<std::uint8_t, 64> kZeros{};

}  // namespace

std::string Sha256Digest::hex() const {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(kSize * 2);
    for (const std::uint8_t value : bytes_) {
        text.push_back(kDigits[value >> 4]);
        text.push_back(kDigits[value & 0x0F]);
    }
    return text;
}

Sha256::Sha256() noexcept {
    reset();
}

void Sha256::reset() noexcept {
    state_ = kInitialState;
    buffered_ = 0;
    length_ = 0;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    std::size_t index = 0;
    for (std::uint32_t& word : schedule) {
        if (index < 16) {
            const std::uint8_t* bytes = block + index * 4;
            word = (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) | (std::uint32_t{bytes[2]} << 8) |
                   std::uint32_t{bytes[3]};
        } else {
            const std::uint32_t w15 = schedule[index - 15];
            const std::uint32_t w2 = schedule[index - 2];
            const std::uint32_t s0 = std::rotr(w15, 7) ^ std::rotr(w15, 18) ^ (w15 >> 3);
            const std::uint32_t s1 = std::rotr(w2, 17) ^ std::rotr(w2, 19) ^ (w2 >> 10);
            word = schedule[index - 16] + s0 + schedule[index - 7] + s1;
        }
        ++index;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    index = 0;
    for (const std::uint32_t constant : kRoundConstants) {
        const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t choice = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + choice + constant + schedule[index];
        const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
        ++index;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(std::span<const std::byte> data) noexcept {
    length_ += data.size();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t left = data.size();
    if (buffered_ > 0) {
        const std::size_t take = std::min(left, buffer_.size() - buffered_);
        std::copy_n(bytes, take, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += take;
        bytes += take;
        left -= take;
        if (buffered_ < buffer_.size()) {
            return;
        }
        compress(buffer_.data());
        buffered_ = 0;
    }
    while (left >= buffer_.size()) {
        compress(bytes);
        bytes += buffer_.size();
        left -= buffer_.size();
    }
    std::copy_n(bytes, left, buffer_.begin());
    buffered_ = left;
}

void Sha256::updateZeros(std::uint64_t count) noexcept {
    while (count > 0) {
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(count, kZeros.size()));
        update(std::as_bytes(std::span(kZeros).first(take)));
        count -= take;
    }
}

Sha256Digest Sha256::finish() noexcept {
    const std::uint64_t bits = length_ << 3;
    // The message, a 1 bit, zeros up to 56 bytes mod 64, the length in bits.
    buffer_[buffered_++] = 0x80;
    if (buffered_ > 56) {
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end(), std::uint8_t{0});
        compress(buffer_.data());
        buffered_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.begin() + 56, std::uint8_t{0});
    for (std::size_t i = 0; i < 8; ++i) {
        buffer_[56 + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    }
    compress(buffer_.data());

    std::array<std::uint8_t, Sha256Digest::kSize> digest{};
    std::size_t index = 0;
    for (const std::uint32_t word : state_) {
        digest[index++] = static_cast<std::uint8_t>(word >> 24);
        digest[index++] = static_cast<std::uint8_t>(word >> 16);
        digest[index++] = static_cast<std::uint8_t>(word >> 8);
        digest[index++] = static_cast<std::uint8_t>(word);
    }
    reset();
    return Sha256Digest(digest);
}

Sha256Digest sha256(std::span<const std::byte> data) noexcept {
    Sha256 hash;
    hash.update(data);
    return hash.finish();
}

}  // namespace recovery
