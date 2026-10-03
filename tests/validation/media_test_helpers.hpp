#pragma once

// Helpers shared by the validation tests: the registered formats and media
// decoders, running the levels on bytes in memory, and mutation fuzzing of
// the media decoders.

#include "carving/format_registry.hpp"
#include "validation/media_validator.hpp"
#include "validation/validation.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::validation::testing {

using Bytes = std::vector<std::byte>;

// Every carving format (images, audio, video) and every media decoder.
[[nodiscard]] const carving::FormatRegistry& allFormats();
[[nodiscard]] const MediaValidatorRegistry& allMediaValidators();

// The media level of `formatId`'s decoder on `data` (a test failure when the
// decoder fails with an error).
[[nodiscard]] LevelResult mediaOf(std::string_view formatId, std::span<const std::byte> data,
                                  const MediaLimits& limits = {});
// Every level, with the format `formatId` (structural and media; playability
// only when options ask for it).
[[nodiscard]] ValidationState validated(std::string_view formatId, std::span<const std::byte> data,
                                        const ValidationOptions& options = {});

[[nodiscard]] std::string describe(const LevelResult& result);
[[nodiscard]] std::string describe(const ValidationState& state);

// The structure is Valid and the media level passed with this coverage.
[[nodiscard]] ::testing::AssertionResult passesEveryLevel(std::string_view formatId, std::span<const std::byte> data,
                                                          Coverage coverage = Coverage::Full);
// The media level has this status, and its detail holds `detail`.
[[nodiscard]] ::testing::AssertionResult mediaIs(std::string_view formatId, std::span<const std::byte> data,
                                                 LevelStatus status, std::string_view detail = {});

// Bits written most significant first, for syntax written field by field
// (AAC raw data blocks, AVC and HEVC parameter sets and slice headers).
class BitWriter {
public:
    void put(std::uint32_t value, unsigned count) {
        for (unsigned i = count; i-- > 0;) {
            bits_.push_back(((value >> i) & 1U) != 0);
        }
    }
    void flag(bool value) { bits_.push_back(value); }
    // ue(v) and se(v): Exp-Golomb codes.
    void ue(std::uint32_t value) {
        const std::uint64_t code = std::uint64_t{value} + 1;
        unsigned length = 0;
        while ((code >> (length + 1)) != 0) {
            ++length;
        }
        put(0, length);
        for (unsigned i = length + 1; i-- > 0;) {
            bits_.push_back(((code >> i) & 1U) != 0);
        }
    }
    void se(std::int32_t value) {
        ue(value > 0 ? static_cast<std::uint32_t>(2 * value - 1)
                     : static_cast<std::uint32_t>(-2 * std::int64_t{value}));
    }
    // rbsp_trailing_bits(): a one, then zeros to the next byte boundary.
    void trailingBits() {
        bits_.push_back(true);
        while (bits_.size() % 8 != 0) {
            bits_.push_back(false);
        }
    }
    [[nodiscard]] std::size_t size() const noexcept { return bits_.size(); }
    // The bits so far, the last byte padded with zeros.
    [[nodiscard]] Bytes bytes() const {
        Bytes out((bits_.size() + 7) / 8);
        for (std::size_t i = 0; i < bits_.size(); ++i) {
            if (bits_[i]) {
                out[i / 8] |= static_cast<std::byte>(0x80U >> (i % 8));
            }
        }
        return out;
    }

private:
    std::vector<bool> bits_;
};

// Runs the decoder on `iterations` damaged copies of `file` (bytes replaced,
// cut short, bytes inserted and removed, runs of zeros and of 0xFF) and
// checks that every result is well formed: a problem's offset lies inside
// the data. Crashes, hangs and out-of-bounds reads are what this is for
// (AddressSanitizer build).
void fuzzMedia(std::string_view formatId, const Bytes& file, int iterations, std::uint64_t seed);

}  // namespace recovery::validation::testing
