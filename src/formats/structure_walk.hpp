#pragma once

// What the format modules share (private to recovery_formats): the outcome
// of walking a file's structure from its first byte, and how that outcome
// becomes an end detection (carving) or a validation verdict.
//
// Each format has one walk. End detection and validation both use it, so a
// file ends exactly where its validator stops looking. The walk separates:
//  * where the structure goes: the layout. A layout that cannot go on is
//    Broken; one that goes beyond the data is Truncated.
//  * whether the elements it passes are consistent: checksums, field values,
//    element order. An inconsistency is a problem; the walk records the first
//    one and carries on, because the layout still says where the file ends.
// End detection only uses the layout. Validation fails on a problem too.

#include "carving/content_reader.hpp"
#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace recovery::formats::detail {

// How much a walk checks.
enum class Depth : std::uint8_t {
    // The layout, and whatever it needs no extra reads for (end detection).
    Layout,
    // Also what needs every byte read: checksums, compressed streams' framing (validation).
    Full,
};

enum class WalkStatus : std::uint8_t {
    // The structure ended at `end`.
    Complete,
    // The data ran out before the structure ended.
    Truncated,
    // The structure stops making sense at `end`.
    Broken,
};

struct WalkProblem {
    // Where the inconsistent element starts.
    std::uint64_t offset = 0;
    std::string detail;
};

struct Walk {
    WalkStatus status = WalkStatus::Broken;
    // Complete: one past the structure's last byte. Truncated: the bytes whose
    // structure was checked before the data ran out. Broken: where the
    // structure breaks (the bytes before it are consistent).
    std::uint64_t end = 0;
    // What ended the walk (never file content).
    std::string detail;
    // The first inconsistency that did not stop the walk.
    std::optional<WalkProblem> problem;

    // Records `detail` at `offset` unless a problem was recorded already.
    void noteProblem(std::uint64_t offset, std::string detail);
    // Sets the outcome and returns *this (for `return walk.finish(...)`).
    Walk& finish(WalkStatus status, std::uint64_t end, std::string detail);
};

// Found at the end of a complete structure, Truncated with every byte of the
// content, or Broken at the break.
[[nodiscard]] carving::EndDetection endOf(const Walk& walk, std::uint64_t contentSize);

// Valid when the structure is complete, has no problem and ends exactly at
// the end of the content; Truncated when the data ran out; Invalid otherwise
// (a problem, a break, or bytes after the end of the structure).
[[nodiscard]] carving::ValidationResult verdictOf(const Walk& walk, std::uint64_t contentSize);

// The `length` bytes at `offset`, or an empty optional when they do not all
// lie inside the content (the data ran out). Fails with the reader's errors.
[[nodiscard]] Result<std::optional<std::span<const std::byte>>> readIfAvailable(carving::IContentReader& content,
                                                                                std::uint64_t offset,
                                                                                std::size_t length);

// Reads content [offset, end) byte by byte with one content read per chunk.
// Nothing else may read the content while a SequentialReader is in use: a
// content read invalidates the chunk it holds.
class SequentialReader {
public:
    static constexpr std::size_t kChunk = 64 * 1024;

    // `end` is clipped to the content's size.
    SequentialReader(carving::IContentReader& content, std::uint64_t offset, std::uint64_t end) noexcept;

    // The next byte, or an empty optional at the end.
    [[nodiscard]] Result<std::optional<std::uint8_t>> next();
    // Skips `count` bytes. False (and positioned at the end) when fewer are left.
    [[nodiscard]] bool skip(std::uint64_t count) noexcept;
    [[nodiscard]] std::uint64_t position() const noexcept { return position_; }

private:
    carving::IContentReader& content_;
    std::uint64_t position_;
    std::uint64_t end_;
    std::span<const std::byte> chunk_;
    std::uint64_t chunkOffset_ = 0;
};

// "0x1F" style, for marker codes and field values in details.
[[nodiscard]] std::string hexByte(std::uint8_t value);

// True when the four bytes are ASCII letters (the rule for PNG chunk types).
[[nodiscard]] bool isLetterCode(std::span<const std::byte> code) noexcept;

// The four bytes as text when they are printable ASCII (a chunk's FourCC in
// a detail), otherwise "(unprintable)".
[[nodiscard]] std::string fourCcText(std::span<const std::byte> code);

}  // namespace recovery::formats::detail
