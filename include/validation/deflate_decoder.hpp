#pragma once

// The engine's DEFLATE decoder for the ZIP and PDF formats
// (formats::IDeflateDecoder): the inflater that checks the image data of PNG
// files (inflate.hpp), on raw DEFLATE data and on zlib streams. Register the
// archive and document formats with it:
//
//   formats::registerArchiveFormats(registry, validation::deflateDecoder());
//   formats::registerDocumentFormats(registry, validation::deflateDecoder());

#include "formats/deflate_decoder.hpp"

#include <memory>

namespace recovery::validation {

// One shared decoder: it holds no state between calls.
[[nodiscard]] std::shared_ptr<const formats::IDeflateDecoder> deflateDecoder();

}  // namespace recovery::validation
