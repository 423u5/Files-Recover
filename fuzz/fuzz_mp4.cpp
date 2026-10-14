// Fuzz target: the MP4 (ISO base media) parser and analysis on the input as a
// file: boxes, the movie and its tracks, sample tables, movie fragments, chunk
// placement, moov search, brand classification and sample framing.

#include "carving/content_reader.hpp"
#include "formats/mp4_analysis.hpp"
#include "formats/mp4_parser.hpp"
#include "fuzz_support.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace recovery;
    namespace mp4 = formats::mp4;
    const std::span<const std::byte> bytes = fuzz::bytesOf(data, size);
    carving::MemoryContentReader content(bytes);
    const Result<mp4::Mp4File> file = mp4::parseFile(content);
    if (file.ok()) {
        (void)mp4::classify(file->fileType, file->movie.has_value() ? &*file->movie : nullptr);
        if (file->movie.has_value()) {
            (void)mp4::mediaExtent(*file->movie);
            (void)mp4::checkSampleFraming(content, *file->movie);
        }
    }
    (void)mp4::findMovie(content, 0, bytes.size());
    return 0;
}
