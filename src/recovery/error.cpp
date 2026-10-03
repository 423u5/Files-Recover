#include "recovery/error.hpp"

#include <utility>

namespace recovery {

std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidInput:
        return "INVALID_INPUT";
    case ErrorCode::IoError:
        return "IO_ERROR";
    case ErrorCode::UnreadableRegion:
        return "UNREADABLE_REGION";
    case ErrorCode::CorruptedFilesystem:
        return "CORRUPTED_FILESYSTEM";
    case ErrorCode::UnsupportedFilesystem:
        return "UNSUPPORTED_FILESYSTEM";
    case ErrorCode::InvalidFormat:
        return "INVALID_FORMAT";
    case ErrorCode::PartialRecovery:
        return "PARTIAL_RECOVERY";
    case ErrorCode::DestinationError:
        return "DESTINATION_ERROR";
    case ErrorCode::Cancelled:
        return "CANCELLED";
    case ErrorCode::InternalError:
        return "INTERNAL_ERROR";
    }
    return "UNKNOWN_ERROR";
}

Error makeError(ErrorCode code, std::string message, std::uint32_t systemErrorCode) {
    return Error{code, std::move(message), systemErrorCode};
}

std::string describe(const Error& error) {
    std::string text(toString(error.code));
    if (!error.message.empty()) {
        text += ": ";
        text += error.message;
    }
    if (error.systemErrorCode != 0) {
        text += " (system error ";
        text += std::to_string(error.systemErrorCode);
        text += ')';
    }
    return text;
}

}  // namespace recovery
