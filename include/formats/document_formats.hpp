#pragma once

// The document format module: PDF and OLE2 compound files (the Office
// 97-2003 documents, Outlook messages, Windows Installer packages). The
// documents built on ZIP (DOCX, XLSX, PPTX, ODT, EPUB) are the archive
// module's (archive_formats.hpp).
//
// The application adds the formats it wants to a carving::FormatRegistry.
// The PDF format decodes FlateDecode streams through a decoder it is given:
// the engine's is validation::deflateDecoder().

#include "carving/format_registry.hpp"
#include "formats/deflate_decoder.hpp"
#include "formats/ole2_format.hpp"
#include "formats/pdf_format.hpp"
#include "recovery/result.hpp"

#include <memory>
#include <vector>

namespace recovery::formats {

// PDF and OLE2, in this order. InvalidInput when `decoder` is null.
[[nodiscard]] Result<std::vector<std::shared_ptr<const carving::IFileFormat>>> documentFormats(
    std::shared_ptr<const IDeflateDecoder> decoder);

// Adds documentFormats(decoder) to `registry`. Fails with InvalidInput,
// leaving the registry unchanged, when `decoder` is null or one of their ids
// is already registered.
[[nodiscard]] Status registerDocumentFormats(carving::FormatRegistry& registry,
                                             std::shared_ptr<const IDeflateDecoder> decoder);

}  // namespace recovery::formats
