#include "structure_walk.hpp"

#include "recovery/checked_math.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace recovery::formats::detail {

void Walk::noteProblem(std::uint64_t offset, std::string text) {
    if (!problem.has_value()) {
        problem = WalkProblem{offset, std::move(text)};
    }
}

Walk& Walk::finish(WalkStatus outcome, std::uint64_t at, std::string text) {
    status = outcome;
    end = at;
    detail = std::move(text);
    return *this;
}

carving::EndDetection endOf(const Walk& walk, std::uint64_t contentSize) {
    switch (walk.status) {
    case WalkStatus::Complete:
        return carving::EndDetection{carving::EndStatus::Found, walk.end, walk.detail};
    case WalkStatus::Truncated:
        return carving::EndDetection{carving::EndStatus::Truncated, contentSize, walk.detail};
    case WalkStatus::Broken:
        break;
    }
    return carving::EndDetection{carving::EndStatus::Broken, walk.end, walk.detail};
}

carving::ValidationResult verdictOf(const Walk& walk, std::uint64_t contentSize) {
    using carving::ValidationStatus;
    if (walk.problem.has_value()) {
        return carving::ValidationResult{ValidationStatus::Invalid, std::min(walk.problem->offset, contentSize),
                                         walk.problem->detail};
    }
    switch (walk.status) {
    case WalkStatus::Complete:
        if (walk.end < contentSize) {
            return carving::ValidationResult{ValidationStatus::Invalid, walk.end,
                                             walk.detail + "; " + std::to_string(contentSize - walk.end) +
                                                 " bytes follow the end of the structure"};
        }
        return carving::ValidationResult{ValidationStatus::Valid, contentSize, walk.detail};
    case WalkStatus::Truncated:
        return carving::ValidationResult{ValidationStatus::Truncated, std::min(walk.end, contentSize), walk.detail};
    case WalkStatus::Broken:
        break;
    }
    return carving::ValidationResult{ValidationStatus::Invalid, std::min(walk.end, contentSize), walk.detail};
}

Result<std::optional<std::span<const std::byte>>> readIfAvailable(carving::IContentReader& content,
                                                                  std::uint64_t offset, std::size_t length) {
    if (!rangeWithin<std::uint64_t>(offset, length, content.size())) {
        return std::optional<std::span<const std::byte>>{};
    }
    Result<std::span<const std::byte>> bytes = content.read(offset, length);
    if (!bytes.ok()) {
        return bytes.error();
    }
    return std::optional<std::span<const std::byte>>(*bytes);
}

SequentialReader::SequentialReader(carving::IContentReader& content, std::uint64_t offset,
                                   std::uint64_t end) noexcept
    : content_(content), position_(offset), end_(std::min(end, content.size())) {}

Result<std::optional<std::uint8_t>> SequentialReader::next() {
    if (position_ >= end_) {
        return std::optional<std::uint8_t>{};
    }
    if (position_ < chunkOffset_ || position_ - chunkOffset_ >= chunk_.size()) {
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, end_ - position_));
        Result<std::span<const std::byte>> chunk = content_.read(position_, length);
        if (!chunk.ok()) {
            return chunk.error();
        }
        chunk_ = *chunk;
        chunkOffset_ = position_;
    }
    const auto byte = static_cast<std::uint8_t>(chunk_[static_cast<std::size_t>(position_ - chunkOffset_)]);
    ++position_;
    return std::optional<std::uint8_t>(byte);
}

bool SequentialReader::skip(std::uint64_t count) noexcept {
    if (position_ >= end_ || count > end_ - position_) {
        position_ = std::max(position_, end_);
        return false;
    }
    position_ += count;
    return true;
}

std::string hexByte(std::uint8_t value) {
    constexpr std::string_view kDigits = "0123456789ABCDEF";
    std::string text = "0x";
    text += kDigits[value >> 4];
    text += kDigits[value & 0x0F];
    return text;
}

bool isLetterCode(std::span<const std::byte> code) noexcept {
    if (code.size() != 4) {
        return false;
    }
    for (const std::byte byte : code) {
        const auto c = static_cast<std::uint8_t>(byte);
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
            return false;
        }
    }
    return true;
}

std::string fourCcText(std::span<const std::byte> code) {
    std::string text;
    for (const std::byte byte : code) {
        const auto c = static_cast<std::uint8_t>(byte);
        if (c < 0x20 || c > 0x7E) {
            return "(unprintable)";
        }
        text += static_cast<char>(c);
    }
    return "'" + text + "'";
}

}  // namespace recovery::formats::detail
