#include "prefix_code.hpp"

namespace recovery::validation::detail {

namespace {

std::uint32_t reverseBits(std::uint32_t code, unsigned length) noexcept {
    std::uint32_t reversed = 0;
    for (unsigned i = 0; i < length; ++i) {
        reversed = (reversed << 1) | ((code >> i) & 1U);
    }
    return reversed;
}

}  // namespace

PrefixCode::Shape PrefixCode::build(std::span<const std::uint8_t> lengths) {
    counts_.fill(0);
    symbols_.clear();
    fast_.assign(std::size_t{1} << kFastBits, 0);
    single_ = -1;
    if (lengths.size() > kMaxSymbols) {
        shape_ = Shape::OverSubscribed;
        return shape_;
    }
    std::size_t used = 0;
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
        const std::uint8_t length = lengths[symbol];
        if (length == 0) {
            continue;
        }
        if (length > kMaxLength) {
            shape_ = Shape::OverSubscribed;
            return shape_;
        }
        ++counts_[length];
        ++used;
        single_ = static_cast<int>(symbol);
    }
    if (used == 0) {
        single_ = -1;
        shape_ = Shape::Empty;
        return shape_;
    }
    // Kraft's inequality: what is left of the code space after each length.
    std::int32_t left = 1;
    for (unsigned length = 1; length <= kMaxLength; ++length) {
        left = left * 2 - counts_[length];
        if (left < 0) {
            single_ = -1;
            shape_ = Shape::OverSubscribed;
            return shape_;
        }
    }

    // Symbols in code order.
    std::array<std::uint16_t, kMaxLength + 2> offsets{};
    for (unsigned length = 1; length <= kMaxLength; ++length) {
        offsets[length + 1] = static_cast<std::uint16_t>(offsets[length] + counts_[length]);
    }
    symbols_.assign(used, 0);
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
        const std::uint8_t length = lengths[symbol];
        if (length != 0) {
            symbols_[offsets[length]++] = static_cast<std::uint16_t>(symbol);
        }
    }
    if (used == 1) {
        shape_ = Shape::Single;
    } else {
        single_ = -1;
        shape_ = left == 0 ? Shape::Complete : Shape::Incomplete;
    }

    // The table for the codes of up to kFastBits bits, indexed by the bits as
    // they are read (the code reversed).
    std::uint32_t code = 0;
    std::size_t index = 0;
    for (unsigned length = 1; length <= kFastBits; ++length) {
        for (std::uint16_t i = 0; i < counts_[length]; ++i) {
            const std::uint16_t symbol = symbols_[index++];
            const std::uint16_t entry = static_cast<std::uint16_t>((symbol << 4) | length);
            for (std::uint32_t fill = reverseBits(code, length); fill < fast_.size(); fill += 1U << length) {
                fast_[fill] = entry;
            }
            ++code;
        }
        code <<= 1;
    }
    return shape_;
}

int PrefixCode::decode(LsbBits& bits) const {
    const std::uint32_t peeked = bits.peek(kMaxLength);
    const std::uint16_t entry = fast_[peeked & ((1U << kFastBits) - 1)];
    if (entry != 0) {
        return bits.drop(entry & 0x0FU) ? static_cast<int>(entry >> 4) : -2;
    }
    // Longer codes, bit by bit.
    std::int32_t code = 0;
    std::int32_t first = 0;
    std::int32_t index = 0;
    for (unsigned length = 1; length <= kMaxLength; ++length) {
        code |= static_cast<std::int32_t>((peeked >> (length - 1)) & 1U);
        const std::int32_t count = counts_[length];
        if (code - count < first) {
            if (!bits.drop(length)) {
                return -2;
            }
            return symbols_[static_cast<std::size_t>(index + (code - first))];
        }
        index += count;
        first = (first + count) * 2;
        code *= 2;
    }
    return bits.ensure(kMaxLength) ? -1 : -2;
}

std::size_t PrefixCode::memory() const noexcept {
    return sizeof(*this) + symbols_.capacity() * sizeof(std::uint16_t) + fast_.capacity() * sizeof(std::uint16_t);
}

}  // namespace recovery::validation::detail
