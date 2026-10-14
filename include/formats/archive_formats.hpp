#pragma once

// The archive format module: ZIP carving (and the formats built on ZIP).
//
// The application adds the formats it wants to a carving::FormatRegistry.
// The ZIP format decodes DEFLATE through a decoder it is given: the engine's
// is validation::deflateDecoder().

#include "carving/format_registry.hpp"
#include "formats/zip_format.hpp"
#include "recovery/result.hpp"

#include <memory>
#include <vector>

namespace recovery::formats {

// ZIP. InvalidInput when `decoder` is null.
[[nodiscard]] Result<std::vector<std::shared_ptr<const carving::IFileFormat>>> archiveFormats(
    std::shared_ptr<const IDeflateDecoder> decoder);

// Adds archiveFormats(decoder) to `registry`. Fails with InvalidInput,
// leaving the registry unchanged, when `decoder` is null or one of their ids
// is already registered.
[[nodiscard]] Status registerArchiveFormats(carving::FormatRegistry& registry,
                                            std::shared_ptr<const IDeflateDecoder> decoder);

}  // namespace recovery::formats
