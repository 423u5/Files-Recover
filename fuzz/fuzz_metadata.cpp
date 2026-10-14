// Fuzz target: media metadata for a user interface (Exif, PNG and GIF
// structure, ID3 and other tags, RIFF INFO, MP4 movies, tracks and user data)
// and the previews it offers, on the input as a file of every format whose
// header accepts it.

#include "carving/content_reader.hpp"
#include "fuzz_support.hpp"
#include "metadata/media_metadata.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace recovery;
    const std::span<const std::byte> bytes = fuzz::bytesOf(data, size);
    for (const auto& format : fuzz::allFormats().formats()) {
        const std::size_t header = std::min<std::size_t>(bytes.size(), format->descriptor().headerSize);
        if (!format->checkHeader(bytes.first(header)).plausible) {
            continue;
        }
        carving::MemoryContentReader content(bytes);
        const Result<metadata::MediaMetadata> found = metadata::extractMetadata(content, format->descriptor().id);
        if (!found.ok()) {
            continue;
        }
        for (const metadata::PreviewSource& preview : found->previews) {
            const Result<std::vector<std::byte>> read = metadata::readPreview(content, preview, 1024 * 1024);
            if (read.ok()) {
                fuzz::require(read->size() <= bytes.size(), "a preview larger than the file");
            }
        }
    }
    return 0;
}
