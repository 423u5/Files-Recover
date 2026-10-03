#include "media_decoders.hpp"

#include "validation/media_validator.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace recovery::validation {

Status MediaValidatorRegistry::add(std::shared_ptr<const IMediaValidator> validator) {
    if (validator == nullptr) {
        return makeError(ErrorCode::InvalidInput, "media validator registry: no validator");
    }
    if (find(validator->formatId()) != nullptr) {
        return makeError(ErrorCode::InvalidInput,
                         "media validator registry: '" + std::string(validator->formatId()) + "' has one already");
    }
    validators_.push_back(std::move(validator));
    return success();
}

const IMediaValidator* MediaValidatorRegistry::find(std::string_view formatId) const noexcept {
    for (const std::shared_ptr<const IMediaValidator>& validator : validators_) {
        if (validator->formatId() == formatId) {
            return validator.get();
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<const IMediaValidator>> mediaValidators() {
    return {
        detail::makeJpegMediaValidator(),
        detail::makePngMediaValidator(),
        detail::makeWebpMediaValidator(),
        detail::makeGifMediaValidator(),
        detail::makeBmpMediaValidator(),
        detail::makeAacMediaValidator(),
        detail::makeMp3MediaValidator(),
        detail::makeWavMediaValidator(),
        detail::makeM4aMediaValidator(),
        detail::makeMp4MediaValidator(),
    };
}

Status registerMediaValidators(MediaValidatorRegistry& registry) {
    const std::vector<std::shared_ptr<const IMediaValidator>> validators = mediaValidators();
    for (const std::shared_ptr<const IMediaValidator>& validator : validators) {
        if (registry.find(validator->formatId()) != nullptr) {
            return makeError(ErrorCode::InvalidInput,
                             "media validator registry: '" + std::string(validator->formatId()) + "' has one already");
        }
    }
    for (const std::shared_ptr<const IMediaValidator>& validator : validators) {
        if (Status added = registry.add(validator); !added.ok()) {
            return added;
        }
    }
    return success();
}

namespace detail {

std::string describeBytes(std::uint64_t bytes) {
    constexpr std::uint64_t kKibibyte = 1024;
    if (bytes < kKibibyte) {
        return std::to_string(bytes) + (bytes == 1 ? " byte" : " bytes");
    }
    static constexpr std::array<const char*, 4> kUnits = {"KiB", "MiB", "GiB", "TiB"};
    std::uint64_t whole = bytes;
    std::uint64_t scale = 1;
    std::size_t unit = 0;
    while (whole >= kKibibyte * kKibibyte && unit + 1 < kUnits.size()) {
        whole /= kKibibyte;
        scale *= kKibibyte;
        ++unit;
    }
    scale *= kKibibyte;
    const std::uint64_t integer = bytes / scale;
    const std::uint64_t tenth = (bytes % scale) * 10 / scale;
    return std::to_string(integer) + (tenth != 0 ? "." + std::to_string(tenth) : std::string()) + " " +
           kUnits[std::min(unit, kUnits.size() - 1)];
}

}  // namespace detail

}  // namespace recovery::validation
