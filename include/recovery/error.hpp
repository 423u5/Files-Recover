#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace recovery {

// Error categories shared by every engine module. Expected failures (I/O
// errors, malformed on-disk data, cancellation) are reported through these
// codes, never through exceptions.
enum class ErrorCode : std::uint8_t {
    InvalidInput,
    IoError,
    UnreadableRegion,
    CorruptedFilesystem,
    UnsupportedFilesystem,
    InvalidFormat,
    PartialRecovery,
    DestinationError,
    Cancelled,
    InternalError,
};

[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::InternalError;
    std::string message;
    // Operating-system error (Win32 GetLastError() value); 0 when not applicable.
    std::uint32_t systemErrorCode = 0;
};

[[nodiscard]] Error makeError(ErrorCode code, std::string message, std::uint32_t systemErrorCode = 0);

// Human-readable single-line description, e.g. "IO_ERROR: read failed (system error 23)".
[[nodiscard]] std::string describe(const Error& error);

}  // namespace recovery
