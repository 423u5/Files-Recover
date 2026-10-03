#include "formats/audio_formats.hpp"

#include "formats/aac_format.hpp"
#include "formats/m4a_format.hpp"
#include "formats/mp3_format.hpp"
#include "formats/wav_format.hpp"

#include <string>

namespace recovery::formats {

std::vector<std::shared_ptr<const carving::IFileFormat>> audioFormats() {
    return {std::make_shared<Mp3Format>(), std::make_shared<WavFormat>(), std::make_shared<M4aFormat>(),
            std::make_shared<AacFormat>()};
}

Status registerAudioFormats(carving::FormatRegistry& registry) {
    const std::vector<std::shared_ptr<const carving::IFileFormat>> formats = audioFormats();
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
