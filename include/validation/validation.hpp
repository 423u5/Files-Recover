#pragma once

// Validation levels (P14): what the engine checked about a file's content,
// one level at a time, so that a file is never taken as recovered just
// because its header, its extension or its name looks right.
//
//   structural   the format's own structure: the carving validators of
//                P8-P12 (lengths, checksums, element order, sample tables)
//   media        the coded media, decoded by the engine's own deterministic
//                decoders (media_validator.hpp): Huffman, deflate, LZW, RLE
//                and VP8L data in full; the headers of lossy codecs as far as
//                they can be checked without the codecs' tables
//   playability  a platform decoder decodes the whole file
//                (playability.hpp; on Windows WIC and Media Foundation,
//                windows_playability.hpp); optional, off by default
//
// Each level ends in a LevelStatus with a detail that says what was checked,
// or what failed and where. Nothing here is a score: a status is a fact about
// one check, and the overall status follows from the levels by fixed rules.

#include "carving/content_reader.hpp"
#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "formats/mp4_parser.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace recovery::validation {

class MediaValidatorRegistry;
class IPlayabilityChecker;

enum class ValidationLevel : std::uint8_t {
    Structural,
    Media,
    Playability,
};

inline constexpr std::size_t kValidationLevelCount = 3;

[[nodiscard]] std::string_view toString(ValidationLevel level) noexcept;

enum class LevelStatus : std::uint8_t {
    // The level did not run: it was not asked for, or an earlier level failed
    // (the detail says which).
    NotRun,
    // Checked, and consistent.
    Passed,
    // Consistent as far as it goes, but the content ends before the file does.
    Truncated,
    // Inconsistent: the detail says what and where.
    Failed,
    // Nothing is coded at this level: the media are stored as they are
    // (uncompressed pixels, PCM samples), so there is nothing to decode.
    NotApplicable,
    // This level cannot check this content: no checker for its format, codec
    // or coding feature (arithmetic-coded JPEG, MP3 audio), a limit, or no
    // platform decoder.
    Unsupported,
};

[[nodiscard]] std::string_view toString(LevelStatus status) noexcept;

enum class Coverage : std::uint8_t {
    // Every coded element was checked.
    Full,
    // Only part of the coded data could be checked: headers, parameter sets
    // and what can be parsed without the codec's tables (lossy WebP, AAC,
    // AVC, HEVC). The detail says how far.
    Partial,
};

[[nodiscard]] std::string_view toString(Coverage coverage) noexcept;

struct LevelResult {
    LevelStatus status = LevelStatus::NotRun;
    // What ran: "jpeg structure", "jpeg decoder", "Windows Imaging Component", ...
    std::string checker;
    // Media: how much of the coded data was checked.
    Coverage coverage = Coverage::Full;
    // Failed and Truncated: the content offset of the problem, when known.
    std::optional<std::uint64_t> offset;
    // What was checked, or what failed and where. Never file content.
    std::string detail;

    [[nodiscard]] bool passed() const noexcept { return status == LevelStatus::Passed; }
};

// The levels of one file's content.
struct ValidationState {
    LevelResult structural;
    LevelResult media;
    LevelResult playability;

    [[nodiscard]] const LevelResult& level(ValidationLevel which) const noexcept;
    [[nodiscard]] LevelResult& level(ValidationLevel which) noexcept;

    // All levels together, first match wins: Invalid when a level failed;
    // Truncated when a level found the content cut short; Valid when the
    // structure passed; NotValidated otherwise (no format, nothing checked).
    // Media and playability that are NotRun, NotApplicable or Unsupported
    // leave a valid structure Valid.
    [[nodiscard]] carving::ValidationStatus status() const noexcept;
    // The deepest level that passed, if any.
    [[nodiscard]] std::optional<ValidationLevel> deepestPassed() const noexcept;
};

// Bounds on the work and memory of the media decoders. A file that needs more
// is reported Unsupported at the media level, with what it would need.
struct MediaLimits {
    // Memory one decoder may hold: JPEG progressive coefficient state, VP8L
    // sub-images and prefix codes, the positions of PNG image data.
    std::uint64_t maxMemory = 256 * kMiB;
    // Work one decoder may do, in bytes of decoded output: PNG scanlines,
    // GIF and VP8L pixels, BMP pixels, 64 per JPEG block a scan visits. A
    // file whose headers ask for more is Unsupported before anything is
    // decoded (a small file can claim a huge image).
    std::uint64_t maxDecodedBytes = 4 * kGiB;
    // Parse limits for MP4 and M4A files.
    formats::mp4::ParseLimits mp4;
};

// InvalidInput when a limit is 0.
[[nodiscard]] Status validate(const MediaLimits& limits);

struct ValidationOptions {
    // Run the media level (the engine's own decoders).
    bool media = true;
    // Run the playability level with this checker; none: it does not run.
    // Not owned: it must outlive every validation that uses these options.
    IPlayabilityChecker* playability = nullptr;
    MediaLimits limits;
};

// The structural level from a carving verdict (Valid: Passed; Truncated and
// Invalid: the problem at validBytes; NotValidated: NotRun).
[[nodiscard]] LevelResult structuralLevel(const carving::ValidationResult& result, std::string_view formatId);

// Validates the file whose bytes are all of `content`, of the format `format`
// (nullptr: no known format), at the levels `options` asks for, in order:
//
//  * structural: `format`'s validator, or `knownStructure` when the caller
//    holds that validator's verdict on exactly these bytes already. No
//    format: Unsupported, and nothing else runs.
//  * media: `media`'s validator for the format (Unsupported when there is
//    none). Not run when the structure failed.
//  * playability: options.playability's checker. Run only when the
//    structure passed and the media neither failed nor found the content
//    cut short: a platform decoder adds nothing about a file known to be
//    damaged or truncated.
//
// Problems of the content are results, never errors. Fails only with the
// reader's errors (Cancelled, source failures), InvalidInput for invalid
// limits, and the playability checker's errors.
[[nodiscard]] Result<ValidationState> validateContent(
    carving::IContentReader& content, const carving::IFileFormat* format, const MediaValidatorRegistry& media,
    const ValidationOptions& options, const std::optional<carving::ValidationResult>& knownStructure = std::nullopt);

}  // namespace recovery::validation
