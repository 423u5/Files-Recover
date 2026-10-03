#include "nal_units.hpp"

#include <algorithm>
#include <bit>

namespace recovery::validation::detail::nal {

namespace {

std::string hexByte(std::uint8_t value) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    return std::string{kDigits[value >> 4], kDigits[value & 0xFU]};
}

std::vector<std::uint8_t> payloadWithoutEscapes(std::span<const std::uint8_t> unit, std::size_t headerBytes,
                                                std::vector<std::uint64_t>& escapes) {
    std::vector<std::uint8_t> out;
    if (headerBytes >= unit.size()) {
        return out;
    }
    out.reserve(unit.size() - headerBytes);
    unsigned zeros = 0;
    for (std::size_t i = headerBytes; i < unit.size(); ++i) {
        const std::uint8_t value = unit[i];
        if (zeros >= 2 && value == 3) {
            escapes.push_back(out.size());
            zeros = 0;
            continue;
        }
        out.push_back(value);
        zeros = value == 0 ? zeros + 1 : 0;
    }
    return out;
}

}  // namespace

void SequenceScan::feed(std::span<const std::uint8_t> bytes, std::uint64_t offset) {
    for (std::size_t i = 0; i < bytes.size() && !violation_.has_value(); ++i) {
        next(bytes[i], offset + i);
    }
}

void SequenceScan::feed(std::span<const std::byte> bytes, std::uint64_t offset) {
    for (std::size_t i = 0; i < bytes.size() && !violation_.has_value(); ++i) {
        next(std::to_integer<std::uint8_t>(bytes[i]), offset + i);
    }
}

void SequenceScan::next(std::uint8_t value, std::uint64_t at) {
    if (afterEscape_) {
        afterEscape_ = false;
        if (value > 3) {
            violation_ = Violation{escapeOffset_, "the byte sequence 00 00 03 " + hexByte(value) +
                                                      " inside the NAL unit"};
            return;
        }
    }
    if (value == 0) {
        if (++zeros_ == 3) {
            runStart_ = at - 2;
        }
        return;
    }
    if (zeros_ >= 3) {
        violation_ = Violation{runStart_, "a run of " + std::to_string(zeros_) + " zero bytes inside the NAL unit"};
        return;
    }
    if (zeros_ == 2) {
        if (value == 1 || value == 2) {
            violation_ = Violation{at - 2, "the byte sequence 00 00 " + hexByte(value) + " inside the NAL unit"};
            return;
        }
        if (value == 3) {
            afterEscape_ = true;
            escapeOffset_ = at - 2;
        }
    }
    zeros_ = 0;
}

Rbsp::Rbsp(std::span<const std::uint8_t> unit, std::size_t headerBytes, bool complete)
    : headerBytes_(headerBytes), bytes_(payloadWithoutEscapes(unit, headerBytes, escapes_)), bits_(bytes_),
      complete_(complete) {
    if (!complete) {
        return;
    }
    for (std::size_t i = bytes_.size(); i-- > 0;) {
        if (bytes_[i] != 0) {
            stopBit_ = std::uint64_t{i} * 8 + 7 - static_cast<unsigned>(std::countr_zero(bytes_[i]));
            break;
        }
    }
}

std::uint64_t Rbsp::unitOffset(std::uint64_t index) const noexcept {
    const auto before = std::upper_bound(escapes_.begin(), escapes_.end(), index) - escapes_.begin();
    return headerBytes_ + index + static_cast<std::uint64_t>(before);
}

unsigned ceilLog2(std::uint64_t n) noexcept {
    return n <= 1 ? 0U : static_cast<unsigned>(std::bit_width(n - 1));
}

}  // namespace recovery::validation::detail::nal
