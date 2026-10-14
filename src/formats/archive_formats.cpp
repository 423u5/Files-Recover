#include "formats/archive_formats.hpp"

#include <string>
#include <utility>

namespace recovery::formats {

Result<std::vector<std::shared_ptr<const carving::IFileFormat>>> archiveFormats(
    std::shared_ptr<const IDeflateDecoder> decoder) {
    Result<std::shared_ptr<const ZipFormat>> zip = ZipFormat::make(std::move(decoder));
    if (!zip.ok()) {
        return zip.error();
    }
    return std::vector<std::shared_ptr<const carving::IFileFormat>>{*zip};
}

Status registerArchiveFormats(carving::FormatRegistry& registry, std::shared_ptr<const IDeflateDecoder> decoder) {
    Result<std::vector<std::shared_ptr<const carving::IFileFormat>>> formats = archiveFormats(std::move(decoder));
    if (!formats.ok()) {
        return formats.error();
    }
    for (const auto& format : *formats) {
        if (registry.find(format->descriptor().id) != nullptr) {
            return makeError(ErrorCode::InvalidInput,
                             "format '" + format->descriptor().id + "' is already registered");
        }
    }
    for (const auto& format : *formats) {
        if (Status added = registry.add(format); !added.ok()) {
            return added;
        }
    }
    return success();
}

}  // namespace recovery::formats
