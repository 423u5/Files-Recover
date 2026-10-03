#include "validation/validation.hpp"

#include "validation/media_validator.hpp"
#include "validation/playability.hpp"

#include <array>
#include <utility>

namespace recovery::validation {

namespace {

LevelResult notRun(std::string detail) {
    LevelResult result;
    result.status = LevelStatus::NotRun;
    result.detail = std::move(detail);
    return result;
}

}  // namespace

std::string_view toString(ValidationLevel level) noexcept {
    switch (level) {
    case ValidationLevel::Structural:
        return "structural";
    case ValidationLevel::Media:
        return "media";
    case ValidationLevel::Playability:
        return "playability";
    }
    return "unknown";
}

std::string_view toString(LevelStatus status) noexcept {
    switch (status) {
    case LevelStatus::NotRun:
        return "not run";
    case LevelStatus::Passed:
        return "passed";
    case LevelStatus::Truncated:
        return "truncated";
    case LevelStatus::Failed:
        return "failed";
    case LevelStatus::NotApplicable:
        return "not applicable";
    case LevelStatus::Unsupported:
        return "unsupported";
    }
    return "unknown";
}

std::string_view toString(Coverage coverage) noexcept {
    switch (coverage) {
    case Coverage::Full:
        return "full";
    case Coverage::Partial:
        return "partial";
    }
    return "unknown";
}

const LevelResult& ValidationState::level(ValidationLevel which) const noexcept {
    switch (which) {
    case ValidationLevel::Structural:
        return structural;
    case ValidationLevel::Media:
        return media;
    case ValidationLevel::Playability:
        return playability;
    }
    return structural;
}

LevelResult& ValidationState::level(ValidationLevel which) noexcept {
    switch (which) {
    case ValidationLevel::Structural:
        return structural;
    case ValidationLevel::Media:
        return media;
    case ValidationLevel::Playability:
        return playability;
    }
    return structural;
}

carving::ValidationStatus ValidationState::status() const noexcept {
    const std::array<const LevelResult*, kValidationLevelCount> levels = {&structural, &media, &playability};
    for (const LevelResult* result : levels) {
        if (result->status == LevelStatus::Failed) {
            return carving::ValidationStatus::Invalid;
        }
    }
    for (const LevelResult* result : levels) {
        if (result->status == LevelStatus::Truncated) {
            return carving::ValidationStatus::Truncated;
        }
    }
    return structural.status == LevelStatus::Passed ? carving::ValidationStatus::Valid
                                                    : carving::ValidationStatus::NotValidated;
}

std::optional<ValidationLevel> ValidationState::deepestPassed() const noexcept {
    if (playability.passed()) {
        return ValidationLevel::Playability;
    }
    if (media.passed()) {
        return ValidationLevel::Media;
    }
    if (structural.passed()) {
        return ValidationLevel::Structural;
    }
    return std::nullopt;
}

Status validate(const MediaLimits& limits) {
    if (limits.maxMemory == 0 || limits.maxDecodedBytes == 0) {
        return makeError(ErrorCode::InvalidInput, "media limits: maxMemory and maxDecodedBytes must not be 0");
    }
    return formats::mp4::validate(limits.mp4);
}

LevelResult structuralLevel(const carving::ValidationResult& result, std::string_view formatId) {
    LevelResult level;
    level.checker = std::string(formatId) + " structure";
    level.detail = result.detail;
    switch (result.status) {
    case carving::ValidationStatus::NotValidated:
        level.status = LevelStatus::NotRun;
        break;
    case carving::ValidationStatus::Valid:
        level.status = LevelStatus::Passed;
        break;
    case carving::ValidationStatus::Truncated:
        level.status = LevelStatus::Truncated;
        level.offset = result.validBytes;
        break;
    case carving::ValidationStatus::Invalid:
        level.status = LevelStatus::Failed;
        level.offset = result.validBytes;
        break;
    }
    return level;
}

Result<ValidationState> validateContent(carving::IContentReader& content, const carving::IFileFormat* format,
                                        const MediaValidatorRegistry& media, const ValidationOptions& options,
                                        const std::optional<carving::ValidationResult>& knownStructure) {
    if (Status valid = validate(options.limits); !valid.ok()) {
        return valid.error();
    }
    ValidationState state;
    if (format == nullptr) {
        state.structural.status = LevelStatus::Unsupported;
        state.structural.detail = "no known format to check the content against";
        state.media = notRun("no known format");
        state.playability = notRun("no known format");
        return state;
    }
    const carving::FormatDescriptor& descriptor = format->descriptor();

    if (knownStructure.has_value() && knownStructure->status != carving::ValidationStatus::NotValidated) {
        state.structural = structuralLevel(*knownStructure, descriptor.id);
    } else {
        Result<carving::ValidationResult> structure = format->validator().validate(content);
        if (!structure.ok()) {
            return structure.error();
        }
        state.structural = structuralLevel(*structure, descriptor.id);
    }
    const bool structureFailed = state.structural.status == LevelStatus::Failed;

    if (!options.media) {
        state.media = notRun("not requested");
    } else if (structureFailed) {
        state.media = notRun("the structure failed");
    } else if (const IMediaValidator* validator = media.find(descriptor.id); validator != nullptr) {
        Result<LevelResult> decoded = validator->validate(content, options.limits);
        if (!decoded.ok()) {
            return decoded.error();
        }
        state.media = std::move(*decoded);
    } else {
        state.media.status = LevelStatus::Unsupported;
        state.media.detail = "no media decoder for " + descriptor.id;
    }

    if (options.playability == nullptr) {
        state.playability = notRun("not requested");
    } else if (structureFailed) {
        state.playability = notRun("the structure failed");
    } else if (state.structural.status != LevelStatus::Passed) {
        state.playability = notRun("the content is cut short");
    } else if (state.media.status == LevelStatus::Failed) {
        state.playability = notRun("the media failed");
    } else if (state.media.status == LevelStatus::Truncated) {
        state.playability = notRun("the content is cut short");
    } else {
        Result<LevelResult> played = options.playability->check(content, descriptor.id, descriptor.extension);
        if (!played.ok()) {
            return played.error();
        }
        state.playability = std::move(*played);
    }
    return state;
}

}  // namespace recovery::validation
