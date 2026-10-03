#pragma once

// Structural validation of one file format. A validator checks the actual
// structure of a file's bytes (sizes, checksums, element order), so that a
// candidate is never taken as recovered just because its header looks right.
//
// Validators only see the file's content (IContentReader), not where it came
// from, so the same validator can check carved files and, later, files found
// through filesystem metadata.

#include "carving/content_reader.hpp"
#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace recovery::carving {

enum class ValidationStatus : std::uint8_t {
    // No validator ran (validation was turned off).
    NotValidated,
    // Every element of the structure is consistent, and the structure ends
    // where the content ends.
    Valid,
    // Consistent as far as it goes, but the content ends before the
    // structure does (a file cut short).
    Truncated,
    // Inconsistent: a wrong checksum, an impossible field, elements out of order.
    Invalid,
};

inline constexpr std::size_t kValidationStatusCount = 4;

[[nodiscard]] std::string_view toString(ValidationStatus status) noexcept;

struct ValidationResult {
    ValidationStatus status = ValidationStatus::NotValidated;
    // Bytes from the start of the file that are consistent with the format.
    std::uint64_t validBytes = 0;
    // What was checked, or what failed and where. Never file content.
    std::string detail;
};

// Thread safety: validate() is const and must be safe to call concurrently
// (validators hold no mutable state).
class FormatValidator {
public:
    virtual ~FormatValidator() = default;

    // Validates the file whose bytes are all of `content` (content.size()
    // bytes). Structural problems are results (Invalid, Truncated), not
    // errors; errors are only the reader's (cancellation, source failures).
    [[nodiscard]] virtual Result<ValidationResult> validate(IContentReader& content) const = 0;

protected:
    FormatValidator() = default;
    FormatValidator(const FormatValidator&) = default;
    FormatValidator& operator=(const FormatValidator&) = default;
    FormatValidator(FormatValidator&&) = default;
    FormatValidator& operator=(FormatValidator&&) = default;
};

}  // namespace recovery::carving
