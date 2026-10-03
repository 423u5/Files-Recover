#include "formats/video_formats.hpp"

#include <string>

namespace recovery::formats {

std::vector<std::shared_ptr<const carving::IFileFormat>> videoFormats(Mp4FormatOptions options) {
    return {std::make_shared<Mp4Format>(options)};
}

Status registerVideoFormats(carving::FormatRegistry& registry, Mp4FormatOptions options) {
    const std::vector<std::shared_ptr<const carving::IFileFormat>> formats = videoFormats(options);
    for (const auto& format : formats) {
        if (registry.find(format->descriptor().id) != nullptr) {
            return makeError(ErrorCode::InvalidInput,
                             "format '" + format->descriptor().id + "' is already registered");
        }
    }
    for (const auto& format : formats) {
        if (Status added = registry.add(format); !added.ok()) {
            return added;
        }
    }
    return success();
}

}  // namespace recovery::formats
