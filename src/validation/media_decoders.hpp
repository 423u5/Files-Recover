#pragma once

// The media validators of media_validator.hpp, and what they share.

#include "validation/media_validator.hpp"
#include "validation/validation.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace recovery::validation::detail {

[[nodiscard]] std::shared_ptr<const IMediaValidator> makeJpegMediaValidator();
[[nodiscard]] std::shared_ptr<const IMediaValidator> makePngMediaValidator();
[[nodiscard]] std::shared_ptr<const IMediaValidator> makeWebpMediaValidator();
[[nodiscard]] std::shared_ptr<const IMediaValidator> makeGifMediaValidator();
[[nodiscard]] std::shared_ptr<const IMediaValidator> makeBmpMediaValidator();
[[nodiscard]] std::shared_ptr<const IMediaValidator> makeMp3MediaValidator();
[[nodiscard]] std::shared_ptr<const IMediaValidator> makeWavMediaValidator();
[[nodiscard]] std::shared_ptr<const IMediaValidator> makeM4aMediaValidator();
[[nodiscard]] std::shared_ptr<const IMediaValidator> makeAacMediaValidator();
[[nodiscard]] std::shared_ptr<const IMediaValidator> makeMp4MediaValidator();

// Results of one decoder (`checker` names it).
class MediaVerdict {
public:
    explicit MediaVerdict(std::string checker) : checker_(std::move(checker)) {}

    [[nodiscard]] LevelResult passed(std::string detail, Coverage coverage = Coverage::Full) const {
        return make(LevelStatus::Passed, std::nullopt, std::move(detail), coverage);
    }
    [[nodiscard]] LevelResult failed(std::uint64_t offset, std::string detail) const {
        return make(LevelStatus::Failed, offset, std::move(detail), Coverage::Full);
    }
    [[nodiscard]] LevelResult truncated(std::uint64_t offset, std::string detail) const {
        return make(LevelStatus::Truncated, offset, std::move(detail), Coverage::Full);
    }
    [[nodiscard]] LevelResult unsupported(std::string detail) const {
        return make(LevelStatus::Unsupported, std::nullopt, std::move(detail), Coverage::Full);
    }
    [[nodiscard]] LevelResult notApplicable(std::string detail) const {
        return make(LevelStatus::NotApplicable, std::nullopt, std::move(detail), Coverage::Full);
    }

private:
    [[nodiscard]] LevelResult make(LevelStatus status, std::optional<std::uint64_t> offset, std::string detail,
                                   Coverage coverage) const {
        LevelResult result;
        result.status = status;
        result.checker = checker_;
        result.coverage = coverage;
        result.offset = offset;
        result.detail = std::move(detail);
        return result;
    }

    std::string checker_;
};

// "1.5 MiB", "300 bytes": sizes in details.
[[nodiscard]] std::string describeBytes(std::uint64_t bytes);

}  // namespace recovery::validation::detail
