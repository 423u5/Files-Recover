#include "validation/media_test_helpers.hpp"

#include "carving/content_reader.hpp"
#include "formats/audio_formats.hpp"
#include "formats/image_formats.hpp"
#include "formats/video_formats.hpp"

#include <algorithm>
#include <random>

namespace recovery::validation::testing {

// The registrations run before the checks: MSVC 14.51 (Release) builds an
// empty registry when they run inside EXPECT_TRUE in a lambda that
// initializes a static (found 2026-10-03).
const carving::FormatRegistry& allFormats() {
    static const carving::FormatRegistry registry = [] {
        carving::FormatRegistry formats;
        const bool images = formats::registerImageFormats(formats).ok();
        const bool audio = formats::registerAudioFormats(formats).ok();
        const bool video = formats::registerVideoFormats(formats).ok();
        EXPECT_TRUE(images && audio && video);
        return formats;
    }();
    return registry;
}

const MediaValidatorRegistry& allMediaValidators() {
    static const MediaValidatorRegistry registry = [] {
        MediaValidatorRegistry validators;
        const bool registered = registerMediaValidators(validators).ok();
        EXPECT_TRUE(registered);
        return validators;
    }();
    return registry;
}

LevelResult mediaOf(std::string_view formatId, std::span<const std::byte> data, const MediaLimits& limits) {
    const IMediaValidator* validator = allMediaValidators().find(formatId);
    if (validator == nullptr) {
        ADD_FAILURE() << "no media validator for " << formatId;
        return {};
    }
    carving::MemoryContentReader content(data);
    Result<LevelResult> result = validator->validate(content, limits);
    if (!result.ok()) {
        ADD_FAILURE() << "media validation failed: " << recovery::describe(result.error());
        return {};
    }
    return std::move(*result);
}

ValidationState validated(std::string_view formatId, std::span<const std::byte> data,
                          const ValidationOptions& options) {
    const carving::IFileFormat* format = allFormats().find(formatId);
    if (format == nullptr) {
        ADD_FAILURE() << "no format " << formatId;
        return {};
    }
    carving::MemoryContentReader content(data);
    Result<ValidationState> state = validateContent(content, format, allMediaValidators(), options);
    if (!state.ok()) {
        ADD_FAILURE() << "validation failed: " << recovery::describe(state.error());
        return {};
    }
    return std::move(*state);
}

std::string describe(const LevelResult& result) {
    std::string text = std::string(toString(result.status));
    if (!result.checker.empty()) {
        text += " (" + result.checker + ")";
    }
    if (result.status == LevelStatus::Passed && result.coverage == Coverage::Partial) {
        text += " [partial]";
    }
    if (result.offset.has_value()) {
        text += " at " + std::to_string(*result.offset);
    }
    return text + ": " + result.detail;
}

std::string describe(const ValidationState& state) {
    return "structural " + describe(state.structural) + "\nmedia " + describe(state.media) + "\nplayability " +
           describe(state.playability);
}

::testing::AssertionResult passesEveryLevel(std::string_view formatId, std::span<const std::byte> data,
                                            Coverage coverage) {
    const ValidationState state = validated(formatId, data);
    if (state.structural.status != LevelStatus::Passed || state.media.status != LevelStatus::Passed ||
        state.media.coverage != coverage || state.status() != carving::ValidationStatus::Valid) {
        return ::testing::AssertionFailure() << describe(state);
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult mediaIs(std::string_view formatId, std::span<const std::byte> data, LevelStatus status,
                                   std::string_view detail) {
    const LevelResult result = mediaOf(formatId, data);
    if (result.status != status) {
        return ::testing::AssertionFailure() << "expected " << toString(status) << ", got " << describe(result);
    }
    if (!detail.empty() && result.detail.find(detail) == std::string::npos) {
        return ::testing::AssertionFailure() << "expected a detail with \"" << detail << "\", got " << describe(result);
    }
    if (result.offset.has_value() && *result.offset > data.size()) {
        return ::testing::AssertionFailure() << "the offset lies beyond the data: " << describe(result);
    }
    return ::testing::AssertionSuccess();
}

void fuzzMedia(std::string_view formatId, const Bytes& file, int iterations, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    const auto below = [&](std::size_t bound) {
        return bound == 0 ? std::size_t{0} : static_cast<std::size_t>(random() % bound);
    };
    for (int i = 0; i < iterations; ++i) {
        Bytes damaged = file;
        switch (i % 6) {
        case 0:  // bytes replaced
            for (std::size_t n = 1 + below(8); n > 0 && !damaged.empty(); --n) {
                damaged[below(damaged.size())] = static_cast<std::byte>(random() & 0xFF);
            }
            break;
        case 1:  // cut short
            damaged.resize(below(damaged.size() + 1));
            break;
        case 2: {  // bytes inserted
            const std::size_t at = below(damaged.size() + 1);
            Bytes noise(1 + below(64));
            for (std::byte& b : noise) {
                b = static_cast<std::byte>(random() & 0xFF);
            }
            damaged.insert(damaged.begin() + static_cast<std::ptrdiff_t>(at), noise.begin(), noise.end());
            break;
        }
        case 3: {  // bytes removed
            const std::size_t at = below(damaged.size());
            const std::size_t count = std::min(damaged.size() - at, 1 + below(64));
            damaged.erase(damaged.begin() + static_cast<std::ptrdiff_t>(at),
                          damaged.begin() + static_cast<std::ptrdiff_t>(at + count));
            break;
        }
        case 4: {  // a run of zeros
            const std::size_t at = below(damaged.size());
            const std::size_t count = std::min(damaged.size() - at, 1 + below(512));
            std::fill_n(damaged.begin() + static_cast<std::ptrdiff_t>(at), count, std::byte{0});
            break;
        }
        default: {  // a run of 0xFF
            const std::size_t at = below(damaged.size());
            const std::size_t count = std::min(damaged.size() - at, 1 + below(512));
            std::fill_n(damaged.begin() + static_cast<std::ptrdiff_t>(at), count, std::byte{0xFF});
            break;
        }
        }
        const LevelResult result = mediaOf(formatId, damaged);
        if (result.offset.has_value()) {
            EXPECT_LE(*result.offset, damaged.size()) << "iteration " << i << ": " << describe(result);
        }
        EXPECT_NE(result.status, LevelStatus::NotRun) << "iteration " << i;
    }
}

}  // namespace recovery::validation::testing
