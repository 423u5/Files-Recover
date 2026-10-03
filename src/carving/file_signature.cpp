#include "carving/file_signature.hpp"

#include <string>
#include <utility>

namespace recovery::carving {

std::size_t FileSignature::reach() const noexcept {
    return static_cast<std::size_t>(offset) + pattern.size();
}

bool FileSignature::matches(std::span<const std::byte> data) const noexcept {
    if (data.size() < pattern.size()) {
        return false;
    }
    if (mask.empty()) {
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            if (data[i] != pattern[i]) {
                return false;
            }
        }
        return true;
    }
    if (mask.size() != pattern.size()) {
        return false;
    }
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if ((data[i] & mask[i]) != (pattern[i] & mask[i])) {
            return false;
        }
    }
    return true;
}

Status validateSignature(const FileSignature& signature) {
    const auto invalid = [&](const std::string& what) {
        return makeError(ErrorCode::InvalidInput, "signature '" + signature.name + "': " + what);
    };
    if (signature.name.empty()) {
        return makeError(ErrorCode::InvalidInput, "signature has no name");
    }
    if (signature.pattern.size() < FileSignature::kMinLength || signature.pattern.size() > FileSignature::kMaxLength) {
        return invalid("pattern must have " + std::to_string(FileSignature::kMinLength) + " to " +
                       std::to_string(FileSignature::kMaxLength) + " bytes");
    }
    if (!signature.mask.empty()) {
        if (signature.mask.size() != signature.pattern.size()) {
            return invalid("mask and pattern differ in length");
        }
        if (signature.mask.front() != std::byte{0xFF}) {
            return invalid("the first pattern byte must be compared in full (mask 0xFF)");
        }
    }
    if (signature.offset > FileSignature::kMaxOffset) {
        return invalid("offset exceeds " + std::to_string(FileSignature::kMaxOffset));
    }
    return success();
}

FileSignature byteSignature(std::string name, std::initializer_list<std::uint8_t> pattern, std::uint32_t offset,
                            std::initializer_list<std::uint8_t> mask) {
    FileSignature signature;
    signature.name = std::move(name);
    signature.offset = offset;
    for (const std::uint8_t byte : pattern) {
        signature.pattern.push_back(static_cast<std::byte>(byte));
    }
    for (const std::uint8_t byte : mask) {
        signature.mask.push_back(static_cast<std::byte>(byte));
    }
    return signature;
}

FileSignature textSignature(std::string name, std::string_view text, std::uint32_t offset) {
    FileSignature signature;
    signature.name = std::move(name);
    signature.offset = offset;
    for (const char c : text) {
        signature.pattern.push_back(static_cast<std::byte>(c));
    }
    return signature;
}

}  // namespace recovery::carving
