// Fuzz target: the validation levels and the engine's media decoders (JPEG
// Huffman scans, PNG inflate, GIF LZW, BMP RLE, VP8L and VP8 headers, ADPCM,
// AAC raw data blocks and ADTS CRCs, AVC and HEVC parameter sets and slice
// headers) on the input as a file of every format whose header accepts it.

#include "carving/content_reader.hpp"
#include "fuzz_support.hpp"
#include "validation/validation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace recovery;
    const std::span<const std::byte> bytes = fuzz::bytesOf(data, size);
    validation::ValidationOptions options;
    options.media = true;
    for (const auto& format : fuzz::allFormats().formats()) {
        const std::size_t header = std::min<std::size_t>(bytes.size(), format->descriptor().headerSize);
        if (!format->checkHeader(bytes.first(header)).plausible) {
            continue;
        }
        carving::MemoryContentReader content(bytes);
        (void)validation::validateContent(content, format.get(), fuzz::allMedia(), options);
    }
    return 0;
}
