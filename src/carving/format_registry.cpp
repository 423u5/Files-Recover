#include "carving/format_registry.hpp"

#include <string>
#include <utility>

namespace recovery::carving {

Status FormatRegistry::add(std::shared_ptr<const IFileFormat> format) {
    if (format == nullptr) {
        return makeError(ErrorCode::InvalidInput, "cannot register a null format");
    }
    const FormatDescriptor& descriptor = format->descriptor();
    if (Status valid = validateDescriptor(descriptor); !valid.ok()) {
        return valid;
    }
    if (find(descriptor.id) != nullptr) {
        return makeError(ErrorCode::InvalidInput, "format '" + descriptor.id + "' is already registered");
    }
    formats_.push_back(std::move(format));
    return success();
}

const IFileFormat* FormatRegistry::find(std::string_view id) const noexcept {
    const std::optional<std::size_t> index = indexOf(id);
    return index.has_value() ? formats_[*index].get() : nullptr;
}

std::optional<std::size_t> FormatRegistry::indexOf(std::string_view id) const noexcept {
    for (std::size_t i = 0; i < formats_.size(); ++i) {
        if (formats_[i]->descriptor().id == id) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace recovery::carving
