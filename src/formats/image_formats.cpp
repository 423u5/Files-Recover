#include "formats/image_formats.hpp"

#include "formats/bmp_format.hpp"
#include "formats/gif_format.hpp"
#include "formats/jpeg_format.hpp"
#include "formats/png_format.hpp"
#include "formats/webp_format.hpp"

#include <string>

namespace recovery::formats {

std::vector<std::shared_ptr<const carving::IFileFormat>> imageFormats() {
    return {std::make_shared<JpegFormat>(), std::make_shared<PngFormat>(), std::make_shared<WebpFormat>(),
            std::make_shared<GifFormat>(), std::make_shared<BmpFormat>()};
}

Status registerImageFormats(carving::FormatRegistry& registry) {
    const std::vector<std::shared_ptr<const carving::IFileFormat>> formats = imageFormats();
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
