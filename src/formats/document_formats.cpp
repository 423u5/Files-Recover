#include "formats/document_formats.hpp"

#include <string>
#include <utility>

namespace recovery::formats {

Result<std::vector<std::shared_ptr<const carving::IFileFormat>>> documentFormats(
    std::shared_ptr<const IDeflateDecoder> decoder) {
    Result<std::shared_ptr<const PdfFormat>> pdf = PdfFormat::make(std::move(decoder));
    if (!pdf.ok()) {
        return pdf.error();
    }
    Result<std::shared_ptr<const Ole2Format>> ole2 = Ole2Format::make();
    if (!ole2.ok()) {
        return ole2.error();
    }
    return std::vector<std::shared_ptr<const carving::IFileFormat>>{*pdf, *ole2};
}

Status registerDocumentFormats(carving::FormatRegistry& registry, std::shared_ptr<const IDeflateDecoder> decoder) {
    Result<std::vector<std::shared_ptr<const carving::IFileFormat>>> formats = documentFormats(std::move(decoder));
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
