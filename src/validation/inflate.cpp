#include "inflate.hpp"

#include "bit_reader.hpp"
#include "prefix_code.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace recovery::validation::detail {

namespace {

constexpr std::array<std::uint16_t, 29> kLengthBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistanceBase = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                         33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                         1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385,
                                                         24577};
constexpr std::array<std::uint8_t, 30> kDistanceExtra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr std::array<std::uint8_t, 19> kCodeLengthOrder = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                                           11, 4,  12, 3, 13, 2, 14, 1, 15};

constexpr std::size_t kWindowSize = 32768;
constexpr std::size_t kOutputChunk = 16384;

// zlib accepts a literal/length or distance code that is complete, empty, or
// a single code of one bit.
bool acceptable(const PrefixCode& code, std::span<const std::uint8_t> lengths) {
    switch (code.shape()) {
    case PrefixCode::Shape::Complete:
    case PrefixCode::Shape::Empty:
        return true;
    case PrefixCode::Shape::Single:
        return lengths[static_cast<std::size_t>(code.single())] == 1;
    case PrefixCode::Shape::Incomplete:
    case PrefixCode::Shape::OverSubscribed:
        return false;
    }
    return false;
}

class Inflater {
public:
    Inflater(ContentBytes& bytes, InflateSink& sink)
        : bytes_(bytes), bits_(bytes), sink_(sink), window_(kWindowSize), out_(kOutputChunk) {}

    InflateOutcome run();

private:
    using Failure = std::optional<InflateOutcome>;

    [[nodiscard]] InflateOutcome outcome(InflateOutcome::Kind kind, std::uint64_t offset, std::string detail) const {
        InflateOutcome result;
        result.kind = kind;
        result.offset = offset;
        result.detail = std::move(detail);
        result.output = written_;
        result.blocks = blocks_;
        return result;
    }
    [[nodiscard]] InflateOutcome invalid(std::uint64_t offset, std::string detail) const {
        return outcome(InflateOutcome::Kind::Invalid, offset, std::move(detail));
    }
    [[nodiscard]] InflateOutcome ended(std::string_view inside) const {
        if (bytes_.error().has_value()) {
            return outcome(InflateOutcome::Kind::ReadError, bytes_.position(), "the content could not be read");
        }
        return outcome(InflateOutcome::Kind::Truncated, bytes_.position(),
                       "the data ends inside " + std::string(inside));
    }
    [[nodiscard]] InflateOutcome stopped() const {
        return outcome(InflateOutcome::Kind::Stopped, bits_.position(), {});
    }

    bool emit(std::uint8_t value) {
        window_[static_cast<std::size_t>(written_ % kWindowSize)] = value;
        ++written_;
        out_[outCount_++] = value;
        return outCount_ < out_.size() || flush();
    }
    bool flush() {
        const std::span<const std::uint8_t> data(out_.data(), outCount_);
        adler_.update(data);
        outCount_ = 0;
        return sink_.write(data);
    }

    Failure stored();
    Failure dynamicCodes(PrefixCode& literals, PrefixCode& distances);
    Failure codes(const PrefixCode& literals, const PrefixCode& distances);

    ContentBytes& bytes_;
    LsbBits bits_;
    InflateSink& sink_;
    std::vector<std::uint8_t> window_;
    std::vector<std::uint8_t> out_;
    std::size_t outCount_ = 0;
    std::uint64_t written_ = 0;
    std::uint64_t blocks_ = 0;
    std::uint32_t windowSize_ = kWindowSize;
    Adler32 adler_;
};

InflateOutcome Inflater::run() {
    const std::uint64_t start = bytes_.position();
    std::uint32_t cmf = 0;
    std::uint32_t flags = 0;
    if (!bits_.read(8, cmf) || !bits_.read(8, flags)) {
        return ended("the zlib header");
    }
    if ((cmf & 0x0FU) != 8) {
        return invalid(start, "compression method " + std::to_string(cmf & 0x0FU) + ", not deflate");
    }
    if ((cmf >> 4) > 7) {
        return invalid(start, "a window of 2^" + std::to_string((cmf >> 4) + 8) + " bytes, larger than 32 KiB");
    }
    if (((cmf << 8) | flags) % 31 != 0) {
        return invalid(start, "the zlib header's check bits are wrong");
    }
    if ((flags & 0x20U) != 0) {
        return invalid(start, "the zlib stream needs a preset dictionary");
    }
    windowSize_ = 1U << ((cmf >> 4) + 8);

    PrefixCode literals;
    PrefixCode distances;
    std::optional<std::pair<PrefixCode, PrefixCode>> fixed;
    bool last = false;
    while (!last) {
        const std::uint64_t blockStart = bits_.position();
        std::uint32_t header = 0;
        if (!bits_.read(3, header)) {
            return ended("a block header");
        }
        last = (header & 1U) != 0;
        ++blocks_;
        Failure failure;
        switch (header >> 1) {
        case 0:
            failure = stored();
            break;
        case 1:
            if (!fixed.has_value()) {
                std::array<std::uint8_t, 288> literalLengths{};
                std::size_t symbol = 0;
                for (std::uint8_t& length : literalLengths) {
                    length = symbol < 144 ? 8 : symbol < 256 ? 9 : symbol < 280 ? 7 : 8;
                    ++symbol;
                }
                std::array<std::uint8_t, 32> distanceLengths{};
                distanceLengths.fill(5);
                fixed.emplace();
                static_cast<void>(fixed->first.build(literalLengths));
                static_cast<void>(fixed->second.build(distanceLengths));
            }
            failure = codes(fixed->first, fixed->second);
            break;
        case 2:
            failure = dynamicCodes(literals, distances);
            if (!failure.has_value()) {
                failure = codes(literals, distances);
            }
            break;
        default:
            return invalid(blockStart, "a block of the reserved type 3");
        }
        if (failure.has_value()) {
            return std::move(*failure);
        }
    }
    if (outCount_ > 0 && !flush()) {
        return stopped();
    }

    bits_.alignToByte();
    const std::uint64_t checksumAt = bits_.position();
    std::uint32_t checksum = 0;
    for (int i = 0; i < 4; ++i) {
        std::uint32_t value = 0;
        if (!bits_.read(8, value)) {
            return ended("the Adler-32");
        }
        checksum = (checksum << 8) | value;
    }
    if (checksum != adler_.value()) {
        return invalid(checksumAt, "the Adler-32 of the data is wrong");
    }
    InflateOutcome done = outcome(InflateOutcome::Kind::Done, 0, {});
    done.trailing = bits_.bufferedBits() / 8 + bytes_.remaining();
    return done;
}

Inflater::Failure Inflater::stored() {
    bits_.alignToByte();
    const std::uint64_t at = bits_.position();
    std::uint32_t length = 0;
    std::uint32_t complement = 0;
    if (!bits_.read(16, length) || !bits_.read(16, complement)) {
        return ended("a stored block's length");
    }
    if (length != (~complement & 0xFFFFU)) {
        return invalid(at, "a stored block's length check is wrong");
    }
    for (std::uint32_t i = 0; i < length; ++i) {
        std::uint32_t value = 0;
        if (!bits_.read(8, value)) {
            return ended("a stored block");
        }
        if (!emit(static_cast<std::uint8_t>(value))) {
            return stopped();
        }
    }
    return std::nullopt;
}

Inflater::Failure Inflater::dynamicCodes(PrefixCode& literals, PrefixCode& distances) {
    const std::uint64_t at = bits_.position();
    std::uint32_t literalCount = 0;
    std::uint32_t distanceCount = 0;
    std::uint32_t codeCount = 0;
    if (!bits_.read(5, literalCount) || !bits_.read(5, distanceCount) || !bits_.read(4, codeCount)) {
        return ended("a block's code counts");
    }
    literalCount += 257;
    distanceCount += 1;
    codeCount += 4;
    if (literalCount > 286 || distanceCount > 30) {
        return invalid(at, "too many length or distance symbols");
    }
    std::array<std::uint8_t, 19> codeLengths{};
    for (std::uint32_t i = 0; i < codeCount; ++i) {
        std::uint32_t length = 0;
        if (!bits_.read(3, length)) {
            return ended("the code length code");
        }
        codeLengths[kCodeLengthOrder[i]] = static_cast<std::uint8_t>(length);
    }
    PrefixCode codeLengthCode;
    if (codeLengthCode.build(codeLengths) != PrefixCode::Shape::Complete) {
        return invalid(at, "the code length code is not a complete prefix code");
    }

    std::array<std::uint8_t, 286 + 30> lengths{};
    const std::uint32_t total = literalCount + distanceCount;
    std::uint32_t count = 0;
    while (count < total) {
        const std::uint64_t symbolAt = bits_.position();
        const int symbol = codeLengthCode.decode(bits_);
        if (symbol < 0) {
            return symbol == -2 ? ended("the code lengths") : invalid(symbolAt, "an invalid code length code");
        }
        if (symbol < 16) {
            lengths[count++] = static_cast<std::uint8_t>(symbol);
            continue;
        }
        std::uint8_t value = 0;
        std::uint32_t repeat = 0;
        bool read = false;
        if (symbol == 16) {
            if (count == 0) {
                return invalid(symbolAt, "a code length repeat with no length before it");
            }
            value = lengths[count - 1];
            read = bits_.read(2, repeat);
            repeat += 3;
        } else if (symbol == 17) {
            read = bits_.read(3, repeat);
            repeat += 3;
        } else {
            read = bits_.read(7, repeat);
            repeat += 11;
        }
        if (!read) {
            return ended("the code lengths");
        }
        if (repeat > total - count) {
            return invalid(symbolAt, "a code length repeat beyond the last symbol");
        }
        for (std::uint32_t i = 0; i < repeat; ++i) {
            lengths[count++] = value;
        }
    }
    if (lengths[256] == 0) {
        return invalid(at, "the literal/length code has no end-of-block code");
    }
    const std::span<const std::uint8_t> literalLengths = std::span(lengths).first(literalCount);
    const std::span<const std::uint8_t> distanceLengths = std::span(lengths).subspan(literalCount, distanceCount);
    static_cast<void>(literals.build(literalLengths));
    if (!acceptable(literals, literalLengths)) {
        return invalid(at, "the literal/length code is not a valid prefix code");
    }
    static_cast<void>(distances.build(distanceLengths));
    if (!acceptable(distances, distanceLengths)) {
        return invalid(at, "the distance code is not a valid prefix code");
    }
    return std::nullopt;
}

Inflater::Failure Inflater::codes(const PrefixCode& literals, const PrefixCode& distances) {
    for (;;) {
        const std::uint64_t at = bits_.position();
        const int symbol = literals.decode(bits_);
        if (symbol < 0) {
            return symbol == -2 ? ended("a literal/length code") : invalid(at, "an invalid literal/length code");
        }
        if (symbol < 256) {
            if (!emit(static_cast<std::uint8_t>(symbol))) {
                return stopped();
            }
            continue;
        }
        if (symbol == 256) {
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(symbol - 257);
        if (index >= kLengthBase.size()) {
            return invalid(at, "the invalid length symbol " + std::to_string(symbol));
        }
        std::uint32_t extra = 0;
        if (!bits_.read(kLengthExtra[index], extra)) {
            return ended("a length");
        }
        const std::uint32_t length = kLengthBase[index] + extra;
        const std::uint64_t distanceAt = bits_.position();
        const int distanceSymbol = distances.decode(bits_);
        if (distanceSymbol < 0) {
            return distanceSymbol == -2 ? ended("a distance code") : invalid(distanceAt, "an invalid distance code");
        }
        if (static_cast<std::size_t>(distanceSymbol) >= kDistanceBase.size()) {
            return invalid(distanceAt, "the invalid distance symbol " + std::to_string(distanceSymbol));
        }
        if (!bits_.read(kDistanceExtra[static_cast<std::size_t>(distanceSymbol)], extra)) {
            return ended("a distance");
        }
        const std::uint32_t distance = kDistanceBase[static_cast<std::size_t>(distanceSymbol)] + extra;
        if (distance > written_ || distance > windowSize_) {
            return invalid(distanceAt, "a distance of " + std::to_string(distance) + " bytes reaches before the data");
        }
        for (std::uint32_t i = 0; i < length; ++i) {
            const std::uint8_t value = window_[static_cast<std::size_t>((written_ - distance) % kWindowSize)];
            if (!emit(value)) {
                return stopped();
            }
        }
    }
}

}  // namespace

void Adler32::update(std::span<const std::uint8_t> data) noexcept {
    constexpr std::uint32_t kModulus = 65521;
    // 5552 bytes is the most that can be summed before b overflows 32 bits.
    constexpr std::size_t kBlock = 5552;
    while (!data.empty()) {
        const std::size_t take = std::min(kBlock, data.size());
        for (const std::uint8_t value : data.first(take)) {
            a_ += value;
            b_ += a_;
        }
        a_ %= kModulus;
        b_ %= kModulus;
        data = data.subspan(take);
    }
}

InflateOutcome inflateZlib(ContentBytes& bytes, InflateSink& sink) {
    const auto inflater = std::make_unique<Inflater>(bytes, sink);
    return inflater->run();
}

}  // namespace recovery::validation::detail
