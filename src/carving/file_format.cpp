#include "carving/file_format.hpp"

#include <algorithm>
#include <string>

namespace recovery::carving {

namespace {

bool isIdCharacter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool isExtensionCharacter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

}  // namespace

std::string_view toString(EndDetectionMethod method) noexcept {
    switch (method) {
    case EndDetectionMethod::SizeField:
        return "SizeField";
    case EndDetectionMethod::StructureWalk:
        return "StructureWalk";
    case EndDetectionMethod::EndMarker:
        return "EndMarker";
    case EndDetectionMethod::None:
        return "None";
    }
    return "Unknown";
}

std::string_view toString(ExtractionStrategy strategy) noexcept {
    switch (strategy) {
    case ExtractionStrategy::Contiguous:
        return "Contiguous";
    }
    return "Unknown";
}

std::string_view toString(EndStatus status) noexcept {
    switch (status) {
    case EndStatus::Found:
        return "Found";
    case EndStatus::Truncated:
        return "Truncated";
    case EndStatus::Broken:
        return "Broken";
    case EndStatus::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

Status validateDescriptor(const FormatDescriptor& descriptor) {
    const auto invalid = [&](const std::string& what) {
        return makeError(ErrorCode::InvalidInput, "format '" + descriptor.id + "': " + what);
    };
    if (descriptor.id.empty() || descriptor.id.size() > FormatDescriptor::kMaxIdLength ||
        !std::all_of(descriptor.id.begin(), descriptor.id.end(), isIdCharacter)) {
        return invalid("the id must have 1 to " + std::to_string(FormatDescriptor::kMaxIdLength) +
                       " characters from a-z, 0-9, '-' and '_'");
    }
    if (descriptor.name.empty()) {
        return invalid("no name");
    }
    if (descriptor.extension.empty() || descriptor.extension.size() > FormatDescriptor::kMaxExtensionLength ||
        !std::all_of(descriptor.extension.begin(), descriptor.extension.end(), isExtensionCharacter)) {
        return invalid("the extension must have 1 to " + std::to_string(FormatDescriptor::kMaxExtensionLength) +
                       " characters from a-z and 0-9");
    }
    if (descriptor.signatures.empty() || descriptor.signatures.size() > FormatDescriptor::kMaxSignatures) {
        return invalid("a format needs 1 to " + std::to_string(FormatDescriptor::kMaxSignatures) + " signatures");
    }
    std::size_t reach = 0;
    for (const FileSignature& signature : descriptor.signatures) {
        if (Status valid = validateSignature(signature); !valid.ok()) {
            return invalid(valid.error().message);
        }
        reach = std::max(reach, signature.reach());
    }
    if (descriptor.minimumSize < reach) {
        return invalid("the minimum size (" + std::to_string(descriptor.minimumSize) +
                       ") is smaller than a signature's reach (" + std::to_string(reach) + ")");
    }
    if (descriptor.maximumSize < descriptor.minimumSize || descriptor.maximumSize > FormatDescriptor::kMaxMaximumSize) {
        return invalid("the maximum size must lie between the minimum size and " +
                       std::to_string(FormatDescriptor::kMaxMaximumSize));
    }
    if (descriptor.headerSize < reach || descriptor.headerSize > FormatDescriptor::kMaxHeaderSize) {
        return invalid("the header size must lie between the longest signature reach (" + std::to_string(reach) +
                       ") and " + std::to_string(FormatDescriptor::kMaxHeaderSize));
    }
    switch (descriptor.endDetection) {
    case EndDetectionMethod::SizeField:
    case EndDetectionMethod::StructureWalk:
    case EndDetectionMethod::EndMarker:
    case EndDetectionMethod::None:
        break;
    default:
        return invalid("unknown end detection method");
    }
    switch (descriptor.extraction) {
    case ExtractionStrategy::Contiguous:
        break;
    default:
        return invalid("unknown extraction strategy");
    }
    return success();
}

}  // namespace recovery::carving
