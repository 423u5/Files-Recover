#pragma once

// The engine's DEFLATE decoder for the ZIP format (formats::IDeflateDecoder):
// the inflater that checks the image data of PNG files (inflate.hpp), on
// raw DEFLATE data. Register the archive formats with it:
//
//   formats::registerArchiveFormats(registry, validation::deflateDecoder());

#include "formats/zip_format.hpp"

#include <memory>

namespace recovery::validation {

// One shared decoder: it holds no state between calls.
[[nodiscard]] std::shared_ptr<const formats::IDeflateDecoder> deflateDecoder();

}  // namespace recovery::validation
