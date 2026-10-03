#pragma once

// The image format module: JPEG, PNG, WEBP, GIF and BMP carving.
//
// The application adds the formats it wants to a carving::FormatRegistry.
// This is the only place that knows the image formats; the scanner and the
// carver only see carving::IFileFormat.

#include "carving/format_registry.hpp"
#include "recovery/result.hpp"

#include <memory>
#include <vector>

namespace recovery::formats {

// JPEG, PNG, WEBP, GIF and BMP, in that order (the plan's priority; hits at
// the same offset are reported in registration order).
[[nodiscard]] std::vector<std::shared_ptr<const carving::IFileFormat>> imageFormats();

// Adds imageFormats() to `registry`. Fails with InvalidInput, leaving the
// registry unchanged, when one of their ids is already registered.
[[nodiscard]] Status registerImageFormats(carving::FormatRegistry& registry);

}  // namespace recovery::formats
