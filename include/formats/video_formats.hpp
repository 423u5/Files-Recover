#pragma once

// The video format module: MP4 carving (P12).
//
// The application adds the formats it wants to a carving::FormatRegistry,
// alongside the image (image_formats.hpp) and audio (audio_formats.hpp)
// formats or on its own. MP4 and M4A share the "ftyp" signature and divide
// the files between them (mp4_analysis.hpp's classify()), so both can be
// registered side by side.

#include "carving/format_registry.hpp"
#include "formats/mp4_format.hpp"
#include "recovery/result.hpp"

#include <memory>
#include <vector>

namespace recovery::formats {

// MP4.
[[nodiscard]] std::vector<std::shared_ptr<const carving::IFileFormat>> videoFormats(Mp4FormatOptions options = {});

// Adds videoFormats() to `registry`. Fails with InvalidInput, leaving the
// registry unchanged, when one of their ids is already registered.
[[nodiscard]] Status registerVideoFormats(carving::FormatRegistry& registry, Mp4FormatOptions options = {});

}  // namespace recovery::formats
