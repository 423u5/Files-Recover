#pragma once

// The audio format module: MP3, WAV, M4A and raw AAC (ADTS) carving.
//
// The application adds the formats it wants to a carving::FormatRegistry,
// alongside the image formats (image_formats.hpp) or on their own. This is
// the only place that knows the audio formats; the scanner and the carver
// only see carving::IFileFormat.

#include "carving/format_registry.hpp"
#include "recovery/result.hpp"

#include <memory>
#include <vector>

namespace recovery::formats {

// MP3, WAV, M4A and AAC, in that order (the plan's priority; hits at the same
// offset are reported in registration order).
[[nodiscard]] std::vector<std::shared_ptr<const carving::IFileFormat>> audioFormats();

// Adds audioFormats() to `registry`. Fails with InvalidInput, leaving the
// registry unchanged, when one of their ids is already registered.
[[nodiscard]] Status registerAudioFormats(carving::FormatRegistry& registry);

}  // namespace recovery::formats
