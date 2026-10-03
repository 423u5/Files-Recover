#include "vp8l_decoder.hpp"

#include "bit_reader.hpp"
#include "media_decoders.hpp"
#include "prefix_code.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::validation::detail {

namespace {

// The two-dimensional distances of the first 120 distance codes (RFC 9649
// 3.6.5): (y << 4) | (8 - x) for an offset of x columns and y rows.
constexpr std::array<std::uint8_t, 120> kCodeToPlane = {
    0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1A, 0x26, 0x2A, 0x38, 0x05, 0x37, 0x39, 0x15, 0x1B,
    0x36, 0x3A, 0x25, 0x2B, 0x48, 0x04, 0x47, 0x49, 0x14, 0x1C, 0x35, 0x3B, 0x46, 0x4A, 0x24, 0x2C, 0x58, 0x45,
    0x4B, 0x34, 0x3C, 0x03, 0x57, 0x59, 0x13, 0x1D, 0x56, 0x5A, 0x23, 0x2D, 0x44, 0x4C, 0x55, 0x5B, 0x33, 0x3D,
    0x68, 0x02, 0x67, 0x69, 0x12, 0x1E, 0x66, 0x6A, 0x22, 0x2E, 0x54, 0x5C, 0x43, 0x4D, 0x65, 0x6B, 0x32, 0x3E,
    0x78, 0x01, 0x77, 0x79, 0x53, 0x5D, 0x11, 0x1F, 0x64, 0x6C, 0x42, 0x4E, 0x76, 0x7A, 0x21, 0x2F, 0x75, 0x7B,
    0x31, 0x3F, 0x63, 0x6D, 0x52, 0x5E, 0x00, 0x74, 0x7C, 0x41, 0x4F, 0x10, 0x20, 0x62, 0x6E, 0x30, 0x73, 0x7D,
    0x51, 0x5F, 0x40, 0x72, 0x7E, 0x61, 0x6F, 0x50, 0x71, 0x7F, 0x60, 0x70,
};
constexpr std::array<std::uint8_t, 19> kCodeLengthOrder = {17, 18, 0, 1,  2,  3,  4,  5,  16, 6,
                                                           7,  8,  9, 10, 11, 12, 13, 14, 15};
constexpr std::uint32_t kLiterals = 256;
constexpr std::uint32_t kLengthCodes = 24;
constexpr std::uint32_t kDistanceCodes = 40;
constexpr std::uint32_t kMaxCacheBits = 11;
constexpr std::uint32_t kCacheMultiplier = 0x1E35A7BD;
constexpr std::array<std::string_view, 5> kCodeNames = {"the green code", "the red code", "the blue code",
                                                        "the alpha code", "the distance code"};

std::uint32_t subsampled(std::uint32_t size, std::uint32_t bits) {
    return static_cast<std::uint32_t>((std::uint64_t{size} + (std::uint64_t{1} << bits) - 1) >> bits);
}

class Vp8l {
public:
    Vp8l(ContentBytes& bytes, const MediaLimits& limits, std::uint64_t work)
        : bytes_(bytes), bits_(bytes), limits_(limits) {
        outcome_.work = work;
    }

    Vp8lOutcome header() {
        std::uint32_t signature = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t alpha = 0;
        std::uint32_t version = 0;
        if (!read(8, signature, "the VP8L header") || !read(14, width, "the VP8L header") ||
            !read(14, height, "the VP8L header") || !read(1, alpha, "the VP8L header") ||
            !read(3, version, "the VP8L header")) {
            return outcome_;
        }
        if (signature != 0x2F) {
            fail(Vp8lOutcome::Kind::Invalid, "no VP8L signature");
            return outcome_;
        }
        if (version != 0) {
            fail(Vp8lOutcome::Kind::Invalid, "VP8L version " + std::to_string(version));
            return outcome_;
        }
        return stream(width + 1, height + 1);
    }

    Vp8lOutcome stream(std::uint32_t width, std::uint32_t height) {
        outcome_.width = width;
        outcome_.height = height;
        std::uint32_t xsize = width;
        std::uint32_t used = 0;
        for (;;) {
            std::uint32_t present = 0;
            if (!read(1, present, "the transforms")) {
                return outcome_;
            }
            if (present == 0) {
                break;
            }
            std::uint32_t type = 0;
            if (!read(2, type, "the transforms")) {
                return outcome_;
            }
            if ((used & (1U << type)) != 0) {
                fail(Vp8lOutcome::Kind::Invalid, "transform " + std::to_string(type) + " is used twice");
                return outcome_;
            }
            used |= 1U << type;
            ++outcome_.transforms;
            if (type == 0 || type == 1) {  // predictor, color: a sub-image of blocks
                std::uint32_t bits = 0;
                if (!read(3, bits, "a transform") ||
                    !entropyCodedImage(subsampled(xsize, bits + 2), subsampled(height, bits + 2), false, nullptr)) {
                    return outcome_;
                }
            } else if (type == 3) {  // color indexing: the palette, then packed pixels
                std::uint32_t colors = 0;
                if (!read(8, colors, "a transform") || !entropyCodedImage(colors + 1, 1, false, nullptr)) {
                    return outcome_;
                }
                ++colors;
                const std::uint32_t widthBits = colors > 16 ? 0 : colors > 4 ? 1 : colors > 2 ? 2 : 3;
                xsize = subsampled(xsize, widthBits);
            }
        }
        if (entropyCodedImage(xsize, height, true, nullptr)) {
            outcome_.kind = Vp8lOutcome::Kind::Done;
            outcome_.pixels = std::uint64_t{xsize} * height;
        }
        return outcome_;
    }

private:
    struct Group {
        std::array<PrefixCode, 5> codes;
    };

    bool fail(Vp8lOutcome::Kind kind, std::string detail) {
        outcome_.kind = kind;
        outcome_.detail = std::move(detail);
        outcome_.offset = bits_.position();
        return false;
    }
    bool ended(std::string_view inside) {
        if (bytes_.error().has_value()) {
            return fail(Vp8lOutcome::Kind::ReadError, "the content could not be read");
        }
        return fail(Vp8lOutcome::Kind::Ended, "the data ends inside " + std::string(inside));
    }
    bool read(unsigned n, std::uint32_t& value, std::string_view what) {
        return bits_.read(n, value) || ended(what);
    }
    bool spend(std::uint64_t pixels) {
        const std::uint64_t bytes = pixels * 4;
        if (bytes > limits_.maxDecodedBytes - std::min(outcome_.work, limits_.maxDecodedBytes)) {
            return fail(Vp8lOutcome::Kind::Unsupported, "the image needs more decoding than the limit (" +
                                                            describeBytes(limits_.maxDecodedBytes) + ")");
        }
        outcome_.work += bytes;
        return true;
    }
    bool hold(std::uint64_t bytes) {
        if (bytes > limits_.maxMemory - std::min(memory_, limits_.maxMemory)) {
            return fail(Vp8lOutcome::Kind::Unsupported,
                        "the image needs more memory than the limit (" + describeBytes(limits_.maxMemory) + ")");
        }
        memory_ += bytes;
        return true;
    }

    // The next symbol of `code`: a code of one symbol takes no bits. -1 on failure.
    int symbol(const PrefixCode& code, std::string_view what) {
        if (code.shape() == PrefixCode::Shape::Single) {
            return code.single();
        }
        const int value = code.decode(bits_);
        if (value == -2) {
            ended(what);
        } else if (value < 0) {
            fail(Vp8lOutcome::Kind::Invalid, "an invalid code in " + std::string(what));
        }
        return value;
    }

    // A length or distance from its prefix symbol and extra bits (RFC 9649 3.6.4).
    bool prefixValue(int prefix, std::uint32_t& value, std::string_view what) {
        if (prefix < 4) {
            value = static_cast<std::uint32_t>(prefix) + 1;
            return true;
        }
        const auto extra = static_cast<unsigned>((prefix - 2) >> 1);
        const std::uint32_t offset = (2U + (static_cast<std::uint32_t>(prefix) & 1U)) << extra;
        std::uint32_t bits = 0;
        if (!read(extra, bits, what)) {
            return false;
        }
        value = offset + bits + 1;
        return true;
    }

    bool prefixCode(std::uint32_t alphabet, PrefixCode& code, std::string_view what) {
        std::vector<std::uint8_t> lengths(alphabet, 0);
        std::uint32_t simple = 0;
        if (!read(1, simple, what)) {
            return false;
        }
        if (simple != 0) {
            std::uint32_t count = 0;
            std::uint32_t wide = 0;
            std::uint32_t first = 0;
            if (!read(1, count, what) || !read(1, wide, what) || !read(wide != 0 ? 8 : 1, first, what)) {
                return false;
            }
            std::uint32_t second = first;
            if (count == 1 && !read(8, second, what)) {
                return false;
            }
            if (first >= alphabet || second >= alphabet) {
                return fail(Vp8lOutcome::Kind::Invalid, std::string(what) + " has a symbol beyond its alphabet");
            }
            lengths[first] = 1;
            lengths[second] = 1;
        } else {
            std::array<std::uint8_t, 19> codeLengths{};
            std::uint32_t count = 0;
            if (!read(4, count, what)) {
                return false;
            }
            for (std::uint32_t i = 0; i < count + 4; ++i) {
                std::uint32_t length = 0;
                if (!read(3, length, what)) {
                    return false;
                }
                codeLengths[kCodeLengthOrder[i]] = static_cast<std::uint8_t>(length);
            }
            PrefixCode lengthCode;
            const PrefixCode::Shape shape = lengthCode.build(codeLengths);
            if (shape != PrefixCode::Shape::Complete && shape != PrefixCode::Shape::Single) {
                return fail(Vp8lOutcome::Kind::Invalid, std::string(what) + ": its code length code is not a prefix "
                                                                            "code");
            }
            std::uint32_t limited = 0;
            std::uint32_t maxSymbol = alphabet;
            if (!read(1, limited, what)) {
                return false;
            }
            if (limited != 0) {
                std::uint32_t width = 0;
                if (!read(3, width, what) || !read(2 + 2 * width, maxSymbol, what)) {
                    return false;
                }
                maxSymbol += 2;
                if (maxSymbol > alphabet) {
                    return fail(Vp8lOutcome::Kind::Invalid,
                                std::string(what) + " reads " + std::to_string(maxSymbol) + " code lengths, more than "
                                                    "its alphabet of " + std::to_string(alphabet));
                }
            }
            std::uint32_t index = 0;
            std::uint8_t previous = 8;
            while (index < alphabet) {
                if (maxSymbol-- == 0) {
                    break;
                }
                const int length = symbol(lengthCode, what);
                if (length < 0) {
                    return false;
                }
                if (length < 16) {
                    lengths[index++] = static_cast<std::uint8_t>(length);
                    if (length != 0) {
                        previous = static_cast<std::uint8_t>(length);
                    }
                    continue;
                }
                static constexpr std::array<unsigned, 3> kExtraBits = {2, 3, 7};
                static constexpr std::array<std::uint32_t, 3> kRepeatOffsets = {3, 3, 11};
                const auto slot = static_cast<std::size_t>(length - 16);
                std::uint32_t repeat = 0;
                if (!read(kExtraBits[slot], repeat, what)) {
                    return false;
                }
                repeat += kRepeatOffsets[slot];
                if (repeat > alphabet - index) {
                    return fail(Vp8lOutcome::Kind::Invalid,
                                std::string(what) + ": a code length repeat beyond its alphabet");
                }
                std::fill_n(lengths.begin() + index, repeat, length == 16 ? previous : std::uint8_t{0});
                index += repeat;
            }
        }
        const PrefixCode::Shape shape = code.build(lengths);
        if (shape != PrefixCode::Shape::Complete && shape != PrefixCode::Shape::Single) {
            return fail(Vp8lOutcome::Kind::Invalid, std::string(what) + " is not a complete prefix code");
        }
        return hold(code.memory());
    }

    // An entropy-coded image of xsize x ysize: its color cache, (for the main
    // image) its meta prefix codes, its prefix code groups and its data.
    // `values` receives the pixels (the entropy image only).
    bool entropyCodedImage(std::uint32_t xsize, std::uint32_t ysize, bool main, std::vector<std::uint32_t>* values) {
        std::uint32_t useCache = 0;
        std::uint32_t cacheBits = 0;
        if (!read(1, useCache, "a color cache")) {
            return false;
        }
        if (useCache != 0) {
            if (!read(4, cacheBits, "a color cache")) {
                return false;
            }
            if (cacheBits < 1 || cacheBits > kMaxCacheBits) {
                return fail(Vp8lOutcome::Kind::Invalid, "a color cache of " + std::to_string(cacheBits) + " bits");
            }
        }
        std::vector<std::uint32_t> meta;
        std::uint32_t metaBits = 0;
        std::uint32_t metaXsize = 0;
        std::uint32_t groupCount = 1;
        bool useMeta = false;
        if (main) {
            outcome_.cacheBits = cacheBits;
            std::uint32_t flag = 0;
            if (!read(1, flag, "the meta prefix codes")) {
                return false;
            }
            useMeta = flag != 0;
            if (useMeta) {
                if (!read(3, metaBits, "the meta prefix codes")) {
                    return false;
                }
                metaBits += 2;
                metaXsize = subsampled(xsize, metaBits);
                if (!entropyCodedImage(metaXsize, subsampled(ysize, metaBits), false, &meta)) {
                    return false;
                }
                for (const std::uint32_t value : meta) {
                    groupCount = std::max(groupCount, ((value >> 8) & 0xFFFFU) + 1);
                }
            }
        }
        if (!hold(std::uint64_t{groupCount} * sizeof(Group))) {
            return false;
        }
        std::vector<Group> groups(groupCount);
        const std::uint32_t cacheSize = cacheBits != 0 ? 1U << cacheBits : 0;
        const std::array<std::uint32_t, 5> alphabets = {kLiterals + kLengthCodes + cacheSize, kLiterals, kLiterals,
                                                        kLiterals, kDistanceCodes};
        for (Group& group : groups) {
            for (std::size_t i = 0; i < group.codes.size(); ++i) {
                if (!prefixCode(alphabets[i], group.codes[i], kCodeNames[i])) {
                    return false;
                }
            }
        }
        if (main) {
            outcome_.groups = groupCount;
        }
        return imageData(xsize, ysize, cacheBits, groups, useMeta ? &meta : nullptr, metaBits, metaXsize, values);
    }

    bool imageData(std::uint32_t xsize, std::uint32_t ysize, std::uint32_t cacheBits, const std::vector<Group>& groups,
                   const std::vector<std::uint32_t>* meta, std::uint32_t metaBits, std::uint32_t metaXsize,
                   std::vector<std::uint32_t>* values) {
        const std::uint64_t total = std::uint64_t{xsize} * ysize;
        if (!spend(total)) {
            return false;
        }
        std::vector<std::uint32_t> cache;
        if (values != nullptr) {
            if (!hold(total * sizeof(std::uint32_t))) {
                return false;
            }
            values->assign(static_cast<std::size_t>(total), 0);
            if (cacheBits != 0) {
                cache.assign(std::size_t{1} << cacheBits, 0);
            }
        }
        const std::uint32_t cacheSize = cacheBits != 0 ? 1U << cacheBits : 0;
        std::uint64_t position = 0;
        std::uint64_t cached = 0;
        while (position < total) {
            const auto x = static_cast<std::uint32_t>(position % xsize);
            const auto y = static_cast<std::uint32_t>(position / xsize);
            const auto metaIndex = static_cast<std::size_t>(std::uint64_t{y >> metaBits} * metaXsize + (x >> metaBits));
            const Group& group = meta != nullptr ? groups[((*meta)[metaIndex] >> 8) & 0xFFFFU] : groups.front();
            const int green = symbol(group.codes[0], kCodeNames[0]);
            if (green < 0) {
                return false;
            }
            if (green < static_cast<int>(kLiterals)) {
                const int red = symbol(group.codes[1], kCodeNames[1]);
                const int blue = red < 0 ? -1 : symbol(group.codes[2], kCodeNames[2]);
                const int alpha = blue < 0 ? -1 : symbol(group.codes[3], kCodeNames[3]);
                if (alpha < 0) {
                    return false;
                }
                if (values != nullptr) {
                    (*values)[static_cast<std::size_t>(position)] =
                        (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(red) << 16) |
                        (static_cast<std::uint32_t>(green) << 8) | static_cast<std::uint32_t>(blue);
                }
                ++position;
            } else if (green < static_cast<int>(kLiterals + kLengthCodes)) {
                std::uint32_t length = 0;
                std::uint32_t code = 0;
                if (!prefixValue(green - static_cast<int>(kLiterals), length, "a backward reference")) {
                    return false;
                }
                const int distanceSymbol = symbol(group.codes[4], kCodeNames[4]);
                if (distanceSymbol < 0 || !prefixValue(distanceSymbol, code, "a backward reference")) {
                    return false;
                }
                std::uint64_t distance = 0;
                if (code > kCodeToPlane.size()) {
                    distance = code - kCodeToPlane.size();
                } else {
                    const std::uint8_t plane = kCodeToPlane[code - 1];
                    const std::int64_t offset = std::int64_t{plane >> 4} * xsize + 8 - (plane & 0x0F);
                    distance = offset >= 1 ? static_cast<std::uint64_t>(offset) : 1;
                }
                if (distance > position) {
                    return fail(Vp8lOutcome::Kind::Invalid,
                                "pixel " + std::to_string(position) + ": a backward reference " +
                                    std::to_string(distance) + " pixels back, before the image");
                }
                if (length > total - position) {
                    return fail(Vp8lOutcome::Kind::Invalid, "pixel " + std::to_string(position) + ": a backward "
                                                            "reference of " + std::to_string(length) +
                                                                " pixels runs past the image's " +
                                                                std::to_string(total));
                }
                if (values != nullptr) {
                    for (std::uint32_t i = 0; i < length; ++i) {
                        const auto at = static_cast<std::size_t>(position + i);
                        (*values)[at] = (*values)[at - static_cast<std::size_t>(distance)];
                    }
                }
                position += length;
            } else {
                const auto key = static_cast<std::uint32_t>(green) - kLiterals - kLengthCodes;
                if (key >= cacheSize) {
                    return fail(Vp8lOutcome::Kind::Invalid, "pixel " + std::to_string(position) +
                                                                ": color cache entry " + std::to_string(key) +
                                                                " of a cache of " + std::to_string(cacheSize));
                }
                if (values != nullptr) {
                    (*values)[static_cast<std::size_t>(position)] = cache[key];
                }
                ++position;
            }
            if (values != nullptr && cacheSize != 0) {
                for (; cached < position; ++cached) {
                    const std::uint32_t argb = (*values)[static_cast<std::size_t>(cached)];
                    cache[(argb * kCacheMultiplier) >> (32 - cacheBits)] = argb;
                }
            }
        }
        return true;
    }

    ContentBytes& bytes_;
    LsbBits bits_;
    const MediaLimits& limits_;
    Vp8lOutcome outcome_;
    std::uint64_t memory_ = 0;
};

}  // namespace

Vp8lOutcome decodeVp8l(ContentBytes& bytes, const MediaLimits& limits, std::uint64_t work) {
    Vp8l decoder(bytes, limits, work);
    return decoder.header();
}

Vp8lOutcome decodeVp8lStream(ContentBytes& bytes, std::uint32_t width, std::uint32_t height,
                             const MediaLimits& limits, std::uint64_t work) {
    Vp8l decoder(bytes, limits, work);
    return decoder.stream(width, height);
}

}  // namespace recovery::validation::detail
