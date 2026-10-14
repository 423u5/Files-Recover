// Fuzz target: an image's metadata file (<image>.imgmeta), read as a scan of
// that image reads it: whatever parses must serialize, and parse again to the
// same metadata.

#include "fuzz_support.hpp"
#include "imaging/image_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace recovery;
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const Result<imaging::ImageMetadata> parsed = imaging::parseImageMetadata(text);
    if (!parsed.ok()) {
        return 0;
    }
    const Result<std::string> written = imaging::serializeImageMetadata(*parsed);
    fuzz::require(written.ok(), "metadata that parses does not serialize");
    const Result<imaging::ImageMetadata> again = imaging::parseImageMetadata(*written);
    fuzz::require(again.ok(), "serialized metadata does not parse");
    fuzz::require(again->badRegions == parsed->badRegions && again->bytesCompleted == parsed->bytesCompleted &&
                      again->sectorSize == parsed->sectorSize && again->sourceSize == parsed->sourceSize,
                  "metadata changes when written and read again");
    return 0;
}
