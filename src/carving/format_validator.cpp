#include "carving/format_validator.hpp"

namespace recovery::carving {

std::string_view toString(ValidationStatus status) noexcept {
    switch (status) {
    case ValidationStatus::NotValidated:
        return "NotValidated";
    case ValidationStatus::Valid:
        return "Valid";
    case ValidationStatus::Truncated:
        return "Truncated";
    case ValidationStatus::Invalid:
        return "Invalid";
    }
    return "Unknown";
}

}  // namespace recovery::carving
