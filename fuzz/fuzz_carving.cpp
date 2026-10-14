// Fuzz target: the carving framework over a source: one signature scan with
// every format, and every hit carved (end detection, validation, the skip
// rules), as a deep scan's pass does it.

#include "carving/file_carver.hpp"
#include "carving/signature_scanner.hpp"
#include "fuzz_support.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace recovery;
    fuzz::FuzzSource source(fuzz::bytesOf(data, size));
    fuzz::require(source.open().ok(), "opening the source");
    Result<carving::SignatureScanner> scanner = carving::SignatureScanner::create(fuzz::allFormats());
    fuzz::require(scanner.ok(), "making the scanner");
    carving::FileCarver carver(source);
    const std::uint64_t sourceSize = source.size();
    (void)carver.run(*scanner, [&](carving::FileCandidate&& carve) {
        fuzz::require(carve.sourceOffset <= sourceSize && carve.length <= sourceSize - carve.sourceOffset,
                      "a carve beyond the source");
        return success();
    });
    return 0;
}
