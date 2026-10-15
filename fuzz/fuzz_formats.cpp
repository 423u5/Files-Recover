// Fuzz target: every carving format (JPEG, PNG, WEBP, GIF, BMP, MP3, WAV,
// M4A, AAC, MP4, ZIP, PDF, OLE2) on the input as a file's content: the header check, end
// detection and structural validation, with the formats' documented limits
// checked (never more bytes than were given).

#include "carving/content_reader.hpp"
#include "fuzz_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace recovery;
    const std::span<const std::byte> bytes = fuzz::bytesOf(data, size);
    for (const auto& format : fuzz::allFormats().formats()) {
        const std::size_t header = std::min<std::size_t>(bytes.size(), format->descriptor().headerSize);
        (void)format->checkHeader(bytes.first(header));

        carving::MemoryContentReader content(bytes);
        const Result<carving::EndDetection> end = format->findEnd(content);
        if (end.ok()) {
            fuzz::require(end->length <= bytes.size(), "an end beyond the content");
        }
        carving::MemoryContentReader again(bytes);
        const Result<carving::ValidationResult> verdict = format->validator().validate(again);
        if (verdict.ok()) {
            fuzz::require(verdict->validBytes <= bytes.size(), "more valid bytes than the content");
        }
    }
    return 0;
}
